// NVMe: SSDs on PCI Express (PCI class 01:08:02), following the NVM Express
// Base Specification 2.0 and the NVM Command Set Specification.
//
// At probe time the driver resets the controller (CC.EN off, wait for
// CSTS.RDY to clear), gives it an admin submission and completion queue,
// turns it on, and asks it, with IDENTIFY, what it is and which namespaces
// it has. Then it creates one I/O queue pair, and every namespace formatted
// without metadata becomes a block device, "nvme0n1" on (Linux's names).
//
// Commands go one at a time per controller: READ and WRITE (the NVM command
// set's) move data through a 128 KiB DMA buffer that data is copied through,
// described to the controller with PRP entries: the first page in PRP1, and
// when more than two pages, the rest in a PRP list that PRP2 points to. A
// command is never larger than the controller's MDTS.
//
// Completion is interrupt driven: both completion queues raise MSI-X table
// entry 0 (or a single MSI, if the controller has no MSI-X), and the handler
// wakes the thread waiting for that controller, which reads the completion
// queue entry itself (its phase tag says when the entry is new). A command
// that hasn't finished after COMMAND_TIMEOUT_MS fails with EIO, and the
// controller is not used again: it may still write into the DMA buffer. A
// controller with neither MSI-X nor MSI is polled.

#include <block.h>
#include <console.h>
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <memory.h>
#include <pci.h>
#include <pmm.h>
#include <sched.h>
#include <stdarg.h>
#include <string.h>
#include <sync.h>
#include <time.h>

// Controller registers (offsets in BAR 0).
#define NVME_CAP   0x00 // 64 bits
#define NVME_VS    0x08
#define NVME_CC    0x14
#define NVME_CSTS  0x1c
#define NVME_AQA   0x24
#define NVME_ASQ   0x28 // 64 bits
#define NVME_ACQ   0x30 // 64 bits
#define NVME_DOORBELLS 0x1000

#define CAP_MQES(c)   ((uint32_t)((c) & 0xffff) + 1) // largest I/O queue, in entries
#define CAP_TO(c)     ((uint32_t)((c) >> 24 & 0xff)) // worst-case ready time, in 500 ms units
#define CAP_DSTRD(c)  ((uint32_t)((c) >> 32 & 0xf))  // doorbell stride: 4 << DSTRD bytes
#define CAP_CSS_NVM   (1ull << 37)                   // the NVM command set
#define CAP_MPSMIN(c) ((uint32_t)((c) >> 48 & 0xf))  // smallest memory page: 4 KiB << MPSMIN

#define CC_EN            (1u << 0)
#define CC_CSS_NVM       (0u << 4)
#define CC_MPS_4K        (0u << 7)
#define CC_AMS_RR        (0u << 11)
#define CC_IOSQES_64     (6u << 16) // log2 of a submission queue entry
#define CC_IOCQES_16     (4u << 20) // log2 of a completion queue entry

#define CSTS_RDY (1u << 0)
#define CSTS_CFS (1u << 1) // controller fatal status

// Admin commands.
#define ADMIN_CREATE_SQ    0x01
#define ADMIN_CREATE_CQ    0x05
#define ADMIN_IDENTIFY     0x06
#define ADMIN_SET_FEATURES 0x09

#define IDENTIFY_NAMESPACE   0x00
#define IDENTIFY_CONTROLLER  0x01
#define IDENTIFY_ACTIVE_NSID 0x02 // NVMe 1.1 and later

#define FEATURE_NUM_QUEUES 0x07

#define QUEUE_PHYS_CONTIG (1u << 0) // Create I/O SQ/CQ: the queue is one contiguous run
#define CQ_IRQ_ENABLED    (1u << 1)

// NVM commands.
#define NVM_WRITE 0x01
#define NVM_READ  0x02

