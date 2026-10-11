#include "src/syscall/uaccess.h"

static int region_of(const task_t *t, uint32_t addr, uint32_t *end, int *writable) {
    for (uint32_t i = 0; i < t->mem_count; i++) {
        if (addr >= t->mem[i].start && addr < t->mem[i].end) {
            *end = t->mem[i].end;
            *writable = t->mem[i].writable;
            return 1;
        }
    }
    if (task_mem_lookup(t, addr, end)) {
        *writable = 1;
        return 1;
    }
    return 0;
}

int uaccess_range_ok(const task_t *t, const void *addr, uint32_t len, int write) {
    if (t == NULL) return 0;
    if (len == 0) return 1;

    uint32_t start = (uint32_t)addr;
    uint32_t last  = start + len - 1u;
    if (last < start) return 0;                  /* dépassement d'entier */

        uint32_t end;
    int writable;
    if (!region_of(t, start, &end, &writable)) return 0;
    if (last >= end) return 0;                   /* dans une seule zone  */
        if (write && !writable) return 0;
        return 1;
}

int uaccess_strlen(const task_t *t, const char *s, uint32_t max) {
    if (t == NULL) return -1;

    uint32_t end;
    int writable;
    if (!region_of(t, (uint32_t)s, &end, &writable)) return -1;

    uint32_t room = end - (uint32_t)s;
    if (room > max) room = max;

    for (uint32_t i = 0; i < room; i++) {
        if (s[i] == '\0') return (int)i;
    }
    return -1;
}
