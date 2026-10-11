#include "src/kernel/kernel.h"

void echo(void) {
    UART0_CONF0_REG |= UART_RXFIFO_RST;
    UART0_CONF0_REG &= ~UART_RXFIFO_RST;
    UART0_INT_CLR_REG = UART_RX_ERROR_MASK;

    uart_print("Input > ");
    led_toggle();

    while (1) {
        uint32_t int_st = UART0_INT_ST_REG;
        if (int_st & UART_RX_ERROR_MASK) {
            UART0_INT_CLR_REG = UART_RX_ERROR_MASK;
            UART0_CONF0_REG |= UART_RXFIFO_RST;
            UART0_CONF0_REG &= ~UART_RXFIFO_RST;
            continue;
        }

        if (UART0_RX_FIFO_CNT > 0) {
            char c = (char)(UART0_FIFO & 0xFFu);

            if (c == '\r' || c == '\n') {
                uart0_putchar('\r');
                uart0_putchar('\n');
                uart_print("Input > ");
                led_toggle();
            } else if (c == '\b' || c == 0x7F) {
                uart_print("\b \b");
            } else {
                uart0_putchar(c);
            }
        }
    }
}

#define SD_SCLK 18
#define SD_MOSI 23
#define SD_MISO 19
#define SD_CS   5

static int spi2_write(const void *buf, uint32_t len) {
    spi_transfer((const uint8_t *)buf, NULL, len);
    return (int)len;
}

static int spi2_read(void *buf, uint32_t len) {
    spi_transfer(NULL, (uint8_t *)buf, len);
    return (int)len;
}

void kernel_main(void) {
    wdt_disable_all();
    mm_init();
    interrupts_init();
    time_init();          /* avant le second cœur : il arme son tick avec */
    scheduler_init();
    start_app_cpu();
    led_init();

    vfs_init();

    int sd = vfs_mount_sd(SD_SCLK, SD_MOSI, SD_MISO, SD_CS);
    if (sd == VFS_SD_NO_CARD) {
        uart_print("[init] pas de carte SD : le noyau tourne sans\n");
    } else if (sd == VFS_SD_BAD_FS) {
        uart_print("[init] carte illisible (formatee en FAT32 ?)\n");
    }

    if (vfs_mknod("/dev/uart0", uart0_read, uart0_write) != 0) {
        uart_print("[init] echec creation /dev/uart0\n");
    }
    if (vfs_mknod("/dev/spi2", spi2_read, spi2_write) != 0) {
        uart_print("[init] echec creation /dev/spi2\n");
    }

    task_create(echo);

    scheduler_start();
}