// A submission queue entry.
struct nvme_sqe {
    uint8_t opcode;
    uint8_t flags; // fused operation, PRP or SGL: 0, PRPs
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

// A completion queue entry.
struct nvme_cqe {
    uint32_t result; // command specific
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status; // phase tag in bit 0, then the status field: SC bits 8:1, SCT 11:9, DNR 15
};

_Static_assert(sizeof(struct nvme_sqe) == 64, "submission queue entries are 64 bytes");
_Static_assert(sizeof(struct nvme_cqe) == 16, "completion queue entries are 16 bytes");

#define STATUS_PHASE     1u
#define STATUS_SC(s)     ((s) >> 1 & 0xff)
#define STATUS_SCT(s)    ((s) >> 9 & 0x7)

#define ADMIN_QUEUE_DEPTH 64
#define IO_QUEUE_DEPTH    64 // or CAP.MQES, if smaller
#define IO_QUEUE_ID       1

// Each controller's DMA memory, page by page: the four queues, the PRP
// list, then the data buffer.
#define DMA_ADMIN_SQ 0
#define DMA_ADMIN_CQ 1
#define DMA_IO_SQ    2
#define DMA_IO_CQ    3
#define DMA_PRP_LIST 4
#define DMA_DATA     5
#define DATA_PAGES   32
#define DATA_BYTES   (DATA_PAGES * PAGE_SIZE)
#define DMA_PAGES    (DMA_DATA + DATA_PAGES)

#define MAX_CONTROLLERS 8
#define MAX_NAMESPACES  16 // per controller

#define COMMAND_TIMEOUT_MS 5000

struct nvme_queue {
    uint16_t id;
    uint16_t depth;
    volatile struct nvme_sqe *sq;
    volatile struct nvme_cqe *cq;
    uint64_t sq_phys, cq_phys;
    uint16_t sq_tail, cq_head;
    uint16_t phase; // what the phase tag of a new completion entry is
    volatile uint32_t *sq_doorbell, *cq_doorbell;
};

struct nvme_controller;

struct nvme_namespace {
    struct nvme_controller *ctrl;
    uint32_t nsid;
    struct block_device disk;
};

struct nvme_controller {
    struct pci_dev *pci;
    volatile uint32_t *regs;
    uint64_t cap;
    unsigned index; // nvme0, nvme1, ...
    struct nvme_queue admin, io;
    uint64_t dma_phys;
    uint64_t *prp_list; // the data buffer's pages after the first
    uint64_t data_phys;
    uint8_t *data;
    uint32_t max_transfer; // bytes per command
    uint16_t next_cid;
    struct mutex lock; // one command at a time, and the data buffer
    bool dead;         // a command timed out: the controller may still be using its DMA memory
    bool use_irq;
    int vector;
    struct spinlock irq_lock; // irq_count and waiters
    uint64_t irq_count;       // interrupts that came in
    struct wait_queue waiters;
};

static struct nvme_controller *controllers[MAX_CONTROLLERS];
static size_t controller_count;

static uint32_t reg_read(struct nvme_controller *c, unsigned reg) { return c->regs[reg / 4]; }
static void reg_write(struct nvme_controller *c, unsigned reg, uint32_t v) { c->regs[reg / 4] = v; }

static uint64_t reg_read64(struct nvme_controller *c, unsigned reg) {
    return (uint64_t)reg_read(c, reg) | (uint64_t)reg_read(c, reg + 4) << 32;
}

static void reg_write64(struct nvme_controller *c, unsigned reg, uint64_t v) {
    reg_write(c, reg, (uint32_t)v);
    reg_write(c, reg + 4, (uint32_t)(v >> 32));
}

static void warn(struct nvme_controller *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kprintf(ANSI_YELLOW "  [warn] NVMe %02x:%02x.%x: ", c->pci->bus, c->pci->dev, c->pci->func);
    kvprintf(fmt, ap);
    kprintf(ANSI_RESET "\n");
    va_end(ap);
}

// Wait up to `ms` for (CSTS & CSTS_RDY) == want. False on a timeout or a
// fatal controller status.
static bool wait_ready(struct nvme_controller *c, uint32_t want, uint64_t ms) {
    uint64_t deadline = clock_monotonic_ns() + ms * NS_PER_MS;
    for (;;) {
        uint32_t csts = reg_read(c, NVME_CSTS);
        if (csts == 0xffffffff || (csts & CSTS_CFS)) return false; // gone, or failed
        if ((csts & CSTS_RDY) == want) return true;
        if (clock_monotonic_ns() > deadline) return false;
        thread_sleep(1);
    }
}

// ---------------------------------------------------------------------------
// Queues and commands
// ---------------------------------------------------------------------------

static void queue_init(struct nvme_controller *c, struct nvme_queue *q, uint16_t id, uint16_t depth, uint64_t sq_phys,
                       uint64_t cq_phys) {
    q->id = id;
    q->depth = depth;
    q->sq_phys = sq_phys;
    q->cq_phys = cq_phys;
    q->sq = phys_to_virt(sq_phys);
    q->cq = phys_to_virt(cq_phys);
    memset(phys_to_virt(sq_phys), 0, PAGE_SIZE);
    memset(phys_to_virt(cq_phys), 0, PAGE_SIZE);
    q->sq_tail = q->cq_head = 0;
    q->phase = 1; // the controller writes 1s in its first pass through the queue
    uint32_t stride = 4u << CAP_DSTRD(c->cap);
    q->sq_doorbell = (volatile uint32_t *)((volatile uint8_t *)c->regs + NVME_DOORBELLS + (2 * id) * stride);
    q->cq_doorbell = (volatile uint32_t *)((volatile uint8_t *)c->regs + NVME_DOORBELLS + (2 * id + 1) * stride);
}

static bool cqe_ready(struct nvme_queue *q) { return (q->cq[q->cq_head].status & STATUS_PHASE) == q->phase; }

// Run one command on queue `q` and wait for it. Call with the controller's
// mutex held. 0, or -EIO; the completion entry goes in `*out`.
static int queue_exec(struct nvme_controller *c, struct nvme_queue *q, struct nvme_sqe *cmd, struct nvme_cqe *out) {
    if (c->dead) return -EIO;
    if (c->next_cid == 0xffff) c->next_cid = 0; // FFFFh means "no command" in the error log
    cmd->cid = c->next_cid++;

    uint64_t flags = spin_lock_irqsave(&c->irq_lock);
    uint64_t irqs = c->irq_count;
    spin_unlock_irqrestore(&c->irq_lock, flags);

    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->depth);
    __asm__ volatile("" ::: "memory"); // the entry is in memory before the controller is told
    *q->sq_doorbell = q->sq_tail;

