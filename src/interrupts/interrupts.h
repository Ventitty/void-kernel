#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include "src/types.h"
#include "arch/xtensa_lx6/includes/xtensa.h"
#include "arch/xtensa_lx6/includes/cpu.h"
#include "src/scheduler/spinlock.h"

typedef void (*isr_handler_t)(void *arg);

typedef struct {
    isr_handler_t handler;
    void *arg;
} isr_entry_t;

typedef struct {
    uint32_t a0;         // Offset  0 (0*4)
    uint32_t sp_orig;    // Offset  4 (1*4)
    uint32_t a2;         // Offset  8 (2*4)
    uint32_t a3;         // Offset 12 (3*4)
    uint32_t a4;         // Offset 16 (4*4)
    uint32_t a5;         // Offset 20 (5*4)
    uint32_t a6;         // Offset 24 (6*4)
    uint32_t a7;         // Offset 28 (7*4)
    uint32_t a8;         // Offset 32 (8*4)
    uint32_t a9;         // Offset 36 (9*4)
    uint32_t a10;        // Offset 40 (10*4)
    uint32_t a11;        // Offset 44 (11*4)
    uint32_t a12;        // Offset 48 (12*4)
    uint32_t a13;        // Offset 52 (13*4)
    uint32_t a14;        // Offset 56 (14*4)
    uint32_t a15;        // Offset 60 (15*4)
    uint32_t epc;        // Offset 64 (16*4) - PC lors de l'exception/interruption
    uint32_t ps;         // Offset 68 (17*4) - Processor State
    uint32_t sar;        // Offset 72 (18*4) - Shift Amount Register
    uint32_t exccause;   // Offset 76 (19*4) - Cause de l'exception
} cpu_context_t;

extern void uart_print(const char *str);
extern void uart_print_hex(uint32_t val);
extern uint32_t* time_tick_interrupt(uint32_t *sp);
extern uint32_t* schedule_next_task(uint32_t *current_sp);
extern uint32_t* scheduler_terminate_current(uint32_t *sp);
extern uint32_t* handle_syscall(cpu_context_t *ctx);

void interrupts_init(void);
void interrupts_init_this_core(void);
void interrupts_enable_line(uint8_t int_num);
void interrupts_disable_line(uint8_t int_num);
void interrupts_enable_global(void);
void interrupts_disable_global(void);
void interrupts_register_handler(uint8_t int_num, isr_handler_t handler, void *arg);
uint32_t* c_interrupt_handler(uint32_t *sp, uint32_t level);

#endif
