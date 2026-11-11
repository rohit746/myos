// Spinlocks, wait queues, mutexes, semaphores and condition variables, with
// debug checks. See sync.h.

#include <cpu.h>
#include <kheap.h>
#include <kprintf.h>
#include <sched.h>
#include <string.h>
#include <sync.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Debug checks
// ---------------------------------------------------------------------------

enum violation_kind { V_NONE, V_SLEEP, V_NOT_OWNER, V_RECURSIVE, V_ORDER };

// The self-test sets `expecting` to count violations instead of panicking.
// The operation that broke the rule is then skipped where it would do harm
// (an unlock of a lock the thread does not hold, or taking a lock again).
static bool expecting;
static enum violation_kind last_violation;
static unsigned violation_count;

static bool note_violation(enum violation_kind kind) {
    last_violation = kind;
    violation_count++;
    return true;
}

// Panic with the message, or, in the self-test, note the violation and
// evaluate to true.
#define violation(kind, ...) (expecting ? note_violation(kind) : (panic(__VA_ARGS__), true))

static const char *thread_name(const struct thread *t) { return t ? t->name : "(before the scheduler)"; }

static const char *lock_name(const struct lock_dep *d) { return d->name ? d->name : "(internal)"; }

// The spinlock the thread took last, for messages.
static const char *spin_name(const struct thread *t) {
    for (unsigned i = t->locks_held; i-- > 0;)
        if (t->held[i]->spin) return t->held[i]->name;
    return "(internal)";
}

void might_sleep(const char *what) {
    struct thread *t = thread_current();
    if (t == NULL) return; // before the scheduler: nothing can sleep yet, nor needs to
    if (t->spins_held)
        violation(V_SLEEP, "%s: thread %s may sleep while holding spinlock %s", what, t->name, spin_name(t));
    else if (!(read_rflags() & RFLAGS_IF))
        violation(V_SLEEP, "%s: thread %s may sleep with interrupts off (in an interrupt handler?)", what, t->name);
}

// Lock order. Every lock name is a class; taken_after[a] has bit b set once
// class b has been taken while class a was held. Taking b while holding a
// is wrong if a is reachable from b: some thread may hold b and wait for a.
#define LOCK_CLASSES 64

static const char *class_names[LOCK_CLASSES];
static uint64_t taken_after[LOCK_CLASSES];
static unsigned class_count;

// The lock's class number, or -1 once the table is full (then new classes
// are not checked).
static int lock_class(struct lock_dep *d) {
    if (d->class_id) return d->class_id - 1;
    for (unsigned i = 0; i < class_count; i++) {
        if (strcmp(class_names[i], d->name) == 0) {
            d->class_id = i + 1;
            return i;
        }
    }
    if (class_count == LOCK_CLASSES) return -1;
    class_names[class_count] = d->name;
    d->class_id = ++class_count;
    return class_count - 1;
}

static bool reaches(int from, int to) {
    uint64_t seen = 1ull << from, todo = seen;
    while (todo) {
        int c = __builtin_ctzll(todo);
        todo &= todo - 1;
        uint64_t next = taken_after[c] & ~seen;
        if (next & (1ull << to)) return true;
        seen |= next;
        todo |= next;
    }
    return false;
}

// The running thread is about to take the lock: check the order against
// the locks it holds, and add it to them.
static void lock_acquire(struct lock_dep *d) {
    struct thread *t = thread_current();
    if (t == NULL || d->name == NULL) return;
    uint64_t flags = read_rflags();
    cli();
    int c = lock_class(d);
    for (unsigned i = 0; c >= 0 && i < t->locks_held; i++) {
        int h = t->held[i]->class_id - 1;
        if (h < 0 || h == c || (taken_after[h] & (1ull << c))) continue;
        if (reaches(c, h)) {
            violation(V_ORDER,
                      "lock order: thread %s takes %s while holding %s, but %s has been taken while holding %s "
                      "(possible deadlock)",
                      t->name, d->name, t->held[i]->name, t->held[i]->name, d->name);
            continue;
        }
        taken_after[h] |= 1ull << c;
    }
    if (t->locks_held == THREAD_LOCKS_MAX)
        panic("thread %s holds more than %d locks (taking %s)", t->name, THREAD_LOCKS_MAX, d->name);
    t->held[t->locks_held++] = d;
    if (flags & RFLAGS_IF) sti();
}

