#include "arch/xtensa_lx6/drivers/esp32_clock/clock.h"

typedef char clock_hz_per_cycle_must_be_exact[(CLOCK_UART_BAUD % (CLOCK_TIMED_BYTES * CLOCK_BITS_PER_BYTE)) == 0 ? 1 : -1];

static uint32_t txfifo_count(void) {
    return (UART0_STATUS >> 16) & 0xFFu;
}

static uint32_t wait_txfifo_empty(uint32_t budget, int *ok) {
    uint32_t start = cpu_read_ccount();
    while (txfifo_count() != 0) {
        if (cpu_read_ccount() - start > budget) { *ok = 0; return 0; }
    }
    return cpu_read_ccount();
}

uint32_t clock_measure_cpu_hz(const char *label) {
    if (label == NULL || label[0] == '\0') return 0;

    const uint32_t budget = 0x80000000u;
    int ok = 1;

    for (const char *p = label; *p; p++) {
        while (txfifo_count() >= 128u) { }
        UART0_FIFO = (uint32_t)*p;
    }
    uint32_t t0 = wait_txfifo_empty(budget, &ok);

    for (uint32_t i = 0; i < CLOCK_TIMED_BYTES; i++) {
        UART0_FIFO = (uint32_t)'.';
    }
    uint32_t t1 = wait_txfifo_empty(budget, &ok);

    if (!ok) return 0;

    uint32_t cycles = t1 - t0;
    if (cycles == 0 || cycles > 0xFFFFFFFFu / CLOCK_HZ_PER_CYCLE) return 0;

    return clock_snap_hz(cycles * CLOCK_HZ_PER_CYCLE);
}

uint32_t clock_snap_hz(uint32_t measured_hz) {
    static const uint32_t standard_mhz[] = { 240, 160, 80, 40, 26, 20, 10 };

    for (uint32_t i = 0; i < sizeof(standard_mhz) / sizeof(standard_mhz[0]); i++) {
        uint32_t ref = standard_mhz[i] * 1000000u;
        uint32_t diff = measured_hz > ref ? measured_hz - ref : ref - measured_hz;
        if (diff < ref / 25u) return ref;
    }

    uint32_t mhz = measured_hz / 1000000u + ((measured_hz % 1000000u) >= 500000u ? 1u : 0u);
    if (mhz > 0xFFFFFFFFu / 1000000u) mhz = 0xFFFFFFFFu / 1000000u;
    return mhz * 1000000u;
}

static void reg_set_field(uint32_t reg, uint32_t mask, uint32_t shift, uint32_t value) {
    REG32(reg) = (REG32(reg) & ~mask) | ((value << shift) & mask);
}

static void wait_us(uint32_t us, uint32_t mhz) {
    uint32_t cycles = us * mhz;
    uint32_t start = cpu_read_ccount();
    while (cpu_read_ccount() - start < cycles) { }
}

static int ana_i2c_write(uint8_t block, uint8_t host, uint8_t reg, uint8_t data) {
    volatile uint32_t *r = (volatile uint32_t *)CLOCK_ANA_I2C_HOST_REG(host);
    *r = (uint32_t)block | ((uint32_t)reg << 8) | ((uint32_t)data << 16) | CLOCK_ANA_I2C_WRITE;

    uint32_t start = cpu_read_ccount();
    while (*r & CLOCK_ANA_I2C_BUSY) {
        if (cpu_read_ccount() - start > CLOCK_ANA_I2C_BUDGET) return -1;
    }
    return 0;
}

static int bbpll_write(uint8_t reg, uint8_t data) {
    return ana_i2c_write((uint8_t)CLOCK_I2C_BBPLL, (uint8_t)CLOCK_I2C_BBPLL_HOSTID, reg, data);
}

static uint32_t cpu_src(void) {
    return (REG32(CLOCK_RTC_CNTL_CLK_CONF_REG) & CLOCK_SOC_CLK_SEL_M) >> CLOCK_SOC_CLK_SEL_S;
}

