/*
 * ORI Deterministic Shield — Pure C11 Safety Kernel
 * License: Apache-2.0 (see LICENSE-APACHE)
 *
 * Principle: "AI Proposes, Deterministic C Disposes."
 * Zero heap allocation, zero floating-point requirement, MISRA-style fixed-width types.
 */
#ifndef ORI_SHIELD_H
#define ORI_SHIELD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fault bitmask flags (latched until physical conditions are safe and reset is requested) */
#define ORI_FAULT_NONE          (0U)
#define ORI_FAULT_THERMAL       (1U << 0)
#define ORI_FAULT_ESTOP         (1U << 1)
#define ORI_FAULT_WATCHDOG      (1U << 2)
#define ORI_FAULT_SENSOR_RANGE  (1U << 3)

/* Sentinel indicating an invalid / uncalibrated Q15.1 sensor reading */
#define ORI_TEMP_INVALID_Q15_1  ((int16_t)-32768)

/* Default NUCLEO-F401RE safe operating envelope */
#define ORI_DEFAULT_MIN_U                 ((int32_t)0)
#define ORI_DEFAULT_MAX_U                 ((int32_t)1000)
#define ORI_DEFAULT_MAX_SLEW_STEP         ((int32_t)100)
#define ORI_DEFAULT_MAX_TEMP_Q15_1        ((int16_t)120)   /* 60.0 C (0.5 C/LSB) */
#define ORI_DEFAULT_HYSTERESIS_Q15_1      ((int16_t)10)    /* 5.0 C recovery margin */
#define ORI_DEFAULT_MIN_VALID_TEMP_Q15_1  ((int16_t)-80)   /* -40.0 C */
#define ORI_DEFAULT_MAX_VALID_TEMP_Q15_1  ((int16_t)400)   /* +200.0 C */

typedef struct {
    int32_t min_u;                    /* Minimum safe actuator command (e.g. 0 PWM) */
    int32_t max_u;                    /* Maximum safe actuator command (e.g. 1000 PWM) */
    int32_t max_slew_step;            /* Maximum allowed |delta_u| per control tick */
    int32_t safe_fallback_u;          /* Deterministic output forced while faulted (typically 0) */
    int16_t max_temp_q15_1;           /* Thermal cutoff threshold in Q15.1 (0.5 C/LSB) */
    int16_t thermal_hysteresis_q15_1; /* Margin below cutoff required before fault clear */
    int16_t min_valid_temp_q15_1;     /* Lower plausible sensor bound (detect open circuit) */
    int16_t max_valid_temp_q15_1;     /* Upper plausible sensor bound (detect short circuit) */
} ori_shield_config_t;

typedef struct {
    int32_t  last_u;                  /* Previous applied actuator command */
    int32_t  last_proposed_u;         /* Last AI/host proposed setpoint */
    int16_t  last_temp_q15_1;         /* Last sampled temperature in Q15.1 (0.5 C/LSB) */
    uint32_t fault_flags;             /* Latched fault bitmask (ORI_FAULT_*) */
    uint32_t tick_count;              /* Monotonic control-loop step counter */
    bool     clamped_last_tick;       /* True if proposal exceeded ceiling/floor/slew limit */
} ori_shield_state_t;

/* Populate config with safe default bounds (60.0 C cutoff, 0..1000 PWM, max slew 100/tick). */
void ori_shield_config_default(ori_shield_config_t *cfg);

/* Initialize runtime state cleanly (optionally latching ORI_FAULT_WATCHDOG after IWDG reset). */
void ori_shield_init(ori_shield_state_t *state, bool watchdog_reset_detected);

/*
 * Convert raw 12-bit STM32F401RE ADC1_IN18 temperature reading into Q15.1 (0.5 C/LSB)
 * using factory OTP calibration words TS_CAL1 (30 C) and TS_CAL2 (110 C) in pure int32_t math.
 * Returns ORI_TEMP_INVALID_Q15_1 if ts_cal2 <= ts_cal1 (divide-by-zero guard).
 */
int16_t ori_shield_adc_to_q15_1(uint16_t adc_raw, uint16_t ts_cal1, uint16_t ts_cal2);

/*
 * Execute one deterministic control-envelope step.
 * Always returns the safe actuator output to apply to physical hardware.
 */
int32_t ori_shield_step(
    ori_shield_state_t *state,
    const ori_shield_config_t *cfg,
    int32_t proposed_u,
    int16_t temp_q15_1,
    bool estop_pressed
);

/*
 * Attempt to clear latched faults. Succeeds (returns true) ONLY if E-Stop is released,
 * sensor reading is in valid range, and temperature is below (max_temp - hysteresis).
 */
bool ori_shield_clear_faults(
    ori_shield_state_t *state,
    const ori_shield_config_t *cfg,
    int16_t temp_q15_1,
    bool estop_pressed
);

/* Compute standard CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) for telemetry framing. */
uint16_t ori_shield_crc16(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ORI_SHIELD_H */
