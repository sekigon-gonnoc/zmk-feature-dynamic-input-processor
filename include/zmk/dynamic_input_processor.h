/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

struct device;

#define DYNAMIC_POINTER_CURVE_POINTS 3

enum dynamic_pointer_mode {
    DYNAMIC_POINTER_MODE_CURSOR,
    DYNAMIC_POINTER_MODE_SCROLL,
    DYNAMIC_POINTER_MODE_ENCODER,
};

struct dynamic_input_processor_params {
    int16_t rotation_deg;
    uint32_t fine_layers;
    uint8_t fine_coef;
    uint32_t rough_layers;
    uint8_t rough_coef;
    uint8_t shift_points[DYNAMIC_POINTER_CURVE_POINTS];
    uint8_t shift_rates[DYNAMIC_POINTER_CURVE_POINTS];
    uint32_t scroll_layers;
    bool scroll_invert;
    bool scroll_snap;
    uint8_t scroll_divisor;
    uint32_t encoder_layers;
    uint32_t encoder_snap_layers;
    uint8_t encoder_divisor;
    bool aml_enabled;
    uint8_t aml_layer;
    uint16_t aml_timeout_ms;
    uint8_t aml_debounce_ms;
    uint8_t aml_threshold;
    uint16_t aml_delay_ms;
};

struct dynamic_pointer_data {
    const struct device *dev;
    struct k_work_delayable aml_work;
    struct k_work_delayable frame_work;
    struct k_spinlock lock;
    struct dynamic_input_processor_params params;
    int32_t pending_x;
    int32_t pending_y;
    uint16_t input_count_peak;
    bool pending_motion;
    int32_t rotation_cos_q15;
    int32_t rotation_sin_q15;
    int32_t cursor_remainder_x;
    int32_t cursor_remainder_y;
    int32_t rotation_remainder_x;
    int32_t rotation_remainder_y;
    int32_t scroll_remainder_x;
    int32_t scroll_remainder_y;
    int32_t encoder_remainder_x;
    int32_t encoder_remainder_y;
    uint8_t temp_scroll_count;
    uint8_t temp_encoder_counts[CONFIG_ZMK_DYNAMIC_INPUT_PROCESSOR_POINTER_ENCODER_COUNT];
    int32_t aml_accumulated;
    int64_t aml_last_nonmouse_ms;
    int64_t aml_debounce_until_ms;
    bool aml_activation_pending;
    bool aml_owned_layer;
    uint8_t aml_owned_layer_id;
    uint8_t snap_count;
    uint8_t snap_history;
    uint8_t snap_state;
    uint8_t last_layer;
    enum dynamic_pointer_mode last_mode;
    uint8_t last_encoder_index;
    int64_t last_motion_ms;
};

int dynamic_input_processor_params_validate(const struct dynamic_input_processor_params *params);
int dynamic_input_processor_settings_after_change(const struct device *dev);
uint32_t
dynamic_input_processor_shift_curve_q16(const struct dynamic_input_processor_params *params,
                                        uint64_t norm);
int dynamic_pointer_set_temp_mode(const struct device *dev, enum dynamic_pointer_mode mode,
                                  uint32_t encoder_index, bool pressed);

/* Q16 scalar with signed fractional carry and int16 output saturation. */
int dynamic_input_processor_scale_q16(int32_t input, uint32_t factor_q16, int32_t *remainder,
                                      int16_t *output);
int dynamic_input_processor_divide_q16(int32_t input, uint8_t divisor, int32_t *remainder,
                                       int16_t *output);
