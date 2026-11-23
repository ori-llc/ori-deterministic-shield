/*
 * ORI Deterministic Shield — Bare-Metal STM32 NUCLEO-F401RE Target (v0.1)
 * License: Apache-2.0 (see LICENSE-APACHE)
 *
 * Zero external wiring required on Day 1:
 *   - Clock:      Default 16 MHz internal HSI (no external X3 crystal required)
 *   - Serial:     USART2 on PA2 (TX) / PA3 (RX) via ST-LINK/V2-1 USB VCP @ 115200 8N1
 *   - Temp:       On-chip ADC1_IN18 (RM0368 / AN4646: TSVREFE=1, explicit VBATE=0, 480 cycles)
 *                 converted to pure int32_t Q15.1 (0.5 C/LSB) from TS_CAL1/TS_CAL2
 *   - LED:        LD2 on PA5 (heartbeat in normal operation, rapid blink when fault latched)
 *   - E-Stop:     Blue User Button B1 on PC13 (active-low physical Emergency-Stop)
 *   - Watchdog:   Hardware IWDG (~3.2 s timeout); send "TEST_HANG" over serial to demo live reset
 *   - Profiling:  DWT->CYCCNT unlocked via CoreDebug->DEMCR TRCENA (bit 24)
 *
 * Commands (115200 8N1, newline-terminated):
 *   SET <int>   -> Propose a new actuator setpoint (e.g. "SET 400" or "SET 9999")
 *   RESET       -> Attempt to clear latched faults (succeeds only when E-Stop & temp are safe)
 *   STATUS      -> Immediately emit a telemetry frame
 *   TEST_HANG   -> Enter deliberate infinite loop to demonstrate hardware IWDG recovery
 */
#include "ori_shield.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── STM32F401RE Register Definitions (RM0368) ──────────────────────────── */
#define REG32(addr)        (*((volatile uint32_t *)(uintptr_t)(addr)))
#define REG16_RO(addr)     (*((const volatile uint16_t *)(uintptr_t)(addr)))

/* RCC (0x40023800) */
#define RCC_AHB1ENR        REG32(0x40023830UL)
#define RCC_APB1ENR        REG32(0x40023840UL)
#define RCC_APB2ENR        REG32(0x40023844UL)
#define RCC_CSR            REG32(0x40023874UL)
#define RCC_CSR_RMVF       (1UL << 24)
#define RCC_CSR_IWDGRSTF   (1UL << 29)

/* GPIOA (0x40020000) & GPIOC (0x40020800) */
#define GPIOA_MODER        REG32(0x40020000UL)
#define GPIOA_BSRR         REG32(0x40020018UL)
#define GPIOA_AFRL         REG32(0x40020020UL)
#define GPIOC_MODER        REG32(0x40020800UL)
#define GPIOC_IDR          REG32(0x40020810UL)

/* USART2 (0x40004400) — ST-LINK/V2-1 Virtual COM Port on PA2/PA3 */
#define USART2_SR          REG32(0x40004400UL)
#define USART2_DR          REG32(0x40004404UL)
#define USART2_BRR         REG32(0x40004408UL)
#define USART2_CR1         REG32(0x4000440CUL)
#define USART_SR_RXNE      (1UL << 5)
#define USART_SR_TXE       (1UL << 7)

/* ADC1 (0x40012000) & Common Control Register (0x40012304) */
#define ADC1_SR            REG32(0x40012000UL)
#define ADC1_CR2           REG32(0x40012008UL)
#define ADC1_SMPR1         REG32(0x4001200CUL)
#define ADC1_SQR1          REG32(0x4001202CUL)
#define ADC1_SQR3          REG32(0x40012034UL)
#define ADC1_DR            REG32(0x4001204CUL)
#define ADC_CCR            REG32(0x40012304UL)
#define ADC_SR_EOC         (1UL << 1)
#define ADC_CR2_ADON       (1UL << 0)
#define ADC_CR2_SWSTART    (1UL << 30)
#define ADC_CCR_VBATE      (1UL << 22)
#define ADC_CCR_TSVREFE    (1UL << 23)

/* STM32F401xE Factory Temperature Calibration OTP Addresses (DS10086) */
#define STM32_TS_CAL1      REG16_RO(0x1FFF7A2CUL) /* 30 C @ 3.3 V */
#define STM32_TS_CAL2      REG16_RO(0x1FFF7A2EUL) /* 110 C @ 3.3 V */

