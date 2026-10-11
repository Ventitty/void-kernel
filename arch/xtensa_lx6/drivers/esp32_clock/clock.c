#include "arch/xtensa_lx6/drivers/esp32_clock/clock.h"
#include "arch/xtensa_lx6/includes/xtensa.h"
#include "arch/xtensa_lx6/includes/cpu.h"

/* Aucune division 64 bits dans ce fichier : celle de libgcc est compilée
 * pour l'ABI windowed, incompatible avec le noyau (call0). */
typedef char clock_hz_per_cycle_must_be_exact[
    (CLOCK_UART_BAUD % (CLOCK_TIMED_BYTES * CLOCK_BITS_PER_BYTE)) == 0 ? 1 : -1];

    /* Nombre d'octets présents dans la FIFO d'émission (STATUS bits 23:16). */
    static uint32_t txfifo_count(void) {
        return (UART0_STATUS >> 16) & 0xFFu;
    }

    /* Attend que la FIFO d'émission soit vide et retourne l'instant (CCOUNT)
     * où on l'a constaté. 0 dans *ok si l'attente dépasse budget cycles. */
    static uint32_t wait_txfifo_empty(uint32_t budget, int *ok) {
        uint32_t start = cpu_read_ccount();
        while (txfifo_count() != 0) {
            if (cpu_read_ccount() - start > budget) { *ok = 0; return 0; }
        }
        return cpu_read_ccount();
    }

    uint32_t clock_measure_cpu_hz(const char *label) {
        if (label == NULL || label[0] == '\0') return 0;

        /* Budget d'attente large : 2^31 cycles, soit plusieurs secondes même
         * à 240 MHz, très au-delà de l'émission de quelques dizaines d'octets. */
        const uint32_t budget = 0x80000000u;
        int ok = 1;

        /* 1. Émettre le libellé et attendre que la FIFO se vide. À cet instant
         *    t0, le dernier octet du libellé vient de passer dans le registre
         *    à décalage et commence à partir sur la ligne. */
        for (const char *p = label; *p; p++) {
            while (txfifo_count() >= 128u) { }
            UART0_FIFO = (uint32_t)*p;
        }
        uint32_t t0 = wait_txfifo_empty(budget, &ok);

        /* 2. Remplir aussitôt la FIFO avec les octets chronométrés (128 places,
         *    l'écriture est instantanée à l'échelle d'un octet). Chacun quitte
         *    la FIFO quand le précédent a fini d'être émis : la FIFO redevient
         *    vide exactement CLOCK_TIMED_BYTES durées d'octet après t0. */
        for (uint32_t i = 0; i < CLOCK_TIMED_BYTES; i++) {
            UART0_FIFO = (uint32_t)'.';
        }
        uint32_t t1 = wait_txfifo_empty(budget, &ok);

        if (!ok) return 0;

        /* cycles par seconde = cycles écoulés / durée d'émission
         *                    = (t1 - t0) * baud / (octets * bits par octet)
         *                    = (t1 - t0) * CLOCK_HZ_PER_CYCLE */
        uint32_t cycles = t1 - t0;
        if (cycles == 0 || cycles > 0xFFFFFFFFu / CLOCK_HZ_PER_CYCLE) return 0;

        return clock_snap_hz(cycles * CLOCK_HZ_PER_CYCLE);
    }

    uint32_t clock_snap_hz(uint32_t measured_hz) {
        static const uint32_t standard_mhz[] = { 240, 160, 80, 40, 26, 20, 10 };

        for (uint32_t i = 0; i < sizeof(standard_mhz) / sizeof(standard_mhz[0]); i++) {
            uint32_t ref = standard_mhz[i] * 1000000u;
            uint32_t diff = measured_hz > ref ? measured_hz - ref : ref - measured_hz;
            if (diff < ref / 25u) return ref;   /* écart < 4 % */
        }

        /* Fréquence inhabituelle : arrondi au MHz (sans débordement). */
        uint32_t mhz = measured_hz / 1000000u + ((measured_hz % 1000000u) >= 500000u ? 1u : 0u);
        if (mhz > 0xFFFFFFFFu / 1000000u) mhz = 0xFFFFFFFFu / 1000000u;
        return mhz * 1000000u;
    }
