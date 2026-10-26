#include "arch/xtensa_lx6/drivers/esp32_uart/uart.h"

void uart0_putchar(char c) {
    while (((UART0_STATUS >> 16) & 0xFFu) >= 128) { }
    UART0_FIFO = (uint32_t)c;
}

int uart0_write(const void *buf, uint32_t len) {
    const uint8_t *in = (const uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) {
        uart0_putchar((char)in[i]);
    }
    return (int)len;
}

int uart0_read(void *buf, uint32_t len) {
    uint8_t *out = (uint8_t *)buf;
    uint32_t n = 0;
    while (n < len && UART0_RX_FIFO_CNT > 0) {
        out[n++] = (uint8_t)(UART0_FIFO & 0xFFu);
    }
    return (int)n;
}

int uart0_wait_tx_idle(uint32_t budget) {
    uint32_t start = cpu_read_ccount();
    while (((UART0_STATUS >> 16) & 0xFFu) != 0 || ((UART0_STATUS >> UART0_ST_UTX_OUT_S) & 0xFu) != 0) {
        if (cpu_read_ccount() - start > budget) return -1;
    }
    return 0;
}

int uart0_set_baud(uint32_t baud, uint32_t clk_hz) {
    /* Diviseur en seizièmes : clk * 16 / baud (uart_ll_set_baudrate). */
    if (baud == 0 || clk_hz > 0x0FFFFFFFu) return -1;
    uint32_t div16 = (clk_hz << 4) / baud;
    uint32_t div_int = div16 >> 4;
    if (div_int == 0 || div_int > UART0_CLKDIV_INT_MAX) return -1;

    UART0_CONF0_REG |= UART0_TICK_REF_ALWAYS_ON;            /* source : APB */
    UART0_CLKDIV_REG = div_int | ((div16 & 0xFu) << UART0_CLKDIV_FRAG_S);
    return 0;
}