/* ARM Cortex-M4 CoreDebug, DWT Cycle Counter & SysTick */
#define COREDEBUG_DEMCR    REG32(0xE000EDFCUL)
#define DWT_CTRL           REG32(0xE0001000UL)
#define DWT_CYCCNT         REG32(0xE0001004UL)
#define SYST_CSR           REG32(0xE000E010UL)
#define SYST_RVR           REG32(0xE000E014UL)
#define SYST_CVR           REG32(0xE000E018UL)

/* Hardware Independent Watchdog (IWDG @ 0x40003000) */
#define IWDG_KR            REG32(0x40003000UL)
#define IWDG_PR            REG32(0x40003004UL)
#define IWDG_RLR           REG32(0x40003008UL)
#define IWDG_SR            REG32(0x4000300CUL)

/* ── Minimal Bare-Metal Drivers (Zero libc stdio dependency) ────────────── */
static void dwt_cyccnt_init(void)
{
    /* Enable TRCENA (bit 24) before CYCCNTENA (bit 0) so DWT->CYCCNT ticks on silicon */
    COREDEBUG_DEMCR |= (1UL << 24);
    DWT_CYCCNT = 0UL;
    DWT_CTRL |= 1UL;
}

static void systick_init_1ms_16mhz(void)
{
    /* 16 MHz HSI -> 16,000 cycles per 1.000 ms -> SYST_RVR = 15999U */
    SYST_RVR = 15999UL;
    SYST_CVR = 0UL;
    SYST_CSR = 0x05UL; /* ENABLE | CLKSOURCE (CPU) */
}

static void gpio_init(void)
{
    RCC_AHB1ENR |= (1UL << 0) | (1UL << 2); /* Enable GPIOA and GPIOC clocks */

    /* PA5 (LD2 Green LED) = General Purpose Output (01) */
    GPIOA_MODER = (GPIOA_MODER & ~(3UL << 10)) | (1UL << 10);

    /* PA2 (USART2_TX) & PA3 (USART2_RX) = Alternate Function AF7 */
    GPIOA_MODER = (GPIOA_MODER & ~(0xFUL << 4)) | (0xAUL << 4);
    GPIOA_AFRL = (GPIOA_AFRL & ~(0xFFUL << 8)) | (0x77UL << 8);

    /* PC13 (Blue Button B1, active-low E-Stop) = Input (00) */
    GPIOC_MODER &= ~(3UL << 26);
}

static void led_set(bool on)
{
    GPIOA_BSRR = on ? (1UL << 5) : (1UL << (5U + 16U));
}

static bool estop_button_pressed(void)
{
    return (GPIOC_IDR & (1UL << 13)) == 0UL;
}

static void usart2_init_115200_16mhz(void)
{
    RCC_APB1ENR |= (1UL << 17); /* Enable USART2 clock */
    /* 16 MHz / (16 * 115200) = 8.6805 -> Mantissa 8, Fraction 11 (0xB) -> 0x008B */
    USART2_BRR = 0x008BUL;
    USART2_CR1 = (1UL << 13) | (1UL << 3) | (1UL << 2); /* UE | TE | RE */
}

static void usart2_putc(char ch)
{
    while ((USART2_SR & USART_SR_TXE) == 0UL) {
        /* Wait for TX buffer empty */
    }
    USART2_DR = (uint32_t)(uint8_t)ch;
}

static void usart2_puts(const char *s)
{
    while (*s != '\0') {
        usart2_putc(*s++);
    }
}

static void adc1_tempsensor_init(void)
{
    RCC_APB2ENR |= (1UL << 8); /* Enable ADC1 clock */

    /*
     * RM0368 Section 11.9 / AN4646 (STM32F401 / F411):
     * Internal temperature sensor is wired to ADC1_IN18 (shared with VBAT).
     * Must set TSVREFE=1 AND explicitly clear VBATE=0, with ADCPRE=/2 (8 MHz ADC clock).
     */
    ADC_CCR = (ADC_CCR & ~((3UL << 16) | ADC_CCR_VBATE)) | ADC_CCR_TSVREFE;

    /* Channel 18 sample time = 480 cycles (60 us @ 8 MHz > 10 us minimum) */
    ADC1_SMPR1 |= (7UL << 24);
    ADC1_SQR1 &= ~(0xFUL << 20); /* 1 conversion in regular sequence */
    ADC1_SQR3 = 18UL;            /* Select ADC1_IN18 */

    ADC1_CR2 |= ADC_CR2_ADON;

    /* Brief startup stabilization (~50 us at 16 MHz) */
    for (volatile uint32_t i = 0U; i < 800U; ++i) {
        /* spin */
    }
}