    uint64_t deadline = clock_monotonic_ns() + COMMAND_TIMEOUT_MS * NS_PER_MS;
    bool done = false, interrupted = false;
    if (c->use_irq) {
        flags = spin_lock_irqsave(&c->irq_lock);
        for (;;) {
            done = cqe_ready(q);
            interrupted = c->irq_count != irqs;
            if (done && interrupted) break;
            if (!wait_queue_sleep_until(&c->waiters, &c->irq_lock, deadline)) {
                done = cqe_ready(q);
                interrupted = c->irq_count != irqs;
                break;
            }
        }
        spin_unlock_irqrestore(&c->irq_lock, flags);
        if (done && !interrupted) {
            // It finished, but the interrupt never came: poll from now on.
            warn(c, "a command finished without an interrupt; polling from now on");
            c->use_irq = false;
        }
    } else {
        for (;;) {
            done = cqe_ready(q);
            if (done || clock_monotonic_ns() > deadline) break;
            thread_yield();
        }
    }

    if (!done) {
        uint32_t csts = reg_read(c, NVME_CSTS);
        warn(c, "command %02x on queue %u timed out (CSTS %08x); the controller is not used any more", cmd->opcode,
             q->id, csts);
        c->dead = true;
        return -EIO;
    }
    __asm__ volatile("" ::: "memory"); // the phase tag first, then the rest of the entry
    *out = q->cq[q->cq_head];
    if (++q->cq_head == q->depth) {
        q->cq_head = 0;
        q->phase ^= 1;
    }
    *q->cq_doorbell = q->cq_head;

