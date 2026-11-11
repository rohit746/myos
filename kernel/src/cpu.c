// CPU identification through CPUID.

#include <cpu.h>
#include <string.h>

void cpu_identify(struct cpu_ident *id) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    memcpy(id->vendor + 0, &b, 4);
    memcpy(id->vendor + 4, &d, 4);
    memcpy(id->vendor + 8, &c, 4);
    id->vendor[12] = '\0';

    id->family = id->model = id->stepping = 0;
    if (a >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        unsigned family = (a >> 8) & 0xf, model = (a >> 4) & 0xf;
        if (family == 0xf) family += (a >> 20) & 0xff;
        if (family >= 6) model |= ((a >> 16) & 0xf) << 4;
        id->family = family;
        id->model = model;
        id->stepping = a & 0xf;
    }

    char brand[49] = "unknown";
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        uint32_t *p = (uint32_t *)brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++, p += 4)
            cpuid(leaf, 0, &p[0], &p[1], &p[2], &p[3]);
        brand[48] = '\0';
    }
    const char *name = brand;
    while (*name == ' ') name++;
    size_t n = strlen(name);
    while (n > 0 && name[n - 1] == ' ') n--;
    memcpy(id->brand, name, n);
    id->brand[n] = '\0';
}