static void lock_release(const struct lock_dep *d) {
    struct thread *t = thread_current();
    if (t == NULL || d->name == NULL) return;
    uint64_t flags = read_rflags();
    cli();
    for (unsigned i = t->locks_held; i-- > 0;) {
        if (t->held[i] != d) continue;
        for (; i + 1 < t->locks_held; i++) t->held[i] = t->held[i + 1];
        t->locks_held--;
        break;
    }
    if (flags & RFLAGS_IF) sti();
}

// ---------------------------------------------------------------------------
// Spinlocks
// ---------------------------------------------------------------------------

void spin_init(struct spinlock *l, const char *name) { *l = (struct spinlock)SPINLOCK_INIT(name); }

void spin_lock(struct spinlock *l) {
    preempt_disable();
    struct thread *t = thread_current();
    // A spinlock's holder never sleeps or gets preempted, so the only way
    // to find it held by this thread is to have taken it already, here or
    // in the code an interrupt handler interrupted. Spinning would never end.
    if (l->locked && l->owner == t &&
        violation(V_RECURSIVE, "spin_lock: %s already holds spinlock %s (deadlock)", thread_name(t),
                  lock_name(&l->dep))) {
        preempt_enable();
        return;
    }
    lock_acquire(&l->dep);
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        while (l->locked) pause();
    l->owner = t;
    if (t) t->spins_held++;
}

// Let go of the lock but leave preemption off. False if the thread did not
// hold it (in the self-test).
static bool spin_release(struct spinlock *l) {
    struct thread *t = thread_current();
    if ((!l->locked || l->owner != t) &&
        violation(V_NOT_OWNER, "spin_unlock: %s unlocks spinlock %s, which %s%s", thread_name(t),
                  lock_name(&l->dep), l->locked ? "is held by " : "is not held",
                  l->locked ? thread_name(l->owner) : ""))
        return false;
    lock_release(&l->dep);
    if (t) t->spins_held--;
    l->owner = NULL;
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    return true;
}

void spin_unlock(struct spinlock *l) {
    if (spin_release(l)) preempt_enable();
}

uint64_t spin_lock_irqsave(struct spinlock *l) {
    uint64_t flags = read_rflags();
    cli();
    spin_lock(l);
    return flags;
}

// Interrupts go back on before preemption does, so a switch that was put
// off while the lock was held happens now rather than on the next tick.
void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags) {
    bool held = spin_release(l);
    if (flags & RFLAGS_IF) sti();
    if (held) preempt_enable();
}

// ---------------------------------------------------------------------------
// Wait queues
// ---------------------------------------------------------------------------

static uint64_t sleeps; // times a thread blocked on a wait queue (for the self-test)

void wait_queue_sleep(struct wait_queue *wq, struct spinlock *lock) {
    struct thread *t = thread_current();
    if (t == NULL) panic("wait_queue_sleep: no threads yet (lock %s)", lock_name(&lock->dep));
    if (read_rflags() & RFLAGS_IF)
        panic("wait_queue_sleep: lock %s must be taken with spin_lock_irqsave", lock_name(&lock->dep));
    t->wait_next = NULL;
    if (wq->tail) wq->tail->wait_next = t;
    else wq->head = t;
    wq->tail = t;
    sleeps++;
    // Interrupts stay off from here until the switch, so nothing can wake
    // the thread before it is off the CPU.
    spin_unlock(lock);
    if (t->spins_held) panic("thread %s sleeps while holding spinlock %s", t->name, spin_name(t));
    sched_block();
    spin_lock(lock);
}

struct thread *wait_queue_wake_one(struct wait_queue *wq) {
    struct thread *t = wq->head;
    if (t == NULL) return NULL;
    wq->head = t->wait_next;
    if (wq->head == NULL) wq->tail = NULL;
    t->wait_next = NULL;
    thread_wake(t);
    return t;
}

size_t wait_queue_wake_all(struct wait_queue *wq) {
    size_t n = 0;
    while (wait_queue_wake_one(wq)) n++;
    return n;
}

// ---------------------------------------------------------------------------
// Mutexes
// ---------------------------------------------------------------------------

void mutex_init(struct mutex *m, const char *name) {
    *m = (struct mutex){.guard = SPINLOCK_INIT(NULL), .dep = {.name = name}};
}

