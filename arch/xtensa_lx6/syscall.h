#ifndef VOIDOS_ABI_XTENSA_LX6_SYSCALL_H
#define VOIDOS_ABI_XTENSA_LX6_SYSCALL_H

#include "../../syscalls.h"
#include "../../errno.h"

static inline long voidos_syscall(long num, long a0, long a1, long a2) {
    register long r2 __asm__("a2") = num;
    register long r3 __asm__("a3") = a0;
    register long r4 __asm__("a4") = a1;
    register long r5 __asm__("a5") = a2;

    __asm__ volatile (
        "syscall\n"
        : "+r"(r2)
        : "r"(r3), "r"(r4), "r"(r5)
        : "memory"
    );

    return r2;
}

#endif /* VOIDOS_ABI_XTENSA_LX6_SYSCALL_H */
