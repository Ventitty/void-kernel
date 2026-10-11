#include "src/syscall/syscall.h"
#include "src/interrupts/interrupts.h"
#include "src/scheduler/scheduler.h"
#include "src/kernel/kernel.h"
#include "src/time/time.h"

uint32_t *handle_syscall(cpu_context_t *ctx) {
    ctx->epc += 3;

    uint32_t num = ctx->a2;
    uint32_t arg0 = ctx->a3;

    switch (num) {
        case SYS_YIELD:
            ctx->a2 = 0;
            return schedule_next_task((uint32_t *)ctx);

        case SYS_PRINT:
            uart_print((const char *)arg0);
            ctx->a2 = 0;
            return (uint32_t *)ctx;

        case SYS_SLEEP: {
            ctx->a2 = 0;
            if (arg0 == 0) return schedule_next_task((uint32_t *)ctx);

            /* Même règle que sleep_ms : un tick de plus que la durée
             * arrondie, pour dormir au moins arg0 millisecondes. */
            uint32_t n = time_ms_to_ticks(arg0);
            if (n < TIME_MAX_SLEEP_TICKS) n++;
            return scheduler_sleep_from_syscall((uint32_t *)ctx, tick_count() + n);
        }

        case SYS_EXIT:
            return scheduler_terminate_current((uint32_t *)ctx);

        default:
            ctx->a2 = (uint32_t)-1;
            return (uint32_t *)ctx;
    }
}