void mutex_lock(struct mutex *m) {
    might_sleep("mutex_lock");
    struct thread *t = thread_current();
    if (m->locked && m->owner == t &&
        violation(V_RECURSIVE, "mutex_lock: %s already holds mutex %s (deadlock)", thread_name(t), m->dep.name))
        return;
    lock_acquire(&m->dep);
    uint64_t flags = spin_lock_irqsave(&m->guard);
    if (!m->locked) {
        m->locked = true;
        m->owner = t;
    } else {
        if (t == NULL) panic("mutex_lock: mutex %s is held before the scheduler started", m->dep.name);
        wait_queue_sleep(&m->waiters, &m->guard); // mutex_unlock hands it over
        if (m->owner != t) panic("mutex_lock: %s woke up without mutex %s", t->name, m->dep.name);
    }
    spin_unlock_irqrestore(&m->guard, flags);
}

void mutex_unlock(struct mutex *m) {
    struct thread *t = thread_current();
    uint64_t flags = spin_lock_irqsave(&m->guard);
    if ((!m->locked || m->owner != t) &&
        violation(V_NOT_OWNER, "mutex_unlock: %s unlocks mutex %s, which %s%s", thread_name(t), m->dep.name,
                  m->locked ? "is held by " : "is not held", m->locked ? thread_name(m->owner) : "")) {
        spin_unlock_irqrestore(&m->guard, flags);
        return;
    }
    lock_release(&m->dep);
    struct thread *next = wait_queue_wake_one(&m->waiters);
    m->owner = next;
    m->locked = next != NULL;
    spin_unlock_irqrestore(&m->guard, flags);
}

bool mutex_held(const struct mutex *m) { return m->locked && m->owner == thread_current(); }

// ---------------------------------------------------------------------------
// Semaphores
// ---------------------------------------------------------------------------

void sem_init(struct semaphore *s, uint64_t count) {
    *s = (struct semaphore){.guard = SPINLOCK_INIT(NULL), .count = count};
}

void sem_down(struct semaphore *s) {
    might_sleep("sem_down");
    uint64_t flags = spin_lock_irqsave(&s->guard);
    if (s->count) {
        s->count--;
    } else {
        if (thread_current() == NULL) panic("sem_down: semaphore at 0 before the scheduler started");
        wait_queue_sleep(&s->waiters, &s->guard); // sem_up hands its count over
    }
    spin_unlock_irqrestore(&s->guard, flags);
}

void sem_up(struct semaphore *s) {
    uint64_t flags = spin_lock_irqsave(&s->guard);
    if (wait_queue_wake_one(&s->waiters) == NULL) s->count++;
    spin_unlock_irqrestore(&s->guard, flags);
}

// ---------------------------------------------------------------------------
// Condition variables
// ---------------------------------------------------------------------------

void cond_init(struct condvar *c) { *c = (struct condvar){.guard = SPINLOCK_INIT(NULL)}; }

void cond_wait(struct condvar *c, struct mutex *m) {
    might_sleep("cond_wait");
    if (!mutex_held(m) &&
        violation(V_NOT_OWNER, "cond_wait: %s waits without holding mutex %s", thread_name(thread_current()),
                  m->dep.name))
        return;
    // The thread is on the queue before the mutex is free, and a signal
    // needs c->guard, so none can come in between and be missed.
    uint64_t flags = spin_lock_irqsave(&c->guard);
    mutex_unlock(m);
    wait_queue_sleep(&c->waiters, &c->guard);
    spin_unlock_irqrestore(&c->guard, flags);
    mutex_lock(m);
}

void cond_signal(struct condvar *c) {
    uint64_t flags = spin_lock_irqsave(&c->guard);
    wait_queue_wake_one(&c->waiters);
    spin_unlock_irqrestore(&c->guard, flags);
}

void cond_broadcast(struct condvar *c) {
    uint64_t flags = spin_lock_irqsave(&c->guard);
    wait_queue_wake_all(&c->waiters);
    spin_unlock_irqrestore(&c->guard, flags);
}

// ---------------------------------------------------------------------------
// Boot self-test (moves into a KTEST once the test framework exists)
// ---------------------------------------------------------------------------

#define TEST_THREADS    8
#define TEST_ITERS      2000 // per thread, for the mutex and the semaphore
#define TEST_SPIN_MS    200  // spinlock: every thread goes on until then
#define TEST_RACY_ITERS 50   // per thread, with no lock
#define TEST_SLOTS      3    // the counting semaphore's count
#define TEST_QUEUE      4    // condvar test: slots in the queue
#define TEST_ITEMS      1000 // condvar test: items per producer
#define TEST_TIMEOUT_MS 20000

