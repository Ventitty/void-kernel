#ifndef UART_H
#define UART_H

#include "src/types.h"
#include "arch/xtensa_lx6/includes/xtensa.h"
#include "arch/xtensa_lx6/includes/cpu.h"

#define UART0_CLKDIV_REG       (*(volatile uint32_t *)(0x3FF40000u + 0x0014))
#define UART0_CLKDIV_FRAG_S    20                 /* partie fractionnaire, /16 */
#define UART0_CLKDIV_INT_MAX   0xFFFFFu
#define UART0_TICK_REF_ALWAYS_ON (1u << 27)       /* CONF0 : 1 = horloge APB */
#define UART0_ST_UTX_OUT_S     24                 /* STATUS : état de l'émetteur */

void uart0_putchar(char c);
int  uart0_write(const void *buf, uint32_t len);
int  uart0_read(void *buf, uint32_t len);
int  uart0_wait_tx_idle(uint32_t budget);
int  uart0_set_baud(uint32_t baud, uint32_t clk_hz);

#endif /* UART_H */