    if (out->cid != cmd->cid || out->sq_id != q->id) {
        warn(c, "queue %u: a completion for command %u came back for %u (queue %u); the controller is not used any more",
             q->id, cmd->cid, out->cid, out->sq_id);
        c->dead = true;
        return -EIO;
    }
    if (STATUS_SC(out->status) || STATUS_SCT(out->status)) {
        warn(c, "command %02x on queue %u failed (status type %u, code %02x)", cmd->opcode, q->id,
             STATUS_SCT(out->status), STATUS_SC(out->status));
        return -EIO;
    }
    return 0;
}

static int admin_exec(struct nvme_controller *c, struct nvme_sqe *cmd, struct nvme_cqe *out) {
    return queue_exec(c, &c->admin, cmd, out);
}

// Point a command at the first `bytes` of the data buffer: PRP1 is its
// first page, PRP2 the second, or for more than two pages, the PRP list.
static void set_prps(struct nvme_controller *c, struct nvme_sqe *cmd, uint32_t bytes) {
    uint32_t pages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    cmd->prp1 = c->data_phys;
    if (pages == 2) cmd->prp2 = c->data_phys + PAGE_SIZE;
    else if (pages > 2) cmd->prp2 = c->dma_phys + DMA_PRP_LIST * PAGE_SIZE;
}

// IDENTIFY into the data buffer (4 KiB). Call with the mutex held.
static int identify(struct nvme_controller *c, uint8_t cns, uint32_t nsid) {
    struct nvme_sqe cmd = {.opcode = ADMIN_IDENTIFY, .nsid = nsid, .cdw10 = cns};
    set_prps(c, &cmd, PAGE_SIZE);
    struct nvme_cqe cqe;
    return admin_exec(c, &cmd, &cqe);
}

static int ns_rw(struct block_device *dev, uint64_t lba, uint32_t count, void *buf, bool write) {
    struct nvme_namespace *ns = dev->priv;
    struct nvme_controller *c = ns->ctrl;
    uint32_t per_command = c->max_transfer / dev->sector_size;
    uint8_t *b = buf;
    mutex_lock(&c->lock);
    int err = 0;
    while (count && err == 0) {
        uint32_t n = count < per_command ? count : per_command;
        uint32_t bytes = n * dev->sector_size;
        if (c->dead) { // it may still be using c->data
            err = -EIO;
            break;
        }
        struct nvme_sqe cmd = {
            .opcode = write ? NVM_WRITE : NVM_READ,
            .nsid = ns->nsid,
            .cdw10 = (uint32_t)lba,
            .cdw11 = (uint32_t)(lba >> 32),
            .cdw12 = n - 1, // number of logical blocks, 0-based
        };
        set_prps(c, &cmd, bytes);
        if (write) memcpy(c->data, b, bytes);
        struct nvme_cqe cqe;
        err = queue_exec(c, &c->io, &cmd, &cqe);
        if (err == 0 && !write) memcpy(b, c->data, bytes);
        lba += n;
        count -= n;
        b += bytes;
    }
    mutex_unlock(&c->lock);
    return err;
}

static int nvme_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    return ns_rw(dev, lba, count, buf, false);
}

static int nvme_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    return ns_rw(dev, lba, count, (void *)buf, true);
}

static const struct block_ops nvme_ops = {.read = nvme_read, .write = nvme_write};

// ---------------------------------------------------------------------------
// The controller
// ---------------------------------------------------------------------------

static void nvme_interrupt(struct interrupt_frame *f) {
    for (size_t i = 0; i < controller_count; i++) {
        struct nvme_controller *c = controllers[i];
        if (c->vector < 0 || (uint64_t)c->vector != f->vector) continue;
        spin_lock(&c->irq_lock);
        c->irq_count++;
        wait_queue_wake_all(&c->waiters);
        spin_unlock(&c->irq_lock);
    }
}

