/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT zmk_input_processor_pointer

#define DYNAMIC_POINTER_ENCODER_COUNT CONFIG_ZMK_DYNAMIC_INPUT_PROCESSOR_POINTER_ENCODER_COUNT

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <settings_schema_generated.h>
#include <zmk/behavior.h>
#include <zmk/dynamic_input_processor.h>
#include <zmk/dynamic_settings/store.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/keys.h>
#include <zmk/sensors.h>

struct dynamic_pointer_config {
    uint8_t type;
    uint16_t x_code;
    uint16_t y_code;
    struct dynamic_input_processor_params defaults;
    uint8_t encoder_sensor_indices[DYNAMIC_POINTER_ENCODER_COUNT * 2];
};

static int pointer_read_params(const struct device *dev,
                               struct dynamic_input_processor_params *params);
int dynamic_pointer_setting_read(const struct dynamic_setting_descriptor *descriptor,
                                 struct dynamic_setting_value *value, const void *context);
static void rotation_q15(int16_t degrees, int32_t *cosine, int32_t *sine);
static void pointer_process_frame(const struct device *dev,
                                  const struct dynamic_pointer_config *cfg, int32_t input_x,
                                  int32_t input_y);

#define POINTER_DEVICE_ENTRY(n) DEVICE_DT_INST_GET(n),
static const struct device *const pointer_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(POINTER_DEVICE_ENTRY) NULL};

static void aml_work_handler(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct dynamic_pointer_data *data =
        CONTAINER_OF(delayable, struct dynamic_pointer_data, aml_work);
    struct dynamic_input_processor_params params;
    if (pointer_read_params(data->dev, &params) != 0 || !params.aml_enabled) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    bool activate = data->aml_activation_pending;
    bool owned = data->aml_owned_layer;
    data->aml_activation_pending = false;
    k_spin_unlock(&data->lock, key);
    if (activate) {
        if (!zmk_keymap_layer_active(params.aml_layer)) {
            if (zmk_keymap_layer_activate(params.aml_layer, false) == 0) {
                owned = true;
            }
        }
        key = k_spin_lock(&data->lock);
        data->aml_owned_layer = owned;
        if (owned) {
            data->aml_owned_layer_id = params.aml_layer;
        }
        k_spin_unlock(&data->lock, key);
        if (owned) {
            k_work_reschedule(&data->aml_work, K_MSEC(params.aml_timeout_ms));
        }
        return;
    }
    if (owned && !zmk_keymap_layer_locked(params.aml_layer)) {
        zmk_keymap_layer_deactivate(params.aml_layer, false);
    }
    key = k_spin_lock(&data->lock);
    data->aml_owned_layer = false;
    data->aml_debounce_until_ms = k_uptime_get() + params.aml_debounce_ms;
    k_spin_unlock(&data->lock, key);
}

