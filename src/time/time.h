#ifndef TIME_H
#define TIME_H

#include "src/types.h"
#include "src/scheduler/scheduler.h"
#include "src/scheduler/spinlock.h"
#include "src/syscall/syscall.h"
#include "src/utils/utils.h"
#include "arch/xtensa_lx6/includes/cpu.h"
#include "arch/xtensa_lx6/includes/xtensa.h"
#include "arch/xtensa_lx6/drivers/esp32_clock/clock.h"
#include "arch/xtensa_lx6/drivers/esp32_uart/uart.h"

#ifndef TICK_HZ
#define TICK_HZ 1000u
#endif

#define TICK_MS (1000u / TICK_HZ)
#define TIME_MAX_SLEEP_TICKS 0x3FFFFFFFu
#define TICK_MIN_MARGIN_CYCLES 1000u

typedef char tick_hz_must_divide_1000[(1000u % TICK_HZ) == 0 ? 1 : -1];

void time_init(void);
void time_start_this_core(void);
uint32_t *time_tick_interrupt(uint32_t *sp);
uint32_t tick_count(void);
uint64_t tick_count64(void);
uint64_t time_uptime_ms(void);
uint32_t time_cpu_hz(void);

int time_set_cpu_mhz(uint32_t mhz);
uint32_t time_ms_to_ticks(uint32_t ms);
int sleep_ms(uint32_t ms);
int sleep_ticks(uint32_t n);
int sleep_until(uint32_t *last_wake, uint32_t period);

void delay_us(uint32_t us);
void delay_ms(uint32_t ms);

extern void uart_print(const char *str);

#endif /* TIME_H */
