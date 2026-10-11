#include "src/syscall/syscall.h"
#include "src/syscall/uaccess.h"
#include "src/interrupts/interrupts.h"
#include "src/scheduler/scheduler.h"
#include "src/time/time.h"
#include "src/console/console.h"
#include "src/memory_manager/task_memory.h"

/* ---------------------------------------------------------------------------
 * Gestionnaire des appels système (contrat : abi/voidos/syscalls.h).
 *
 * Chaque appel reçoit le contexte sauvegardé de la tâche, ses trois
 * arguments (a3, a4, a5) et l'origine de l'appel. Il écrit son résultat
 * avec set_ret (>= 0, ou -VOID_Exxx) et retourne le contexte à restaurer :
 * le même pour un appel simple, un autre s'il change de tâche.
 * ------------------------------------------------------------------------- */

typedef struct {
    cpu_context_t *ctx;
    uint32_t a0, a1, a2;
    int from_user;          /* appel venu du mode utilisateur (PS.UM) */
} syscall_args_t;

typedef uint32_t *(*syscall_fn_t)(const syscall_args_t *args);

static void set_ret(cpu_context_t *ctx, long value) {
    ctx->a2 = (uint32_t)value;
}

static uint32_t *same(const syscall_args_t *args) {
    return (uint32_t *)args->ctx;
}

/* ---- Appels ------------------------------------------------------------- */

static uint32_t *sys_yield_handler(const syscall_args_t *args) {
    set_ret(args->ctx, 0);
    return schedule_next_task((uint32_t *)args->ctx);
}

static uint32_t *sys_print_handler(const syscall_args_t *args) {
    const char *str = (const char *)args->a0;

    if (args->from_user) {
        if (uaccess_strlen(scheduler_current_task(), str, VOID_PRINT_MAX) < 0) {
            set_ret(args->ctx, -VOID_EFAULT);
            return same(args);
        }
    } else if (str == NULL) {
        set_ret(args->ctx, -VOID_EFAULT);
        return same(args);
    }

    uart_print(str);
    set_ret(args->ctx, 0);
    return same(args);
}

static uint32_t *sys_exit_handler(const syscall_args_t *args) {
    return scheduler_terminate_current((uint32_t *)args->ctx);
}

static uint32_t *sys_sleep_handler(const syscall_args_t *args) {
    uint32_t ms = args->a0;

    set_ret(args->ctx, 0);
    if (ms == 0) return schedule_next_task((uint32_t *)args->ctx);

    /* Même règle que sleep_ms : un tick de plus que la durée arrondie,
     * pour dormir au moins ms millisecondes. */
    uint32_t n = time_ms_to_ticks(ms);
    if (n < TIME_MAX_SLEEP_TICKS) n++;
    return scheduler_sleep_from_syscall((uint32_t *)args->ctx, tick_count() + n);
}

static uint32_t *sys_abi_version_handler(const syscall_args_t *args) {
    set_ret(args->ctx, VOID_ABI_VERSION);
    return same(args);
}

/* Vérifie qu'un tampon passé par une tâche utilisateur lui appartient.
 * Les tâches du noyau ne sont pas contrôlées (elles accèdent à tout). */
static int user_buffer_ok(const syscall_args_t *args, uint32_t addr, uint32_t len, int write) {
    if (!args->from_user) return len == 0 || addr != 0;
    return uaccess_range_ok(scheduler_current_task(), (const void *)addr, len, write);
}

/* ---- Console ------------------------------------------------------------ */

static uint32_t *sys_write_handler(const syscall_args_t *args) {
    uint32_t buf = args->a0;
    uint32_t len = args->a1;

    if (len > VOID_WRITE_MAX) len = VOID_WRITE_MAX;   /* écriture partielle */
        if (!user_buffer_ok(args, buf, len, 0)) {
            set_ret(args->ctx, -VOID_EFAULT);
            return same(args);
        }

        set_ret(args->ctx, (long)console_write((const char *)buf, len));
    return same(args);
}

