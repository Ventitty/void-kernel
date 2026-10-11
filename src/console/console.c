#include "src/console/console.h"
#include "src/scheduler/spinlock.h"
#include "arch/xtensa_lx6/drivers/esp32_uart/uart.h"

static spinlock_t console_lock = SPINLOCK_INIT;

static void put_translated(char c) {
    if (c == '\n') uart0_putchar('\r');
    uart0_putchar(c);
}

void uart_print(const char *str) {
    spinlock_acquire(&console_lock);
    while (*str != '\0') put_translated(*str++);
    spinlock_release(&console_lock);
}

void uart_print_hex(uint32_t val) {
    static const char hex_chars[] = "0123456789ABCDEF";
    char buf[11];

    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = hex_chars[(val >> (28 - 4 * i)) & 0xF];
    }
    buf[10] = '\0';

    uart_print(buf);
}

uint32_t console_write(const char *buf, uint32_t len) {
    spinlock_acquire(&console_lock);
    for (uint32_t i = 0; i < len; i++) put_translated(buf[i]);
    spinlock_release(&console_lock);
    return len;
}

uint32_t console_read(void *buf, uint32_t len) {
    int n = uart0_read(buf, len);
    return n > 0 ? (uint32_t)n : 0u;
}
