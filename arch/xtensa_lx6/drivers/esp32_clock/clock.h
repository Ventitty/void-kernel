#ifndef CLOCK_H
#define CLOCK_H

#include "src/types.h"
#include "arch/xtensa_lx6/includes/xtensa.h"
#include "arch/xtensa_lx6/includes/cpu.h"

#define CLOCK_UART_BAUD      115200u
#define CLOCK_BITS_PER_BYTE  10u          /* start + 8 données + stop */
#define CLOCK_TIMED_BYTES    32u          /* octets chronométrés      */

#define CLOCK_HZ_PER_CYCLE   (CLOCK_UART_BAUD / (CLOCK_TIMED_BYTES * CLOCK_BITS_PER_BYTE))
#define CLOCK_DEFAULT_HZ     160000000u

uint32_t clock_measure_cpu_hz(const char *label);
uint32_t clock_snap_hz(uint32_t measured_hz);

#define CLOCK_XTAL_MHZ 40u     /* quartz de la carte ; réglages PLL prévus pour 40 MHz */
#define CLOCK_RTC_CNTL_CLK_CONF_REG   0x3FF48070u   /* SOC_CLK_SEL : bits 28:27         */
#define CLOCK_RTC_CNTL_REG            0x3FF4807Cu   /* DIG_DBIAS_WAK : bits 13:11       */
#define CLOCK_SYSCON_SYSCLK_CONF_REG  0x3FF66000u   /* PRE_DIV_CNT : bits 9:0           */
#define CLOCK_SYSCON_XTAL_TICK_REG    0x3FF66004u   /* REF_TICK en mode quartz          */
#define CLOCK_SYSCON_PLL_TICK_REG     0x3FF66008u   /* REF_TICK en mode PLL             */
#define CLOCK_DPORT_CPU_PER_CONF_REG  0x3FF0003Cu   /* CPUPERIOD_SEL : 0=80 1=160 2=240 */
#define CLOCK_ANA_CONFIG_REG          0x6000E044u   /* bit 17 à 0 : esclave I2C du PLL  */
#define CLOCK_ANA_I2C_HOST_REG(h)     (0x6000E000u + 4u * (uint32_t)(h))

#define CLOCK_SOC_CLK_SEL_S      27
#define CLOCK_SOC_CLK_SEL_M      (0x3u << CLOCK_SOC_CLK_SEL_S)
#define CLOCK_SRC_XTAL           0u
#define CLOCK_SRC_PLL            1u
#define CLOCK_DIG_DBIAS_S        11
#define CLOCK_DIG_DBIAS_M        (0x7u << CLOCK_DIG_DBIAS_S)
#define CLOCK_DBIAS_1V00         2u
#define CLOCK_DBIAS_1V10         4u
#define CLOCK_DBIAS_1V25         7u
#define CLOCK_ANA_I2C_BBPLL_EN   (1u << 17)

/* RTC_CNTL_OPTIONS0 : mises hors tension forcées du PLL et de son bus. */
#define CLOCK_BB_I2C_FORCE_PD     (1u << 6)
#define CLOCK_BBPLL_I2C_FORCE_PD  (1u << 8)
#define CLOCK_BBPLL_FORCE_PD      (1u << 10)
#define CLOCK_BIAS_I2C_FORCE_PD   (1u << 18)

/* Bus I2C analogique : mot écrit dans CLOCK_ANA_I2C_HOST_REG(hôte). */
#define CLOCK_ANA_I2C_WRITE      (1u << 24)
#define CLOCK_ANA_I2C_BUSY       (1u << 25)

/* PLL (regi2c_bbpll.h) : bloc 0x66, hôte 4, et ses registres. */
#define CLOCK_I2C_BBPLL           0x66u
#define CLOCK_I2C_BBPLL_HOSTID    4u
#define CLOCK_BBPLL_IR_CAL_DELAY  0u
#define CLOCK_BBPLL_IR_CAL_EXT_CAP 1u
#define CLOCK_BBPLL_OC_LREF       2u
#define CLOCK_BBPLL_OC_DIV_7_0    3u
#define CLOCK_BBPLL_OC_ENB_FCAL   4u
#define CLOCK_BBPLL_OC_DCUR       5u
#define CLOCK_BBPLL_BBADC_DSMP    9u
#define CLOCK_BBPLL_OC_ENB_VCON   10u
#define CLOCK_BBPLL_ENDIV5        11u
#define CLOCK_BBPLL_BBADC_CAL_7_0 12u

/* Gel d'un cœur (cpu_utility_ll.h) : 2 dans le champ C0, 0x21 dans C1. */
#define CLOCK_STALL_PROCPU_C0_S  2
#define CLOCK_STALL_PROCPU_C1_S  26

/* Délais d'ESP-IDF (soc/rtc.h), en µs. Une période de l'horloge lente
 * (150 kHz, ~7 µs) est attendue avec une marge : CLOCK_SLOW_CYCLE_US. */
#define CLOCK_DELAY_DBIAS_RAISE_US  3u
#define CLOCK_DELAY_PLL_ENABLE_US   160u
#define CLOCK_SLOW_CYCLE_US         20u
#define CLOCK_DELAY_AFTER_PLL_US    30u

/* Budget d'attente du bus I2C analogique, en cycles (à 40 MHz : 1 ms). */
#define CLOCK_ANA_I2C_BUDGET     40000u

/* 1 si la fréquence mhz est atteignable (voir la liste ci-dessus). */
int clock_cpu_mhz_supported(uint32_t mhz);

/* Fréquence CPU actuelle, lue dans les registres (pas mesurée). 0 si la
 * configuration n'est pas reconnue (source RC ou APLL). */
uint32_t clock_get_cpu_mhz(void);

/* Bascule le CPU (les deux cœurs) à mhz MHz et met dans *apb_hz la
 * nouvelle fréquence du bus APB. 0, ou -1 si mhz n'est pas supportée ou
 * si le PLL ne répond pas (le CPU reste alors sur le quartz à 40 MHz).
 *
 * Bas niveau : n'adapte ni l'UART, ni le temps, ni le SPI. À appeler
 * interruptions masquées, l'autre cœur gelé (clock_stall_core) et l'UART
 * au repos. time_set_cpu_mhz fait tout cela. */
int clock_set_cpu_mhz(uint32_t mhz, uint32_t *apb_hz);

/* Gèle ou relance un cœur (0 : PRO, 1 : APP). Ne pas geler le cœur courant. */
void clock_stall_core(uint32_t core);
void clock_unstall_core(uint32_t core);

#endif /* CLOCK_H */