static uint32_t *sys_read_handler(const syscall_args_t *args) {
    uint32_t buf   = args->a0;
    uint32_t len   = args->a1;
    uint32_t flags = args->a2;

    if (flags & ~(uint32_t)VOID_READ_NONBLOCK) {
        set_ret(args->ctx, -VOID_EINVAL);
        return same(args);
    }
    if (!user_buffer_ok(args, buf, len, 1)) {
        set_ret(args->ctx, -VOID_EFAULT);
        return same(args);
    }
    if (len == 0) {
        set_ret(args->ctx, 0);
        return same(args);
    }

    uint32_t n = console_read((void *)buf, len);
    if (n > 0) {
        set_ret(args->ctx, (long)n);
        return same(args);
    }
    if (flags & VOID_READ_NONBLOCK) {
        set_ret(args->ctx, -VOID_EAGAIN);
        return same(args);
    }

    /* Rien de reçu : la tâche dort un tick, puis l'appel est REJOUÉ.
     * Reculer epc fait réexécuter l'instruction syscall à son réveil ; a2
     * contient toujours le numéro de l'appel (on n'y écrit pas de
     * résultat) et a3..a5 ses arguments. L'attente ne consomme donc rien
     * entre deux vérifications (une par milliseconde à 1000 Hz). */
    args->ctx->epc -= 3;
    return scheduler_sleep_from_syscall((uint32_t *)args->ctx, tick_count() + 1);
}

/* ---- Temps -------------------------------------------------------------- */

static uint32_t *sys_uptime_ms_handler(const syscall_args_t *args) {
    uint32_t out = args->a0;

    /* 8 octets, alignés sur 4 : écrits en deux mots de 32 bits. */
    if ((out & 3u) != 0 || !user_buffer_ok(args, out, 8, 1)) {
        set_ret(args->ctx, -VOID_EFAULT);
        return same(args);
    }

    uint64_t ms = time_uptime_ms();
    volatile uint32_t *dst = (volatile uint32_t *)out;
    dst[0] = (uint32_t)ms;           /* little-endian : poids faible d'abord */
    dst[1] = (uint32_t)(ms >> 32);

    set_ret(args->ctx, 0);
    return same(args);
}

/* ---- Mémoire ------------------------------------------------------------ */

static uint32_t *sys_mem_alloc_handler(const syscall_args_t *args) {
    uint32_t size = args->a0;

    if (size == 0) {
        set_ret(args->ctx, -VOID_EINVAL);
        return same(args);
    }

    void *block = task_mem_alloc(scheduler_current_task(), size);
    set_ret(args->ctx, block ? (long)(uint32_t)block : -VOID_ENOMEM);
    return same(args);
}

static uint32_t *sys_mem_free_handler(const syscall_args_t *args) {
    int r = task_mem_free(scheduler_current_task(), (void *)args->a0);
    set_ret(args->ctx, r == 0 ? 0 : -VOID_EINVAL);
    return same(args);
}

/* ---- Table -------------------------------------------------------------- */

static const syscall_fn_t syscall_table[] = {
    [VOID_SYS_YIELD]       = sys_yield_handler,
    [VOID_SYS_PRINT]       = sys_print_handler,
    [VOID_SYS_EXIT]        = sys_exit_handler,
    [VOID_SYS_SLEEP]       = sys_sleep_handler,
    [VOID_SYS_ABI_VERSION] = sys_abi_version_handler,
    [VOID_SYS_WRITE]       = sys_write_handler,
    [VOID_SYS_READ]        = sys_read_handler,
    [VOID_SYS_UPTIME_MS]   = sys_uptime_ms_handler,
    [VOID_SYS_MEM_ALLOC]   = sys_mem_alloc_handler,
    [VOID_SYS_MEM_FREE]    = sys_mem_free_handler,
};

#define SYSCALL_COUNT (sizeof(syscall_table) / sizeof(syscall_table[0]))

uint32_t *handle_syscall(cpu_context_t *ctx) {
    /* Reprendre après l'instruction syscall (3 octets). */
    ctx->epc += 3;

    syscall_args_t args;
    args.ctx       = ctx;
    args.a0        = ctx->a3;
    args.a1        = ctx->a4;
    args.a2        = ctx->a5;
    args.from_user = (ctx->ps & PS_UM_MASK) != 0;

    uint32_t num = ctx->a2;
    if (num >= SYSCALL_COUNT || syscall_table[num] == NULL) {
        set_ret(ctx, -VOID_ENOSYS);
        return (uint32_t *)ctx;
    }

    return syscall_table[num](&args);
}