// Identify strings are ASCII, padded with spaces.
static void id_string(char *out, const uint8_t *in, unsigned len) {
    unsigned start = 0, end = len;
    while (start < end && in[start] == ' ') start++;
    while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\0')) end--;
    memcpy(out, in + start, end - start);
    out[end - start] = '\0';
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

// IDENTIFY namespace `nsid` and register it as a disk. Call with the mutex
// held. False, with a warning if it is one that can't be used.
static bool add_namespace(struct nvme_controller *c, uint32_t nsid, const char *model) {
    if (identify(c, IDENTIFY_NAMESPACE, nsid)) {
        warn(c, "IDENTIFY of namespace %u failed", nsid);
        return false;
    }
    const uint8_t *id = c->data;
    uint64_t size = le64(id + 0); // NSZE, in logical blocks
    if (size == 0) return false;  // not active
    uint8_t nlbaf = id[25], flbas = id[26];
    unsigned format = flbas & 0xf;
    if (nlbaf >= 16) format |= (flbas >> 5 & 3) << 4; // NVMe 2.0: up to 64 formats
    uint32_t lbaf = le32(id + 128 + 4 * format);
    uint32_t metadata = lbaf & 0xffff, lbads = lbaf >> 16 & 0xff;
    if (metadata) {
        warn(c, "namespace %u is formatted with %u bytes of metadata per block (not supported)", nsid, metadata);
        return false;
    }
    if (lbads < 9 || lbads > 31 || (1u << lbads) > c->max_transfer) {
        warn(c, "namespace %u has blocks of 2^%u bytes (not supported)", nsid, lbads);
        return false;
    }

    struct nvme_namespace *ns = kcalloc(1, sizeof *ns);
    if (ns == NULL) {
        warn(c, "namespace %u: no memory", nsid);
        return false;
    }
    ns->ctrl = c;
    ns->nsid = nsid;
    struct block_device *d = &ns->disk;
    ksnprintf(d->name, sizeof d->name, "nvme%un%u", c->index, nsid);
    ksnprintf(d->model, sizeof d->model, "%s", model);
    struct pci_dev *pci = c->pci;
    ksnprintf(d->where, sizeof d->where, "NVMe %02x:%02x.%x namespace %u", pci->bus, pci->dev, pci->func, nsid);
    d->sector_size = 1u << lbads;
    d->sector_count = size;
    d->ops = &nvme_ops;
    d->priv = ns;
    block_register(d);
    return true;
}

// Reset the controller and turn it on with the admin queues. False, with a
// warning, if it doesn't come up.
static bool enable_controller(struct nvme_controller *c) {
    uint64_t timeout_ms = (CAP_TO(c->cap) + 1) * 500ull;
    uint32_t cc = reg_read(c, NVME_CC);
    if (cc & CC_EN) {
        // Turning EN off while the controller is still coming up (RDY not
        // yet set) is not allowed; wait for that first.
        wait_ready(c, CSTS_RDY, timeout_ms);
        reg_write(c, NVME_CC, cc & ~CC_EN);
    }
    if (!wait_ready(c, 0, timeout_ms)) {
        warn(c, "the controller did not reset (CSTS %08x)", reg_read(c, NVME_CSTS));
        return false;
    }

    reg_write(c, NVME_AQA, (uint32_t)(ADMIN_QUEUE_DEPTH - 1) << 16 | (ADMIN_QUEUE_DEPTH - 1));
    reg_write64(c, NVME_ASQ, c->admin.sq_phys);
    reg_write64(c, NVME_ACQ, c->admin.cq_phys);
    reg_write(c, NVME_CC, CC_CSS_NVM | CC_MPS_4K | CC_AMS_RR | CC_IOSQES_64 | CC_IOCQES_16);
    reg_write(c, NVME_CC, CC_CSS_NVM | CC_MPS_4K | CC_AMS_RR | CC_IOSQES_64 | CC_IOCQES_16 | CC_EN);
    if (!wait_ready(c, CSTS_RDY, timeout_ms)) {
        warn(c, "the controller did not become ready (CSTS %08x)", reg_read(c, NVME_CSTS));
        return false;
    }
    return true;
}

// Create the I/O completion queue, then the submission queue that posts to it.
static bool create_io_queues(struct nvme_controller *c) {
    struct nvme_cqe cqe;
    // One I/O queue pair (0-based counts of submission and completion queues).
    struct nvme_sqe cmd = {.opcode = ADMIN_SET_FEATURES, .cdw10 = FEATURE_NUM_QUEUES, .cdw11 = 0};
    if (admin_exec(c, &cmd, &cqe)) return false;

    uint32_t size = (uint32_t)(c->io.depth - 1) << 16 | c->io.id;
    cmd = (struct nvme_sqe){.opcode = ADMIN_CREATE_CQ, .prp1 = c->io.cq_phys, .cdw10 = size,
                            .cdw11 = QUEUE_PHYS_CONTIG | CQ_IRQ_ENABLED}; // interrupt vector 0
    if (admin_exec(c, &cmd, &cqe)) return false;
    cmd = (struct nvme_sqe){.opcode = ADMIN_CREATE_SQ, .prp1 = c->io.sq_phys, .cdw10 = size,
                            .cdw11 = (uint32_t)c->io.id << 16 | QUEUE_PHYS_CONTIG};
    return admin_exec(c, &cmd, &cqe) == 0;
}

static bool nvme_probe(struct pci_dev *d) {
    if (controller_count == MAX_CONTROLLERS) return false;
    volatile uint32_t *regs = pci_map_bar(d, 0);
    if (regs == NULL || d->bars[0].size < NVME_DOORBELLS + 0x100) {
        kprintf(ANSI_YELLOW "  [warn] NVMe %02x:%02x.%x: BAR 0 is not a mapped memory BAR" ANSI_RESET "\n", d->bus,
                d->dev, d->func);
        return false;
    }
    struct nvme_controller *c = kcalloc(1, sizeof *c);
    if (c == NULL) return false;
    c->pci = d;
    c->regs = regs;
    c->vector = -1;
    c->cap = reg_read64(c, NVME_CAP);
    if (!(c->cap & CAP_CSS_NVM)) {
        warn(c, "the controller does not support the NVM command set");
        kfree(c);
        return false;
    }
    if (CAP_MPSMIN(c->cap) != 0) {
        warn(c, "the controller's smallest memory page is %u KiB (only 4 KiB is supported)",
             4u << CAP_MPSMIN(c->cap));
        kfree(c);
        return false;
    }
    c->dma_phys = pmm_alloc_contiguous(DMA_PAGES);
    if (c->dma_phys == 0) {
        warn(c, "no memory");
        kfree(c);
        return false;
    }
    pci_enable(d);

    uint16_t io_depth = CAP_MQES(c->cap) < IO_QUEUE_DEPTH ? (uint16_t)CAP_MQES(c->cap) : IO_QUEUE_DEPTH;
    queue_init(c, &c->admin, 0, ADMIN_QUEUE_DEPTH, c->dma_phys + DMA_ADMIN_SQ * PAGE_SIZE,
               c->dma_phys + DMA_ADMIN_CQ * PAGE_SIZE);
    queue_init(c, &c->io, IO_QUEUE_ID, io_depth, c->dma_phys + DMA_IO_SQ * PAGE_SIZE,
               c->dma_phys + DMA_IO_CQ * PAGE_SIZE);
    c->data_phys = c->dma_phys + DMA_DATA * PAGE_SIZE;
    c->data = phys_to_virt(c->data_phys);
    c->prp_list = phys_to_virt(c->dma_phys + DMA_PRP_LIST * PAGE_SIZE);
    memset(c->prp_list, 0, PAGE_SIZE);
    for (unsigned i = 1; i < DATA_PAGES; i++) c->prp_list[i - 1] = c->data_phys + i * PAGE_SIZE;
    mutex_init(&c->lock, "nvme controller");
    spin_init(&c->irq_lock, "nvme irq");

    // Interrupts before the controller is on: the admin completion queue
    // always uses vector 0.
    const char *irq_kind = "MSI-X";
    int vector = pci_enable_msix(d, 0, nvme_interrupt);
    if (vector < 0) {
        irq_kind = "MSI";
        vector = pci_enable_msi(d, nvme_interrupt);
    }
    c->vector = vector;
    c->use_irq = vector >= 0;
    c->index = (unsigned)controller_count;
    controllers[controller_count++] = c; // the interrupt handler looks here

    if (!enable_controller(c)) goto dead;

    mutex_lock(&c->lock);
    char model[41], serial[21], firmware[9];
    if (identify(c, IDENTIFY_CONTROLLER, 0)) {
        mutex_unlock(&c->lock);
        warn(c, "IDENTIFY of the controller failed");
        goto dead;
    }
    const uint8_t *id = c->data;
    id_string(serial, id + 4, 20);
    id_string(model, id + 24, 40);
    id_string(firmware, id + 64, 8);
    uint8_t mdts = id[77]; // max transfer: 2^MDTS pages of 4 KiB (MPSMIN is 0), 0 for no limit
    uint32_t namespaces = le32(id + 516);
    c->max_transfer = DATA_BYTES;
    if (mdts && (PAGE_SIZE << (mdts < 16 ? mdts : 16)) < DATA_BYTES) c->max_transfer = (uint32_t)(PAGE_SIZE << mdts);

    if (!create_io_queues(c)) {
        mutex_unlock(&c->lock);
        warn(c, "could not create the I/O queues");
        goto dead;
    }

    // The active namespaces: from the list (NVMe 1.1 and later), or 1 to NN.
    uint32_t vs = reg_read(c, NVME_VS);
    uint32_t nsids[MAX_NAMESPACES];
    unsigned count = 0;
    if (vs >= 0x00010100) {
        if (identify(c, IDENTIFY_ACTIVE_NSID, 0) == 0)
            for (unsigned i = 0; i < 1024 && count < MAX_NAMESPACES; i++) {
                uint32_t nsid = le32(c->data + 4 * i);
                if (nsid == 0) break;
                nsids[count++] = nsid;
            }
    } else {
        for (uint32_t nsid = 1; nsid <= namespaces && count < MAX_NAMESPACES; nsid++) nsids[count++] = nsid;
    }
    kprintf("  NVMe %02x:%02x.%x: version %u.%u", d->bus, d->dev, d->func, vs >> 16, vs >> 8 & 0xff);
    if (vs & 0xff) kprintf(".%u", vs & 0xff);
    kprintf(", %s (serial %s, firmware %s), %u active namespace%s, %u KiB per command, ", model, serial,
            firmware, count, count == 1 ? "" : "s", c->max_transfer / 1024);
    if (c->use_irq) kprintf("%s on vector 0x%x\n", irq_kind, vector);
    else if (vector >= 0) kprintf("polled (its %s did not arrive)\n", irq_kind);
    else kprintf("polled (no MSI-X or MSI)\n");

    for (unsigned i = 0; i < count && !c->dead; i++) add_namespace(c, nsids[i], model);
    mutex_unlock(&c->lock);
    return true;

dead:
    // Keep the controller (it is in controllers[], and its DMA memory may be
    // in use), but stop using it.
    c->dead = true;
    return true;
}

static const struct pci_id nvme_ids[] = {
    {PCI_ANY_ID, PCI_ANY_ID, PCI_CLASS(0x01, 0x08, 0x02), PCI_CLASS_MASK_ALL},
    {0},
};
PCI_DRIVER(nvme, nvme_ids, nvme_probe);