int clock_cpu_mhz_supported(uint32_t mhz) {
    if (mhz == 240u || mhz == 160u || mhz == 80u) return 1;
    return mhz > 0u && mhz <= CLOCK_XTAL_MHZ && CLOCK_XTAL_MHZ % mhz == 0u;
}

uint32_t clock_get_cpu_mhz(void) {
    uint32_t src = cpu_src();
    if (src == CLOCK_SRC_XTAL) {
        return CLOCK_XTAL_MHZ / ((REG32(CLOCK_SYSCON_SYSCLK_CONF_REG) & 0x3FFu) + 1u);
    }
    if (src == CLOCK_SRC_PLL) {
        switch (REG32(CLOCK_DPORT_CPU_PER_CONF_REG) & 0x3u) {
            case 0: return 80u;
            case 1: return 160u;
            case 2: return 240u;
            default: return 0u;
        }
    }
    return 0u;
}

static void to_xtal(uint32_t mhz, uint32_t div) {
    reg_set_field(CLOCK_SYSCON_SYSCLK_CONF_REG, 0x3FFu, 0, div - 1u);
    REG32(CLOCK_SYSCON_XTAL_TICK_REG) = mhz - 1u;          /* REF_TICK à 1 MHz */
    reg_set_field(CLOCK_RTC_CNTL_CLK_CONF_REG, CLOCK_SOC_CLK_SEL_M, CLOCK_SOC_CLK_SEL_S, CLOCK_SRC_XTAL);
    reg_set_field(CLOCK_RTC_CNTL_REG, CLOCK_DIG_DBIAS_M, CLOCK_DIG_DBIAS_S,
                  mhz <= 2u ? CLOCK_DBIAS_1V00 : CLOCK_DBIAS_1V10);
}

static void bbpll_disable(void) {
    REG32(RTC_CNTL_OPTIONS0_REG) |= CLOCK_BB_I2C_FORCE_PD | CLOCK_BBPLL_FORCE_PD | CLOCK_BBPLL_I2C_FORCE_PD;
}

static int bbpll_start(uint32_t pll_mhz) {
    REG32(RTC_CNTL_OPTIONS0_REG) &= ~(CLOCK_BIAS_I2C_FORCE_PD | CLOCK_BB_I2C_FORCE_PD |
    CLOCK_BBPLL_FORCE_PD | CLOCK_BBPLL_I2C_FORCE_PD);
    REG32(CLOCK_ANA_CONFIG_REG) &= ~CLOCK_ANA_I2C_BBPLL_EN;

    if (bbpll_write(CLOCK_BBPLL_IR_CAL_DELAY, 0x18) != 0 ||
        bbpll_write(CLOCK_BBPLL_IR_CAL_EXT_CAP, 0x20) != 0 ||
        bbpll_write(CLOCK_BBPLL_OC_ENB_FCAL, 0x9A) != 0 ||
        bbpll_write(CLOCK_BBPLL_OC_ENB_VCON, 0x00) != 0 ||
        bbpll_write(CLOCK_BBPLL_BBADC_CAL_7_0, 0x00) != 0) return -1;
    wait_us(CLOCK_SLOW_CYCLE_US, CLOCK_XTAL_MHZ);

    uint8_t endiv5, dsmp, div7_0;
    if (pll_mhz == 320u) {
        reg_set_field(CLOCK_RTC_CNTL_REG, CLOCK_DIG_DBIAS_M, CLOCK_DIG_DBIAS_S, CLOCK_DBIAS_1V10);
        endiv5 = 0x43; dsmp = 0x84; div7_0 = 32;
    } else {
        reg_set_field(CLOCK_RTC_CNTL_REG, CLOCK_DIG_DBIAS_M, CLOCK_DIG_DBIAS_S, CLOCK_DBIAS_1V25);
        wait_us(CLOCK_DELAY_DBIAS_RAISE_US, CLOCK_XTAL_MHZ);
        endiv5 = 0xC3; dsmp = 0x74; div7_0 = 28;
    }
    if (bbpll_write(CLOCK_BBPLL_ENDIV5, endiv5) != 0 ||
        bbpll_write(CLOCK_BBPLL_BBADC_DSMP, dsmp) != 0 ||
        bbpll_write(CLOCK_BBPLL_OC_LREF, 0x00) != 0 ||
        bbpll_write(CLOCK_BBPLL_OC_DIV_7_0, div7_0) != 0 ||
        bbpll_write(CLOCK_BBPLL_OC_DCUR, (uint8_t)((3u << 6) | 6u)) != 0) return -1;

        wait_us(CLOCK_DELAY_PLL_ENABLE_US, CLOCK_XTAL_MHZ);
    return 0;
}