enum test_kind { TEST_NO_LOCK, TEST_SPIN, TEST_MUTEX, TEST_SEM, TEST_SLOTS_SEM };

static enum test_kind test_kind;
static unsigned test_iters;
static uint64_t test_end_ns;          // not 0: run until then instead of test_iters times
static volatile uint64_t increments;  // what the counter should come to
static struct spinlock test_spin = SPINLOCK_INIT("selftest spinlock");
static struct mutex test_mutex;
static struct semaphore test_sem, test_slots;
static volatile uint64_t counter;
static volatile int last_thread;
static volatile uint64_t turns;       // times the counter changed hands between threads
static volatile unsigned finished;    // threads done
static volatile unsigned irqs_on;     // times interrupts were on under spin_lock_irqsave
static volatile unsigned preempted;   // times a spinlock holder lost the CPU
static volatile unsigned inside, most_inside; // counting semaphore: threads past sem_down

// Read, wait, write back: two threads in here at once lose an update.
static void bump(int me, bool yield) {
    if (last_thread != me) {
        last_thread = me;
        turns++;
    }
    uint64_t v = counter;
    if (yield) thread_yield();
    else
        for (int i = 0; i < 20; i++) pause();
    counter = v + 1;
}

static void contender(void *arg) {
    int me = (int)(uintptr_t)arg;
    unsigned i;
    for (i = 0; test_end_ns ? clock_monotonic_ns() < test_end_ns : i < test_iters; i++) {
        bool yield = i % 8 == 0; // with the lock held: others pile up on it
        switch (test_kind) {
        case TEST_NO_LOCK:
            bump(me, true);
            break;
        case TEST_SPIN: // no yield: a spinlock holder can't sleep
            if (i & 1) {
                uint64_t flags = spin_lock_irqsave(&test_spin);
                if (read_rflags() & RFLAGS_IF) irqs_on++;
                bump(me, false);
                spin_unlock_irqrestore(&test_spin, flags);
            } else {
                spin_lock(&test_spin);
                uint64_t switches = thread_current()->switches;
                bump(me, false);
                if (thread_current()->switches != switches) preempted++;
                spin_unlock(&test_spin);
            }
            break;
        case TEST_MUTEX:
            mutex_lock(&test_mutex);
            bump(me, yield);
            mutex_unlock(&test_mutex);
            break;
        case TEST_SEM:
            sem_down(&test_sem);
            bump(me, yield);
            sem_up(&test_sem);
            break;
        case TEST_SLOTS_SEM: {
            sem_down(&test_slots);
            preempt_disable();
            if (++inside > most_inside) most_inside = inside;
            preempt_enable();
            thread_yield();
            preempt_disable();
            inside--;
            preempt_enable();
            sem_up(&test_slots);
            break;
        }
        }
    }
    __atomic_add_fetch(&increments, i, __ATOMIC_RELAXED);
    __atomic_add_fetch(&finished, 1, __ATOMIC_RELAXED);
}

// Condvar test: producers and consumers share a small queue.
static struct mutex queue_lock;
static struct condvar not_full, not_empty;
static uint64_t queue[TEST_QUEUE];
static unsigned queue_head, queue_len;
static volatile uint64_t consumed_sum, consumed_count;

static void producer(void *arg) {
    for (uint64_t v = 1; v <= TEST_ITEMS; v++) {
        mutex_lock(&queue_lock);
        while (queue_len == TEST_QUEUE) cond_wait(&not_full, &queue_lock);
        queue[(queue_head + queue_len) % TEST_QUEUE] = v;
        queue_len++;
        cond_signal(&not_empty);
        mutex_unlock(&queue_lock);
    }
    __atomic_add_fetch(&finished, 1, __ATOMIC_RELAXED);
}

static void consumer(void *arg) {
    for (unsigned i = 0; i < TEST_ITEMS; i++) {
        mutex_lock(&queue_lock);
        while (queue_len == 0) cond_wait(&not_empty, &queue_lock);
        uint64_t v = queue[queue_head];
        queue_head = (queue_head + 1) % TEST_QUEUE;
        queue_len--;
        consumed_sum += v;
        consumed_count++;
        cond_broadcast(&not_full);
        mutex_unlock(&queue_lock);
    }
    __atomic_add_fetch(&finished, 1, __ATOMIC_RELAXED);
}

