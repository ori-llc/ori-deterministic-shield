/*
 * ORI Deterministic Shield — Host C11 Fault-Injection & Overflow Test Suite
 * Compile & run via: make test
 */
#include "ori_shield.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_nominal_and_slew_rate_clamping(void)
{
    ori_shield_config_t cfg;
    ori_shield_state_t st;
    int32_t out;

    ori_shield_config_default(&cfg);
    ori_shield_init(&st, false);

    /* Room temperature 25.0 C -> Q15.1 = 50; AI proposes +500 -> slew-clamped to +100 */
    out = ori_shield_step(&st, &cfg, 500, 50, false);
    assert(out == 100);
    assert(st.clamped_last_tick == true);
    assert(st.fault_flags == ORI_FAULT_NONE);

    /* Next tick AI proposes 180 -> within +100 slew step, accepted cleanly */
    out = ori_shield_step(&st, &cfg, 180, 50, false);
    assert(out == 180);
    assert(st.clamped_last_tick == false);

    /* Hallucinated AI proposal 9999 -> slew-limited to 280, eventually clamped at ceiling 1000 */
    for (int i = 0; i < 12; ++i) {
        out = ori_shield_step(&st, &cfg, 9999, 50, false);
    }
    assert(out == 1000);
    assert(st.clamped_last_tick == true);

    /* Negative hallucinated AI proposal -5000 -> steps down by 100/tick and stops at floor 0 */
    for (int i = 0; i < 12; ++i) {
        out = ori_shield_step(&st, &cfg, -5000, 50, false);
    }
    assert(out == 0);
}

static void test_q15_1_overflow_immunity_and_thermal_latch(void)
{
    ori_shield_config_t cfg;
    ori_shield_state_t st;
    int32_t out;

    ori_shield_config_default(&cfg);
    ori_shield_init(&st, false);

    /* Ramp output to 100 at nominal 25.0 C */
    out = ori_shield_step(&st, &cfg, 100, 50, false);
    assert(out == 100);

    /*
     * Ariane-5 / Unit 1.1 regression test:
     * +135.0 C in Q15.1 is raw 270 (which wraps to +14 or negative if truncated to int8_t).
     * Pure int16_t Q15.1 MUST detect over-temperature and trip ORI_FAULT_THERMAL!
     */
    out = ori_shield_step(&st, &cfg, 400, 270, false);
    assert(out == 0);
    assert((st.fault_flags & ORI_FAULT_THERMAL) != 0U);

    /* Still latched even if temperature drops to 58.0 C (116 Q15.1, inside 5.0 C hysteresis band) */
    assert(ori_shield_clear_faults(&st, &cfg, 116, false) == false);
    out = ori_shield_step(&st, &cfg, 100, 116, false);
    assert(out == 0);

    /* Cooled to 50.0 C (100 Q15.1 < 110 Q15.1 recovery threshold): explicit clear succeeds */
    assert(ori_shield_clear_faults(&st, &cfg, 100, false) == true);
    assert(st.fault_flags == ORI_FAULT_NONE);
    out = ori_shield_step(&st, &cfg, 80, 100, false);
    assert(out == 80);
}

static void test_physical_estop_and_watchdog_latch(void)
{
    ori_shield_config_t cfg;
    ori_shield_state_t st;
    int32_t out;

    ori_shield_config_default(&cfg);

    /* Boot after hardware Independent Watchdog reset -> must latch ORI_FAULT_WATCHDOG */
    ori_shield_init(&st, true);
    assert((st.fault_flags & ORI_FAULT_WATCHDOG) != 0U);
    out = ori_shield_step(&st, &cfg, 100, 50, false);
    assert(out == 0);

    /* Explicit operator reset clears watchdog latch when temperature & E-Stop are safe */
    assert(ori_shield_clear_faults(&st, &cfg, 50, false) == true);
    out = ori_shield_step(&st, &cfg, 75, 50, false);
    assert(out == 75);

    /* Pressing physical blue button (PC13 E-Stop) forces immediate 0 output and latches */
    out = ori_shield_step(&st, &cfg, 75, 50, true);
    assert(out == 0);
    assert((st.fault_flags & ORI_FAULT_ESTOP) != 0U);

    /* Cannot clear while button is still pressed */
    assert(ori_shield_clear_faults(&st, &cfg, 50, true) == false);
    /* Release button + explicit reset restores control */
    assert(ori_shield_clear_faults(&st, &cfg, 50, false) == true);
    assert(st.fault_flags == ORI_FAULT_NONE);
}

static void test_adc_q15_1_calibration_and_sensor_range(void)
{
    ori_shield_config_t cfg;
    ori_shield_state_t st;

    ori_shield_config_default(&cfg);
    ori_shield_init(&st, false);

    /* Typical STM32F401RE TS_CAL1 (30 C) = 940, TS_CAL2 (110 C) = 1260 (span 320 -> 0.5 C per 2 LSB) */
    assert(ori_shield_adc_to_q15_1(940U, 940U, 1260U) == 60);   /* 30.0 C */
    assert(ori_shield_adc_to_q15_1(1260U, 940U, 1260U) == 220); /* 110.0 C */
    assert(ori_shield_adc_to_q15_1(1100U, 940U, 1260U) == 140); /* 70.0 C */

    /* Divide-by-zero / corrupt OTP guard */
    int16_t bad_temp = ori_shield_adc_to_q15_1(1000U, 1000U, 1000U);
    assert(bad_temp == ORI_TEMP_INVALID_Q15_1);
    assert(ori_shield_step(&st, &cfg, 50, bad_temp, false) == 0);
    assert((st.fault_flags & ORI_FAULT_SENSOR_RANGE) != 0U);
}

static void test_crc16_ccitt_false_standard_vector(void)
{
    /* Standard CRC-16/CCITT-FALSE check vector: ASCII "123456789" == 0x29B1 */
    const uint8_t vec[] = "123456789";
    uint16_t crc = ori_shield_crc16(vec, strlen((const char *)vec));
    assert(crc == 0x29B1U);
}

int main(void)
{
    test_nominal_and_slew_rate_clamping();
    test_q15_1_overflow_immunity_and_thermal_latch();
    test_physical_estop_and_watchdog_latch();
    test_adc_q15_1_calibration_and_sensor_range();
    test_crc16_ccitt_false_standard_vector();
    printf("[PASS] All ori-deterministic-shield C11 safety kernel tests passed.\n");
    return 0;
}