static void to_pll(uint32_t mhz) {
    reg_set_field(CLOCK_RTC_CNTL_REG, CLOCK_DIG_DBIAS_M, CLOCK_DIG_DBIAS_S,
                  mhz == 240u ? CLOCK_DBIAS_1V25 : CLOCK_DBIAS_1V10);
    REG32(CLOCK_DPORT_CPU_PER_CONF_REG) = (mhz == 80u) ? 0u : (mhz == 160u) ? 1u : 2u;
    REG32(CLOCK_SYSCON_PLL_TICK_REG) = 80u - 1u;
    reg_set_field(CLOCK_RTC_CNTL_CLK_CONF_REG, CLOCK_SOC_CLK_SEL_M, CLOCK_SOC_CLK_SEL_S, CLOCK_SRC_PLL);
    wait_us(CLOCK_SLOW_CYCLE_US + CLOCK_DELAY_AFTER_PLL_US, mhz);
}

int clock_set_cpu_mhz(uint32_t mhz, uint32_t *apb_hz) {
    if (!clock_cpu_mhz_supported(mhz)) return -1;

    uint32_t old_src = cpu_src();
    if (old_src != CLOCK_SRC_XTAL) {
        to_xtal(CLOCK_XTAL_MHZ, 1u);
        wait_us(CLOCK_SLOW_CYCLE_US, CLOCK_XTAL_MHZ);
    }
    if (old_src == CLOCK_SRC_PLL) bbpll_disable();

    if (mhz <= CLOCK_XTAL_MHZ) {
        to_xtal(mhz, CLOCK_XTAL_MHZ / mhz);
        if (apb_hz) *apb_hz = mhz * 1000000u;
        return 0;
    }

    if (bbpll_start(mhz == 240u ? 480u : 320u) != 0) {
        bbpll_disable();
        to_xtal(CLOCK_XTAL_MHZ, 1u);
        if (apb_hz) *apb_hz = CLOCK_XTAL_MHZ * 1000000u;
        return -1;
    }
    to_pll(mhz);
    if (apb_hz) *apb_hz = 80000000u;
    return 0;
}

void clock_stall_core(uint32_t core) {
    uint32_t c0_s = core == 0 ? CLOCK_STALL_PROCPU_C0_S : RTC_CNTL_SW_STALL_APPCPU_C0_S;
    uint32_t c1_s = core == 0 ? CLOCK_STALL_PROCPU_C1_S : RTC_CNTL_SW_STALL_APPCPU_C1_S;
    reg_set_field(RTC_CNTL_OPTIONS0_REG, 0x3u << c0_s, c0_s, 0x2u);
    reg_set_field(RTC_CNTL_SW_CPU_STALL_REG, 0x3Fu << c1_s, c1_s, 0x21u);
}

void clock_unstall_core(uint32_t core) {
    uint32_t c0_s = core == 0 ? CLOCK_STALL_PROCPU_C0_S : RTC_CNTL_SW_STALL_APPCPU_C0_S;
    uint32_t c1_s = core == 0 ? CLOCK_STALL_PROCPU_C1_S : RTC_CNTL_SW_STALL_APPCPU_C1_S;
    REG32(RTC_CNTL_OPTIONS0_REG) &= ~(0x3u << c0_s);
    REG32(RTC_CNTL_SW_CPU_STALL_REG) &= ~(0x3Fu << c1_s);
}