static void wait_finished(unsigned want, const char *what) {
    uint64_t t0 = clock_monotonic_ns();
    while (finished < want) {
        if (clock_monotonic_ns() - t0 > TEST_TIMEOUT_MS * NS_PER_MS)
            panic("sync self-test: %s: %u of %u threads done after %d ms (deadlock?)", what, finished, want,
                  TEST_TIMEOUT_MS);
        thread_sleep(5);
    }
}

// Run TEST_THREADS contenders on one kind of lock, `iters` times each or
// (iters 0) for `ms`. Returns the counter.
static uint64_t contend(enum test_kind kind, unsigned iters, unsigned ms, const char *what) {
    test_kind = kind;
    test_iters = iters;
    test_end_ns = iters ? 0 : clock_monotonic_ns() + ms * NS_PER_MS;
    increments = 0;
    counter = 0;
    turns = 0;
    last_thread = -1;
    finished = 0;
    for (int i = 0; i < TEST_THREADS; i++) {
        char name[16];
        ksnprintf(name, sizeof name, "contender%d", i);
        if (thread_create(name, contender, (void *)(uintptr_t)i) == NULL) panic("sync self-test: no memory");
    }
    wait_finished(TEST_THREADS, what);
    return counter;
}

static void check_exact(enum test_kind kind, unsigned iters, unsigned ms, const char *what, bool must_sleep) {
    uint64_t sleeps0 = sleeps;
    uint64_t got = contend(kind, iters, ms, what), want = increments;
    if (got != want || want < TEST_THREADS) panic("sync self-test: %s: counter is %lu, expected %lu", what, got, want);
    // Threads must really have taken turns on the lock (preempted, or
    // blocked on it), or the test proves nothing.
    if (turns <= TEST_THREADS)
        panic("sync self-test: %s: the threads never interleaved (%lu turns)", what, turns);
    if (must_sleep && sleeps == sleeps0) panic("sync self-test: %s: no thread ever blocked on the lock", what);
}

// Debug checks: each must fire exactly once.
static struct mutex check_mutex, order_a, order_b;
static struct spinlock check_spin = SPINLOCK_INIT("selftest held spinlock");
static struct spinlock order_x = SPINLOCK_INIT("selftest order x"), order_y = SPINLOCK_INIT("selftest order y"),
                       order_z = SPINLOCK_INIT("selftest order z");
static unsigned checks_passed;

static void expect(enum violation_kind kind, const char *what) {
    if (violation_count != 1 || last_violation != kind)
        panic("sync self-test: the check for %s did not fire (%u violations, last kind %d)", what, violation_count,
              last_violation);
    violation_count = 0;
    last_violation = V_NONE;
    checks_passed++;
}

static void unlock_check_mutex(void *arg) {
    mutex_unlock(&check_mutex); // held by the main thread
    __atomic_add_fetch(&finished, 1, __ATOMIC_RELAXED);
}

static void test_debug_checks(void) {
    expecting = true;
    violation_count = 0;

    spin_lock(&check_spin);
    mutex_lock(&check_mutex);
    mutex_unlock(&check_mutex);
    spin_unlock(&check_spin);
    expect(V_SLEEP, "sleeping while holding a spinlock");

    cli();
    mutex_lock(&check_mutex);
    mutex_unlock(&check_mutex);
    sti();
    expect(V_SLEEP, "sleeping with interrupts off");

    mutex_lock(&check_mutex);
    finished = 0;
    if (thread_create("unlocker", unlock_check_mutex, NULL) == NULL) panic("sync self-test: no memory");
    wait_finished(1, "unlocker");
    if (!mutex_held(&check_mutex)) panic("sync self-test: another thread unlocked the main thread's mutex");
    expect(V_NOT_OWNER, "unlocking another thread's mutex");

    mutex_lock(&check_mutex);
    expect(V_RECURSIVE, "taking a mutex twice");
    mutex_unlock(&check_mutex);

    spin_unlock(&check_spin);
    expect(V_NOT_OWNER, "unlocking a free spinlock");

    mutex_lock(&order_a);
    mutex_lock(&order_b);
    mutex_unlock(&order_b);
    mutex_unlock(&order_a);
    mutex_lock(&order_b);
    mutex_lock(&order_a);
    mutex_unlock(&order_a);
    mutex_unlock(&order_b);
    expect(V_ORDER, "lock order (b after a, then a after b)");

    spin_lock(&order_x);
    spin_lock(&order_y);
    spin_unlock(&order_y);
    spin_unlock(&order_x);
    spin_lock(&order_y);
    spin_lock(&order_z);
    spin_unlock(&order_z);
    spin_unlock(&order_y);
    spin_lock(&order_z);
    spin_lock(&order_x);
    spin_unlock(&order_x);
    spin_unlock(&order_z);
    expect(V_ORDER, "lock order through another lock (x, y, z, then x after z)");

    expecting = false;
    if (thread_current()->locks_held || thread_current()->spins_held)
        panic("sync self-test: main still holds %u locks", thread_current()->locks_held);
}