static uint16_t adc1_read_raw(void)
{
    ADC1_CR2 |= ADC_CR2_SWSTART;
    while ((ADC1_SR & ADC_SR_EOC) == 0UL) {
        /* Wait for conversion complete */
    }
    return (uint16_t)(ADC1_DR & 0x0FFFU);
}

static void iwdg_init_3s(void)
{
    IWDG_KR = 0x5555UL; /* Unlock PR/RLR */
    IWDG_PR = 4UL;      /* LSI (~32 kHz) / 64 = ~500 Hz */
    IWDG_RLR = 1600UL;  /* ~3.2 s timeout */
    while ((IWDG_SR & 0x03UL) != 0UL) {
        /* Wait for register update */
    }
    IWDG_KR = 0xAAAAUL; /* Reload counter */
    IWDG_KR = 0xCCCCUL; /* Start hardware watchdog */
}

static void iwdg_kick(void)
{
    IWDG_KR = 0xAAAAUL;
}

/* ── Minimal Formatting & Command Parser Helpers ────────────────────────── */
static char *append_str(char *dst, const char *src)
{
    while (*src != '\0') {
        *dst++ = *src++;
    }
    return dst;
}

static char *append_u32(char *dst, uint32_t val)
{
    char tmp[12];
    size_t n = 0U;
    if (val == 0U) {
        *dst++ = '0';
        return dst;
    }
    while (val > 0U) {
        tmp[n++] = (char)('0' + (val % 10U));
        val /= 10U;
    }
    while (n > 0U) {
        *dst++ = tmp[--n];
    }
    return dst;
}

static char *append_i32(char *dst, int32_t val)
{
    if (val < 0) {
        *dst++ = '-';
        return append_u32(dst, (uint32_t)(-(int64_t)val));
    }
    return append_u32(dst, (uint32_t)val);
}

static char *append_hex16(char *dst, uint16_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    *dst++ = hex[(val >> 12) & 0x0FU];
    *dst++ = hex[(val >> 8) & 0x0FU];
    *dst++ = hex[(val >> 4) & 0x0FU];
    *dst++ = hex[val & 0x0FU];
    return dst;
}

static bool str_eq(const char *a, const char *b)
{
    while ((*a != '\0') && (*b != '\0')) {
        if (*a++ != *b++) {
            return false;
        }
    }
    return (*a == '\0') && (*b == '\0');
}

static bool parse_set_command(const char *line, int32_t *out_val)
{
    const char *p = line;
    bool neg = false;
    int64_t acc = 0;

    if ((p[0] != 'S') || (p[1] != 'E') || (p[2] != 'T') || (p[3] != ' ')) {
        return false;
    }
    p += 4;
    while (*p == ' ') {
        ++p;
    }
    if (*p == '-') {
        neg = true;
        ++p;
    } else if (*p == '+') {
        ++p;
    }
    if ((*p < '0') || (*p > '9')) {
        return false;
    }
    while ((*p >= '0') && (*p <= '9')) {
        acc = (acc * 10) + (*p - '0');
        if (acc > 1000000) {
            acc = 1000000;
        }
        ++p;
    }
    *out_val = (int32_t)(neg ? -acc : acc);
    return true;
}

static void emit_telemetry_frame(
    const ori_shield_state_t *st,
    uint32_t exec_cycles
)
{
    char buf[160];
    char *p = buf;
    uint16_t crc;

    p = append_str(p, "{\"tick\":");
    p = append_u32(p, st->tick_count);
    p = append_str(p, ",\"temp_q15_1\":");
    p = append_i32(p, (int32_t)st->last_temp_q15_1);
    p = append_str(p, ",\"proposed_u\":");
    p = append_i32(p, st->last_proposed_u);
    p = append_str(p, ",\"clamped_u\":");
    p = append_i32(p, st->last_u);
    p = append_str(p, ",\"faults\":");
    p = append_u32(p, st->fault_flags);
    p = append_str(p, ",\"cyccnt\":");
    p = append_u32(p, exec_cycles);
    p = append_str(p, "}");

    crc = ori_shield_crc16((const uint8_t *)buf, (size_t)(p - buf));
    p = append_str(p, "*0x");
    p = append_hex16(p, crc);
    p = append_str(p, "\r\n");
    *p = '\0';

    usart2_puts(buf);
}

