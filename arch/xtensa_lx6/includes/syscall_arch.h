#ifndef SYSCALL_ARCH_H
#define SYSCALL_ARCH_H

#include "src/types.h"

static inline uint32_t arch_syscall(uint32_t num, uint32_t a0, uint32_t a1, uint32_t a2) {
    register uint32_t r2 __asm__("a2") = num;
    register uint32_t r3 __asm__("a3") = a0;
    register uint32_t r4 __asm__("a4") = a1;
    register uint32_t r5 __asm__("a5") = a2;

    __asm__ volatile (
        "syscall\n"
        : "+r"(r2)
        : "r"(r3), "r"(r4), "r"(r5)
        : "memory"
    );

    return r2;
}

#endif /* SYSCALL_ARCH_H */