static int aml_keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *event = as_zmk_keycode_state_changed(eh);
    if (event == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (!event->state || event->keycode == 0 || is_mod(event->usage_page, event->keycode) ||
        event->usage_page == HID_USAGE_BUTTON) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    int64_t now = k_uptime_get();
    for (size_t i = 0; i < ARRAY_SIZE(pointer_devices); i++) {
        if (pointer_devices[i] == NULL) {
            continue;
        }
        struct dynamic_pointer_data *data = pointer_devices[i]->data;
        struct dynamic_input_processor_params params;
        if (pointer_read_params(pointer_devices[i], &params) != 0 || !params.aml_enabled) {
            continue;
        }
        k_spinlock_key_t key = k_spin_lock(&data->lock);
        data->aml_last_nonmouse_ms = now;
        data->aml_accumulated = 0;
        k_spin_unlock(&data->lock, key);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dynamic_pointer_aml, aml_keycode_listener);
ZMK_SUBSCRIPTION(dynamic_pointer_aml, zmk_keycode_state_changed);

int dynamic_pointer_set_temp_mode(const struct device *dev, enum dynamic_pointer_mode mode,
                                  uint32_t encoder_index, bool pressed) {
    if (dev == NULL) {
        return -EINVAL;
    }
    struct dynamic_pointer_data *data = dev->data;
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    uint8_t *count = NULL;
    switch (mode) {
    case DYNAMIC_POINTER_MODE_SCROLL:
        count = &data->temp_scroll_count;
        break;
    case DYNAMIC_POINTER_MODE_ENCODER:
        if (encoder_index >= DYNAMIC_POINTER_ENCODER_COUNT) {
            k_spin_unlock(&data->lock, key);
            return -EINVAL;
        }
        count = &data->temp_encoder_counts[encoder_index];
        break;
    default:
        k_spin_unlock(&data->lock, key);
        return -EINVAL;
    }
    if (pressed) {
        if (*count < UINT8_MAX) {
            (*count)++;
        }
    } else if (*count > 0) {
        (*count)--;
    }
    data->snap_count = 0;
    data->snap_history = 0;
    data->snap_state = 0;
    k_spin_unlock(&data->lock, key);
    return 0;
}

static int pointer_read_params(const struct device *dev,
                               struct dynamic_input_processor_params *params) {
    struct dynamic_pointer_data *data = dev->data;
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    *params = data->params;
    k_spin_unlock(&data->lock, key);
    return 0;
}

int dynamic_pointer_setting_read(const struct dynamic_setting_descriptor *descriptor,
                                 struct dynamic_setting_value *value, const void *context) {
    if (!descriptor || !value || !context) {
        return -EINVAL;
    }
    if (descriptor->type != DYNAMIC_SETTING_TYPE_U16) {
        return -EINVAL;
    }
    const struct device *dev = context;
    struct dynamic_pointer_data *data = dev->data;
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    uint16_t count = data->input_count_peak;
    data->input_count_peak = 0;
    k_spin_unlock(&data->lock, key);
    value->type = DYNAMIC_SETTING_TYPE_U16;
    value->length = sizeof(count);
    sys_put_le16(count, value->data);
    return 0;
}

int dynamic_input_processor_settings_after_change(const struct device *dev) {
    if (dev == NULL) {
        return -EINVAL;
    }

    struct dynamic_pointer_data *data = dev->data;
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    struct dynamic_input_processor_params params = data->params;
    int err = dynamic_input_processor_params_validate(&params);
    if (err != 0) {
        k_spin_unlock(&data->lock, key);
        return err;
    }

    data->cursor_remainder_x = 0;
    data->cursor_remainder_y = 0;
    data->rotation_remainder_x = 0;
    data->rotation_remainder_y = 0;
    data->scroll_remainder_x = 0;
    data->scroll_remainder_y = 0;
    data->encoder_remainder_x = 0;
    data->encoder_remainder_y = 0;
    data->snap_count = 0;
    data->snap_history = 0;
    data->snap_state = 0;
    rotation_q15(params.rotation_deg, &data->rotation_cos_q15, &data->rotation_sin_q15);

    bool release_aml_layer = data->aml_owned_layer &&
                             (!params.aml_enabled || params.aml_layer != data->aml_owned_layer_id);
    bool cancel_aml_work = !params.aml_enabled || params.aml_layer != data->aml_owned_layer_id;
    uint8_t release_layer = data->aml_owned_layer_id;
    if (cancel_aml_work) {
        data->aml_activation_pending = false;
    }
    if (release_aml_layer) {
        data->aml_owned_layer = false;
    }
    k_spin_unlock(&data->lock, key);

    if (cancel_aml_work) {
        k_work_cancel_delayable(&data->aml_work);
    }
    if (release_aml_layer && zmk_keymap_layer_active(release_layer) &&
        !zmk_keymap_layer_locked(release_layer)) {
        zmk_keymap_layer_deactivate(release_layer, false);
    }
    return 0;
}

static uint8_t highest_layer_id(void) {
    return zmk_keymap_layer_index_to_id(zmk_keymap_highest_layer_active());
}

static bool layer_matches(uint32_t mask, uint8_t layer) {
    return mask != 0 && layer < 32 && (mask & BIT(layer)) != 0;
}

static uint64_t integer_sqrt(uint64_t value) {
    uint64_t result = 0;
    uint64_t bit = 1ULL << 62;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

/* CORDIC in Q30 radians, returning Q15 cosine/sine without libm. */
static void rotation_q15(int16_t degrees, int32_t *cosine, int32_t *sine) {
    static const int32_t atan_q30[] = {
        843314857, 497837829, 263043837, 133525159, 67021687, 33543516, 16775851, 8388437,
        4194283,   2097149,   1048576,   524288,    262144,   131072,   65536,    32768,
        16384,     8192,      4096,      2048,      1024,     512,      256,      128,
        64,        32,        16,        8,         4,        2,        1};
    int32_t angle_deg = degrees % 360;
    if (angle_deg > 180) {
        angle_deg -= 360;
    } else if (angle_deg < -180) {
        angle_deg += 360;
    }
    int32_t sign = 1;
    if (angle_deg > 90) {
        angle_deg -= 180;
        sign = -1;
    } else if (angle_deg < -90) {
        angle_deg += 180;
        sign = -1;
    }
    int64_t z = (int64_t)angle_deg * 18740330; /* pi / 180 in Q30 */
    int64_t x = 652032874;                     /* CORDIC gain compensation, Q30 */
    int64_t y = 0;
    for (size_t i = 0; i < ARRAY_SIZE(atan_q30); i++) {
        int64_t old_x = x;
        if (z >= 0) {
            x -= y >> i;
            y += old_x >> i;
            z -= atan_q30[i];
        } else {
            x += y >> i;
            y -= old_x >> i;
            z += atan_q30[i];
        }
    }
    *cosine = (int32_t)((x * sign) >> 15);
    *sine = (int32_t)((y * sign) >> 15);
}

static uint32_t cursor_gain_q16(const struct dynamic_input_processor_params *params, uint8_t layer,
                                uint64_t norm) {
    uint32_t fine = layer_matches(params->fine_layers, layer) ? params->fine_coef : 1;
    uint32_t rough = layer_matches(params->rough_layers, layer) ? params->rough_coef : 1;
    uint64_t curve_q16 = dynamic_input_processor_shift_curve_q16(params, norm);
    uint64_t gain = (uint64_t)rough * curve_q16 / ((uint64_t)fine * 100);
    return (uint32_t)CLAMP(gain, 1, UINT32_MAX);
}

static int16_t saturate_i16(int64_t value) { return (int16_t)CLAMP(value, INT16_MIN, INT16_MAX); }

static void snap_update(struct dynamic_pointer_data *data, int32_t *x, int32_t *y, int64_t now_ms) {
    if (now_ms - data->last_motion_ms > 500) {
        data->snap_count = 0;
        data->snap_history = 0;
        data->snap_state = 0;
    }
    data->last_motion_ms = now_ms;
    uint8_t is_x = data->snap_state == 1
                       ? (llabs((long long)*y) > llabs((long long)*x) * 4 ? 0 : 1)
                       : (llabs((long long)*x) > llabs((long long)*y) * 4 ? 1 : 0);
    data->snap_history = (uint8_t)(((data->snap_history << 1) | is_x) & 0x1f);
    if (data->snap_count < 5) {
        data->snap_count++;
    }
    if (data->snap_count < 5) {
        *x = 0;
        *y = 0;
        return;
    }
    uint8_t x_count = 0;
    for (uint8_t bits = data->snap_history; bits != 0; bits >>= 1) {
        x_count += bits & 1;
    }
    if (data->snap_state == 0) {
        data->snap_state = x_count > 2 ? 1 : 2;
    } else if (data->snap_state == 1 && x_count <= 2 && (data->snap_history & 0x03) == 0) {
        data->snap_state = 2;
    } else if (data->snap_state == 2 && x_count > 2 && (data->snap_history & 0x03) == 3) {
        data->snap_state = 1;
    }
    if (data->snap_state == 1) {
        *y = 0;
    } else {
        *x = 0;
    }
}

static void emit_mouse_report(int16_t x, int16_t y, bool scroll) {
    if (scroll) {
        zmk_hid_mouse_movement_set(0, 0);
        zmk_hid_mouse_scroll_set(x, y);
    } else {
        zmk_hid_mouse_scroll_set(0, 0);
        zmk_hid_mouse_movement_set(x, y);
    }
    zmk_endpoint_send_mouse_report();
    zmk_hid_mouse_scroll_set(0, 0);
    zmk_hid_mouse_movement_set(0, 0);
}

static void emit_encoder_step(uint8_t sensor_index, int8_t direction) {
    struct sensor_value value = {.val1 = 0, .val2 = direction};
    raise_zmk_sensor_event((struct zmk_sensor_event){
        .sensor_index = sensor_index,
        .channel_data_size = 1,
        .channel_data = {{.value = value, .channel = SENSOR_CHAN_ROTATION}},
        .timestamp = k_uptime_get(),
    });
}

static void pointer_process_frame(const struct device *dev,
                                  const struct dynamic_pointer_config *cfg, int32_t input_x,
                                  int32_t input_y) {
    struct dynamic_pointer_data *data = dev->data;
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    struct dynamic_input_processor_params params = data->params;
    uint64_t input_count = integer_sqrt((uint64_t)((int64_t)input_x * input_x) +
                                        (uint64_t)((int64_t)input_y * input_y));
    uint16_t input_count_u16 = (uint16_t)MIN(input_count, UINT16_MAX);
    if (input_count_u16 > data->input_count_peak) {
        data->input_count_peak = input_count_u16;
    }
    if (params.aml_enabled) {
        int64_t movement = llabs((long long)input_x) + llabs((long long)input_y);
        data->aml_accumulated = CLAMP((int64_t)data->aml_accumulated + movement, 0, INT32_MAX);
        int64_t now = k_uptime_get();
        if (data->aml_owned_layer) {
            k_work_reschedule(&data->aml_work, K_MSEC(params.aml_timeout_ms));
        } else if (data->aml_accumulated >= params.aml_threshold &&
                   now >= data->aml_last_nonmouse_ms + params.aml_delay_ms &&
                   now >= data->aml_debounce_until_ms &&
                   !zmk_keymap_layer_active(params.aml_layer)) {
            data->aml_activation_pending = true;
            data->aml_accumulated = 0;
            k_work_reschedule(&data->aml_work, K_NO_WAIT);
        }
    }
    uint8_t layer = highest_layer_id();
    uint32_t gain = cursor_gain_q16(&params, layer, input_count);
    int64_t x_q16 = (int64_t)input_x * gain + data->cursor_remainder_x;
    int64_t y_q16 = (int64_t)input_y * gain + data->cursor_remainder_y;
    int64_t scaled_x = x_q16 / 65536;
    int64_t scaled_y = y_q16 / 65536;
    data->cursor_remainder_x = (int32_t)(x_q16 - scaled_x * 65536);
    data->cursor_remainder_y = (int32_t)(y_q16 - scaled_y * 65536);
    int64_t rotated_x_q15 = scaled_x * data->rotation_cos_q15 - scaled_y * data->rotation_sin_q15 +
                            data->rotation_remainder_x;
    int64_t rotated_y_q15 = scaled_x * data->rotation_sin_q15 + scaled_y * data->rotation_cos_q15 +
                            data->rotation_remainder_y;
    int64_t rotated_x = rotated_x_q15 / 32768;
    int64_t rotated_y = rotated_y_q15 / 32768;
    data->rotation_remainder_x = (int32_t)(rotated_x_q15 - rotated_x * 32768);
    data->rotation_remainder_y = (int32_t)(rotated_y_q15 - rotated_y * 32768);
    int32_t x = CLAMP(rotated_x, INT32_MIN, INT32_MAX);
    int32_t y = CLAMP(rotated_y, INT32_MIN, INT32_MAX);
    enum dynamic_pointer_mode mode = DYNAMIC_POINTER_MODE_CURSOR;
    uint8_t encoder_index = 0;
    bool temp_encoder_active = false;
    for (int i = DYNAMIC_POINTER_ENCODER_COUNT - 1; i >= 0; i--) {
        if (data->temp_encoder_counts[i] > 0) {
            encoder_index = i;
            temp_encoder_active = true;
            break;
        }
    }
    if (temp_encoder_active || layer_matches(params.encoder_layers, layer)) {
        mode = DYNAMIC_POINTER_MODE_ENCODER;
    } else if (data->temp_scroll_count > 0 || layer_matches(params.scroll_layers, layer)) {
        mode = DYNAMIC_POINTER_MODE_SCROLL;
    }
    bool scroll = mode == DYNAMIC_POINTER_MODE_SCROLL;
    bool encoder = mode == DYNAMIC_POINTER_MODE_ENCODER;
    bool emit_x_step = false;
    bool emit_y_step = false;
    int8_t x_direction = 0;
    int8_t y_direction = 0;
    uint8_t sensor_offset = encoder_index * 2;
    if (data->last_mode != mode || data->last_layer != layer ||
        (encoder && data->last_encoder_index != encoder_index)) {
        data->snap_count = 0;
        data->snap_history = 0;
        data->snap_state = 0;
        data->encoder_remainder_x = 0;
        data->encoder_remainder_y = 0;
        data->scroll_remainder_x = 0;
        data->scroll_remainder_y = 0;
        data->last_mode = mode;
        data->last_layer = layer;
        data->last_encoder_index = encoder_index;
    }
    if (scroll) {
        int16_t scroll_x;
        int16_t scroll_y;
        (void)dynamic_input_processor_divide_q16(x, params.scroll_divisor,
                                                 &data->scroll_remainder_x, &scroll_x);
        (void)dynamic_input_processor_divide_q16(y, params.scroll_divisor,
                                                 &data->scroll_remainder_y, &scroll_y);
        x = scroll_x;
        y = scroll_y;
        if (params.scroll_snap) {
            snap_update(data, &x, &y, k_uptime_get());
        } else {
            data->snap_count = 0;
            data->snap_history = 0;
            data->snap_state = 0;
        }
        if (params.scroll_invert) {
            x = CLAMP(-(int64_t)x, INT32_MIN, INT32_MAX);
            y = CLAMP(-(int64_t)y, INT32_MIN, INT32_MAX);
        }
        y = CLAMP(-(int64_t)y, INT32_MIN, INT32_MAX);
    } else {
        if (encoder && layer_matches(params.encoder_snap_layers, layer)) {
            snap_update(data, &x, &y, k_uptime_get());
        } else {
            data->snap_count = 0;
            data->snap_history = 0;
            data->snap_state = 0;
        }
    }
    if (encoder) {
        int64_t x_delta = (int64_t)data->encoder_remainder_x + x;
        int64_t y_delta = (int64_t)data->encoder_remainder_y - y;
        int64_t divisor = params.encoder_divisor;
        if (x_delta >= divisor) {
            emit_x_step = true;
            x_direction = 1;
            x_delta -= divisor;
        } else if (x_delta <= -divisor) {
            emit_x_step = true;
            x_direction = -1;
            x_delta += divisor;
        }
        if (y_delta >= divisor) {
            emit_y_step = true;
            y_direction = 1;
            y_delta -= divisor;
        } else if (y_delta <= -divisor) {
            emit_y_step = true;
            y_direction = -1;
            y_delta += divisor;
        }
        data->encoder_remainder_x = CLAMP(x_delta, INT32_MIN, INT32_MAX);
        data->encoder_remainder_y = CLAMP(y_delta, INT32_MIN, INT32_MAX);
        x = 0;
        y = 0;
    }
    k_spin_unlock(&data->lock, key);
    if (encoder) {
        if (emit_x_step) {
            emit_encoder_step(cfg->encoder_sensor_indices[sensor_offset], x_direction);
        }
        if (emit_y_step) {
            emit_encoder_step(cfg->encoder_sensor_indices[sensor_offset + 1], y_direction);
        }
    } else {
        emit_mouse_report(saturate_i16(x), saturate_i16(y), scroll);
    }
}

static void pointer_frame_work_handler(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct dynamic_pointer_data *data =
        CONTAINER_OF(delayable, struct dynamic_pointer_data, frame_work);
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    bool pending = data->pending_motion;
    int32_t input_x = data->pending_x;
    int32_t input_y = data->pending_y;
    data->pending_motion = false;
    data->pending_x = 0;
    data->pending_y = 0;
    k_spin_unlock(&data->lock, key);
    if (pending) {
        pointer_process_frame(data->dev, data->dev->config, input_x, input_y);
    }
}

static int dynamic_input_processor_handle_event(const struct device *dev, struct input_event *event,
                                                uint32_t param1, uint32_t param2,
                                                struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);
    const struct dynamic_pointer_config *cfg = dev->config;
    struct dynamic_pointer_data *data = dev->data;
    bool is_motion = event->type == cfg->type && event->type == INPUT_EV_REL &&
                     (event->code == cfg->x_code || event->code == cfg->y_code);

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    if (is_motion) {
        if (event->code == cfg->x_code) {
            data->pending_x = CLAMP((int64_t)data->pending_x + event->value, INT32_MIN, INT32_MAX);
        } else {
            data->pending_y = CLAMP((int64_t)data->pending_y + event->value, INT32_MIN, INT32_MAX);
        }
        data->pending_motion = true;
    }
    bool flush = event->sync && data->pending_motion;
    int32_t input_x = data->pending_x;
    int32_t input_y = data->pending_y;
    if (flush) {
        data->pending_motion = false;
        data->pending_x = 0;
        data->pending_y = 0;
    }
    k_spin_unlock(&data->lock, key);

    if (event->sync) {
        k_work_cancel_delayable(&data->frame_work);
    } else if (is_motion) {
        /* Coalesce drivers that omit sync markers, preserving paired XY data. */
        k_work_reschedule(&data->frame_work, K_MSEC(1));
    }
    if (flush) {
        pointer_process_frame(dev, cfg, input_x, input_y);
    }
    return is_motion ? ZMK_INPUT_PROC_STOP : ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api dynamic_input_processor_api = {
    .handle_event = dynamic_input_processor_handle_event,
};

static int pointer_init(const struct device *dev) {
    struct dynamic_pointer_data *data = dev->data;
    data->dev = dev;
    data->last_layer = UINT8_MAX;
    const struct dynamic_pointer_config *config = dev->config;
    int err = dynamic_input_processor_params_validate(&config->defaults);
    if (err != 0) {
        return err;
    }
    data->params = config->defaults;
    rotation_q15(config->defaults.rotation_deg, &data->rotation_cos_q15, &data->rotation_sin_q15);
    k_work_init_delayable(&data->aml_work, aml_work_handler);
    k_work_init_delayable(&data->frame_work, pointer_frame_work_handler);
    return 0;
}

#define DYNAMIC_POINTER_SENSOR_INDEX_VALID(node, prop, idx)                                        \
    (DT_PROP_BY_IDX(node, prop, idx) < ZMK_KEYMAP_SENSORS_LEN)

#define DYNAMIC_POINTER_INST(n)                                                                    \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, encoder_sensor_indices) == DYNAMIC_POINTER_ENCODER_COUNT * 2, \
                 "encoder-sensor-indices must map X/Y for each configured target");                \
    BUILD_ASSERT(                                                                                  \
        COND_CODE_0(DT_INST_PROP_LEN(n, encoder_sensor_indices), (1),                              \
                    (DT_INST_FOREACH_PROP_ELEM_SEP(n, encoder_sensor_indices,                      \
                                                   DYNAMIC_POINTER_SENSOR_INDEX_VALID, (&&)))),    \
        "encoder sensor index is not present in zmk,keymap-sensors");                              \
    static const struct dynamic_pointer_config config_##n = {                                      \
        .type = DT_INST_PROP(n, type),                                                             \
        .x_code = DT_INST_PROP(n, x_code),                                                         \
        .y_code = DT_INST_PROP(n, y_code),                                                         \
        .encoder_sensor_indices = {DT_INST_FOREACH_PROP_ELEM_SEP(n, encoder_sensor_indices,        \
                                                                 DT_PROP_BY_IDX, (, ))},           \
        .defaults =                                                                                \
            {                                                                                      \
                .rotation_deg = DT_INST_PROP(n, rotation_deg),                                     \
                .fine_layers = DT_INST_PROP(n, fine_layers),                                       \
                .fine_coef = DT_INST_PROP(n, fine_coef),                                           \
                .rough_layers = DT_INST_PROP(n, rough_layers),                                     \
                .rough_coef = DT_INST_PROP(n, rough_coef),                                         \
                .shift_points = {DT_INST_PROP(n, shift_point_0), DT_INST_PROP(n, shift_point_1),   \
                                 DT_INST_PROP(n, shift_point_2)},                                  \
                .shift_rates = {DT_INST_PROP(n, shift_rate_0), DT_INST_PROP(n, shift_rate_1),      \
                                DT_INST_PROP(n, shift_rate_2)},                                    \
                .scroll_layers = DT_INST_PROP(n, scroll_layers),                                   \
                .scroll_invert = DT_INST_PROP_OR(n, scroll_invert, 0),                             \
                .scroll_snap = DT_INST_PROP_OR(n, scroll_snap, 0),                                 \
                .scroll_divisor = DT_INST_PROP(n, scroll_divisor),                                 \
                .encoder_layers = DT_INST_PROP(n, encoder_layers),                                 \
                .encoder_snap_layers = DT_INST_PROP(n, encoder_snap_layers),                       \
                .encoder_divisor = DT_INST_PROP(n, encoder_divisor),                               \
                .aml_enabled = DT_INST_PROP_OR(n, aml_enabled, 0),                                 \
                .aml_layer = DT_INST_PROP_OR(n, aml_layer, 1),                                     \
                .aml_timeout_ms = DT_INST_PROP_OR(n, aml_timeout_ms, 400),                         \
                .aml_debounce_ms = DT_INST_PROP_OR(n, aml_debounce_ms, 25),                        \
                .aml_threshold = DT_INST_PROP_OR(n, aml_threshold, 10),                            \
                .aml_delay_ms = DT_INST_PROP_OR(n, aml_delay_ms, 200),                             \
            },                                                                                     \
    };                                                                                             \
    static struct dynamic_pointer_data data_##n;                                                   \
    DEVICE_DT_INST_DEFINE(n, pointer_init, NULL, &data_##n, &config_##n, POST_KERNEL,              \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &dynamic_input_processor_api);

DT_INST_FOREACH_STATUS_OKAY(DYNAMIC_POINTER_INST)