int main(void)
{
    ori_shield_config_t cfg;
    ori_shield_state_t st;
    int32_t proposed_u = 0;
    uint32_t ms_ticks = 0U;
    uint32_t last_cycles = 0U;
    char rx_buf[48];
    size_t rx_len = 0U;

    const bool iwdg_reset = (RCC_CSR & RCC_CSR_IWDGRSTF) != 0UL;
    RCC_CSR |= RCC_CSR_RMVF; /* Clear reset flags for next boot */

    dwt_cyccnt_init();
    systick_init_1ms_16mhz();
    gpio_init();
    usart2_init_115200_16mhz();
    adc1_tempsensor_init();

    ori_shield_config_default(&cfg);
    ori_shield_init(&st, iwdg_reset);

    iwdg_init_3s();

    usart2_puts("\r\n[ORI-SHIELD v0.1] STM32F401RE 16MHz HSI | ADC1_IN18 Q15.1 | Commands: SET <n>, RESET, STATUS, TEST_HANG\r\n");
    if (iwdg_reset) {
        usart2_puts("[ORI-SHIELD] Hardware IWDG watchdog reset detected -> ORI_FAULT_WATCHDOG latched. Send RESET to recover.\r\n");
    }

    while (1) {
        iwdg_kick();

        /* 1. Non-blocking USART2 RX line collector */
        if ((USART2_SR & USART_SR_RXNE) != 0UL) {
            char ch = (char)(USART2_DR & 0xFFUL);
            if ((ch == '\r') || (ch == '\n')) {
                if (rx_len > 0U) {
                    int32_t parsed_u = 0;
                    rx_buf[rx_len] = '\0';
                    if (parse_set_command(rx_buf, &parsed_u)) {
                        proposed_u = parsed_u;
                    } else if (str_eq(rx_buf, "RESET")) {
                        uint16_t raw = adc1_read_raw();
                        int16_t t_q15 = ori_shield_adc_to_q15_1(raw, STM32_TS_CAL1, STM32_TS_CAL2);
                        if (ori_shield_clear_faults(&st, &cfg, t_q15, estop_button_pressed())) {
                            usart2_puts("[ORI-SHIELD] Fault latch cleared cleanly.\r\n");
                        } else {
                            usart2_puts("[ORI-SHIELD] RESET refused: physical E-Stop active or temperature above recovery margin.\r\n");
                        }
                    } else if (str_eq(rx_buf, "STATUS")) {
                        emit_telemetry_frame(&st, last_cycles);
                    } else if (str_eq(rx_buf, "TEST_HANG")) {
                        usart2_puts("[ORI-SHIELD] Entering deliberate hang — hardware IWDG will reset MCU in ~3.2s...\r\n");
                        while (1) {
                            /* Stop kicking IWDG to demonstrate hardware recovery */
                        }
                    }
                    rx_len = 0U;
                }
            } else if (rx_len < (sizeof(rx_buf) - 1U)) {
                rx_buf[rx_len++] = ch;
            }
        }

        /* 2. Deterministic 1.000 ms SysTick pacing */
        if ((SYST_CSR & (1UL << 16)) != 0UL) {
            ms_ticks += 1U;

            /* Sample & enforce safety shield at 10 Hz (every 100 ms) */
            if ((ms_ticks % 100U) == 0U) {
                uint16_t raw_adc = adc1_read_raw();
                int16_t temp_q15_1 = ori_shield_adc_to_q15_1(raw_adc, STM32_TS_CAL1, STM32_TS_CAL2);
                bool estop = estop_button_pressed();
                uint32_t c_start = DWT_CYCCNT;
                (void)ori_shield_step(&st, &cfg, proposed_u, temp_q15_1, estop);
                last_cycles = DWT_CYCCNT - c_start;

                /* PA5 LD2 LED: fast 5 Hz blink when faulted, slow 1 Hz heartbeat when nominal */
                if (st.fault_flags != ORI_FAULT_NONE) {
                    led_set(((ms_ticks / 100U) & 1U) != 0U);
                } else {
                    led_set(((ms_ticks / 500U) & 1U) != 0U);
                }

                /* Stream compact JSON+CRC telemetry at 2 Hz (every 500 ms) */
                if ((ms_ticks % 500U) == 0U) {
                    emit_telemetry_frame(&st, last_cycles);
                }
            }
        }
    }
}
