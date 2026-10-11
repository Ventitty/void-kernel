#ifndef SYSCALL_H
#define SYSCALL_H

#include "src/types.h"
#include "arch/xtensa_lx6/syscall.h"
#include "src/syscall/uaccess.h"            /* vérification des pointeurs  */
#include "src/interrupts/interrupts.h"      /* cpu_context_t, PS_UM_MASK   */
#include "src/scheduler/scheduler.h"        /* changement de tâche, sommeil */
#include "src/time/time.h"                  /* ticks, durées               */
#include "src/console/console.h"            /* print, write, read          */
#include "src/memory_manager/task_memory.h" /* mem_alloc, mem_free         */

typedef struct {
    cpu_context_t *ctx;
    uint32_t a0, a1, a2;
    int from_user;          /* appel venu du mode utilisateur (PS.UM) */
} syscall_args_t;

typedef uint32_t *(*syscall_fn_t)(const syscall_args_t *args);

static inline void sys_yield(void) {
    voidos_syscall(VOID_SYS_YIELD, 0, 0, 0);
}

static inline long sys_print(const char *str) {
    return voidos_syscall(VOID_SYS_PRINT, (long)str, 0, 0);
}

static inline void sys_sleep_ms(uint32_t ms) {
    voidos_syscall(VOID_SYS_SLEEP, (long)ms, 0, 0);
}

static inline void sys_exit(void) {
    voidos_syscall(VOID_SYS_EXIT, 0, 0, 0);
    while (1) { }
}

#endif /* SYSCALL_H */
