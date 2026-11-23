/*
 * ORI Deterministic Shield — Pure C11 Safety Kernel Implementation
 * License: Apache-2.0 (see LICENSE-APACHE)
 */
#include "ori_shield.h"

void ori_shield_config_default(ori_shield_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->min_u = ORI_DEFAULT_MIN_U;
    cfg->max_u = ORI_DEFAULT_MAX_U;
    cfg->max_slew_step = ORI_DEFAULT_MAX_SLEW_STEP;
    cfg->safe_fallback_u = 0;
    cfg->max_temp_q15_1 = ORI_DEFAULT_MAX_TEMP_Q15_1;
    cfg->thermal_hysteresis_q15_1 = ORI_DEFAULT_HYSTERESIS_Q15_1;
    cfg->min_valid_temp_q15_1 = ORI_DEFAULT_MIN_VALID_TEMP_Q15_1;
    cfg->max_valid_temp_q15_1 = ORI_DEFAULT_MAX_VALID_TEMP_Q15_1;
}

void ori_shield_init(ori_shield_state_t *state, bool watchdog_reset_detected)
{
    if (state == NULL) {
        return;
    }
    state->last_u = 0;
    state->last_proposed_u = 0;
    state->last_temp_q15_1 = 0;
    state->fault_flags = watchdog_reset_detected ? ORI_FAULT_WATCHDOG : ORI_FAULT_NONE;
    state->tick_count = 0U;
    state->clamped_last_tick = false;
}

int16_t ori_shield_adc_to_q15_1(uint16_t adc_raw, uint16_t ts_cal1, uint16_t ts_cal2)
{
    if (ts_cal2 <= ts_cal1) {
        return ORI_TEMP_INVALID_Q15_1;
    }
    /*
     * Linear two-point interpolation in Q15.1 (0.5 C/LSB):
     *   30.0 C = 60 Q15.1, 110.0 C = 220 Q15.1, span = 160 Q15.1
     *   T_q15_1 = 60 + (160 * (adc_raw - ts_cal1)) / (ts_cal2 - ts_cal1)
     */
    const int32_t delta_adc = (int32_t)adc_raw - (int32_t)ts_cal1;
    const int32_t cal_span = (int32_t)ts_cal2 - (int32_t)ts_cal1;
    int32_t t_q15_1 = 60 + ((160 * delta_adc) / cal_span);

    if (t_q15_1 <= (int32_t)ORI_TEMP_INVALID_Q15_1) {
        t_q15_1 = (int32_t)ORI_TEMP_INVALID_Q15_1 + 1;
    } else if (t_q15_1 > 32767) {
        t_q15_1 = 32767;
    }
    return (int16_t)t_q15_1;
}

static bool is_sensor_reading_valid(const ori_shield_config_t *cfg, int16_t temp_q15_1)
{
    if (temp_q15_1 == ORI_TEMP_INVALID_Q15_1) {
        return false;
    }
    return (temp_q15_1 >= cfg->min_valid_temp_q15_1) &&
           (temp_q15_1 <= cfg->max_valid_temp_q15_1);
}

int32_t ori_shield_step(
    ori_shield_state_t *state,
    const ori_shield_config_t *cfg,
    int32_t proposed_u,
    int16_t temp_q15_1,
    bool estop_pressed
)
{
    int32_t bounded_u;
    int32_t next_u;

    if ((state == NULL) || (cfg == NULL)) {
        return 0;
    }

    state->tick_count += 1U;
    state->last_proposed_u = proposed_u;
    state->last_temp_q15_1 = temp_q15_1;
    state->clamped_last_tick = false;

    /* 1. Hardware & physical safety fault checks (all faults latch deterministically) */
    if (estop_pressed) {
        state->fault_flags |= ORI_FAULT_ESTOP;
    }
    if (!is_sensor_reading_valid(cfg, temp_q15_1)) {
        state->fault_flags |= ORI_FAULT_SENSOR_RANGE;
    } else if (temp_q15_1 >= cfg->max_temp_q15_1) {
        state->fault_flags |= ORI_FAULT_THERMAL;
    }

    /* 2. Any active latched fault forces immediate deterministic fallback */
    if (state->fault_flags != ORI_FAULT_NONE) {
        state->last_u = cfg->safe_fallback_u;
        state->clamped_last_tick = (proposed_u != cfg->safe_fallback_u);
        return state->last_u;
    }

    /* 3. Absolute ceiling / floor clamp */
    bounded_u = proposed_u;
    if (bounded_u < cfg->min_u) {
        bounded_u = cfg->min_u;
        state->clamped_last_tick = true;
    } else if (bounded_u > cfg->max_u) {
        bounded_u = cfg->max_u;
        state->clamped_last_tick = true;
    }

    /* 4. Symmetric slew-rate limiter (overflow-safe difference using int64_t) */
    next_u = bounded_u;
    if (cfg->max_slew_step > 0) {
        const int64_t delta = (int64_t)bounded_u - (int64_t)state->last_u;
        const int64_t limit = (int64_t)cfg->max_slew_step;
        if (delta > limit) {
            next_u = state->last_u + cfg->max_slew_step;
            state->clamped_last_tick = true;
        } else if (delta < -limit) {
            next_u = state->last_u - cfg->max_slew_step;
            state->clamped_last_tick = true;
        }
    }

    state->last_u = next_u;
    return next_u;
}

bool ori_shield_clear_faults(
    ori_shield_state_t *state,
    const ori_shield_config_t *cfg,
    int16_t temp_q15_1,
    bool estop_pressed
)
{
    int32_t safe_recovery_temp;

    if ((state == NULL) || (cfg == NULL)) {
        return false;
    }
    if (estop_pressed || !is_sensor_reading_valid(cfg, temp_q15_1)) {
        return false;
    }

    safe_recovery_temp = (int32_t)cfg->max_temp_q15_1 - (int32_t)cfg->thermal_hysteresis_q15_1;
    if ((int32_t)temp_q15_1 >= safe_recovery_temp) {
        return false;
    }

    state->fault_flags = ORI_FAULT_NONE;
    return true;
}

uint16_t ori_shield_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;
    size_t i;

    if (data == NULL) {
        return crc;
    }
    for (i = 0U; i < len; ++i) {
        uint8_t bit;
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}
