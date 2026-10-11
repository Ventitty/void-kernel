#include "src/interrupts/interrupts.h"

extern char _vector_base[];
static isr_entry_t isr_table[32];
static spinlock_t panic_print_lock = SPINLOCK_INIT;

void interrupts_init_this_core(void) {
    uint32_t vecbase = (uint32_t)_vector_base;
    cpu_set_vecbase(vecbase);
    cpu_write_intenable(0);
    cpu_clear_irq(0xFFFFFFFFu);
}

void interrupts_init(void) {
    interrupts_init_this_core();

    for (int i = 0; i < 32; i++) {
        isr_table[i].handler = 0;
        isr_table[i].arg = 0;
    }
}

void interrupts_enable_line(uint8_t int_num) {
    if (int_num >= 32) return;

    cpu_write_intenable(cpu_read_intenable() | (1U << int_num));
}

void interrupts_disable_line(uint8_t int_num) {
    if (int_num >= 32) return;

    cpu_write_intenable(cpu_read_intenable() & ~(1U << int_num));
}

void interrupts_register_handler(uint8_t int_num, isr_handler_t handler, void *arg) {
    if (int_num >= 32) return;
    isr_table[int_num].handler = handler;
    isr_table[int_num].arg = arg;
}

void interrupts_enable_global(void) {
    (void)cpu_irq_unmask_all();
}

void interrupts_disable_global(void) {
    (void)cpu_irq_mask_all();
}

static void panic_dump(const cpu_context_t *ctx) {
    spinlock_acquire(&panic_print_lock);   /* jamais relâché : un panic est terminal, voir plus bas */

    uart_print("\n================ PANIC KERNEL ================\n");
    uart_print(" EXCEPTION FATALE : Cause = "); uart_print_hex(ctx->exccause); uart_print("\n");
    uart_print(" EPC  (PC fautif) : "); uart_print_hex(ctx->epc); uart_print("\n");
    uart_print(" PS  (Processor)  : "); uart_print_hex(ctx->ps); uart_print("\n");
    uart_print("----------------------------------------------\n");
    uart_print(" Registres :\n");
    uart_print("   a0 (Return Addr) : "); uart_print_hex(ctx->a0); uart_print("\n");
    uart_print("   a1 (Stack Orig)  : "); uart_print_hex(ctx->sp_orig); uart_print("\n");
    uart_print("   a2 : "); uart_print_hex(ctx->a2); uart_print(" | a3 : "); uart_print_hex(ctx->a3); uart_print("\n");
    uart_print("==============================================\n");

    spinlock_release(&panic_print_lock);   /* libère APRES l'affichage : si l'autre coeur panique aussi, il pourra afficher son propre diagnostic au lieu de rester bloqué en silence sur ce verrou */

    while (1) {
        cpu_wait_irq();
    }
}

/* Bit de l'interruption du tick : traitée à part, jamais par isr_table. */
#define TICK_IRQ_BIT (1U << TIMER0_IRQ_LINE)

/* Appelle le gestionnaire de chaque ligne active de lines. Une ligne
 * active sans gestionnaire est désactivée et signalée : une interruption
 * de niveau inattendue se redéclencherait sinon en boucle. Retourne le
 * nombre de gestionnaires appelés. */
static int dispatch_lines(uint32_t lines) {
    int handled = 0;

    for (int i = 0; i < 32; i++) {
        if (!(lines & (1U << i))) continue;

        if (isr_table[i].handler != 0) {
            isr_table[i].handler(isr_table[i].arg);
            handled++;
        } else {
            interrupts_disable_line((uint8_t)i);
            uart_print("[irq] ligne sans gestionnaire, desactivee : ");
            uart_print_hex((uint32_t)i);
            uart_print("\n");
        }

        cpu_clear_irq(1U << i);
    }

    return handled;
}

static uint32_t active_lines(void) {
    return cpu_read_pending_irq() & cpu_read_intenable();
}

uint32_t* c_interrupt_handler(uint32_t *sp, uint32_t level) {
    cpu_context_t *ctx = (cpu_context_t *)sp;

    if (level == 1) {
        if (ctx->exccause == EXCCAUSE_SYSCALL) {
            return handle_syscall(ctx);
        }

        if (ctx->exccause != EXCCAUSE_LEVEL1_INTERRUPT) {
            spinlock_acquire(&panic_print_lock);
            uart_print("[diag] EXCCAUSE="); uart_print_hex(ctx->exccause);
            uart_print(" PS="); uart_print_hex(ctx->ps);
            uart_print(" EPC="); uart_print_hex(ctx->epc);
            uart_print("\n");
            spinlock_release(&panic_print_lock);

            int was_user_mode = (ctx->ps & PS_UM_MASK) != 0;
            if (was_user_mode) {
                uart_print("[noyau] tache user terminee (EXCCAUSE=");
                uart_print_hex(ctx->exccause);
                uart_print(", EPC="); uart_print_hex(ctx->epc);
                uart_print(")\n");
                return scheduler_terminate_current(sp);
            }

            panic_dump(ctx);
            return sp;
        }

        /* Interruptions de niveau 1 : périphériques d'abord, puis le tick,
         * qui peut changer de tâche et doit donc passer en dernier. */
        uint32_t active = active_lines();
        dispatch_lines(active & ~TICK_IRQ_BIT);

        if (active & TICK_IRQ_BIT) {
            return time_tick_interrupt(sp);
        }
        return sp;
    }

    if (dispatch_lines(active_lines() & ~TICK_IRQ_BIT) == 0) {
        panic_dump(ctx);
    }

    return sp;
}