void sync_self_test(char *line, size_t size) {
    size_t threads0 = sched_thread_count();
    uint64_t allocs0 = kheap_get_stats().allocations;
    uint64_t sleeps0 = sleeps;
    mutex_init(&test_mutex, "selftest mutex");
    sem_init(&test_sem, 1);
    sem_init(&test_slots, TEST_SLOTS);

    // Without a lock the test must notice: updates get lost.
    uint64_t racy = contend(TEST_NO_LOCK, TEST_RACY_ITERS, 0, "no lock");
    uint64_t racy_want = (uint64_t)TEST_THREADS * TEST_RACY_ITERS;
    if (racy >= racy_want) panic("sync self-test: no updates lost without a lock (%lu of %lu)", racy, racy_want);

    irqs_on = preempted = 0;
    check_exact(TEST_SPIN, 0, TEST_SPIN_MS, "spinlock", false);
    if (irqs_on) panic("sync self-test: interrupts were on under spin_lock_irqsave %u times", irqs_on);
    if (preempted) panic("sync self-test: spinlock holders were preempted %u times", preempted);
    check_exact(TEST_MUTEX, TEST_ITERS, 0, "mutex", true);
    check_exact(TEST_SEM, TEST_ITERS, 0, "semaphore", true);

    most_inside = 0;
    contend(TEST_SLOTS_SEM, 200, 0, "counting semaphore");
    if (most_inside != TEST_SLOTS)
        panic("sync self-test: semaphore(%d) let %u threads in at once", TEST_SLOTS, most_inside);

    mutex_init(&queue_lock, "selftest queue");
    cond_init(&not_full);
    cond_init(&not_empty);
    queue_head = queue_len = 0;
    consumed_sum = consumed_count = 0;
    finished = 0;
    for (int i = 0; i < TEST_THREADS / 2; i++) {
        if (thread_create("producer", producer, NULL) == NULL || thread_create("consumer", consumer, NULL) == NULL)
            panic("sync self-test: no memory");
    }
    wait_finished(TEST_THREADS, "condition variable");
    uint64_t items = (uint64_t)TEST_THREADS / 2 * TEST_ITEMS;
    uint64_t sum = (uint64_t)TEST_THREADS / 2 * TEST_ITEMS * (TEST_ITEMS + 1) / 2;
    if (consumed_count != items || consumed_sum != sum || queue_len != 0)
        panic("sync self-test: condvar queue: %lu items summing to %lu, expected %lu summing to %lu", consumed_count,
              consumed_sum, items, sum);

    mutex_init(&check_mutex, "selftest check mutex");
    mutex_init(&order_a, "selftest order a");
    mutex_init(&order_b, "selftest order b");
    test_debug_checks();

    // Wait for every thread to exit and be freed.
    uint64_t t0 = clock_monotonic_ns();
    while (sched_thread_count() > threads0) {
        if (clock_monotonic_ns() - t0 > TEST_TIMEOUT_MS * NS_PER_MS)
            panic("sync self-test: %lu threads still there", sched_thread_count() - threads0);
        thread_sleep(5);
    }
    uint64_t allocs = kheap_get_stats().allocations;
    if (allocs != allocs0) panic("sync self-test: %lu heap allocations before, %lu after", allocs0, allocs);

    ksnprintf(line, size,
              "sync self-test passed (%d threads: exact counts with spinlock, mutex, semaphore, condvar; "
              "%lu waits; %u debug checks)",
              TEST_THREADS, sleeps - sleeps0, checks_passed);
}
