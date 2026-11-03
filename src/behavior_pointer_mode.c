/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/dynamic_input_processor.h>

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_pointer_scroll) ||                                      \
    DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_pointer_encoder)
struct pointer_mode_config {
    const struct device *processor;
    enum dynamic_pointer_mode mode;
};

static int pointer_mode_pressed(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    const struct device *behavior = zmk_behavior_get_binding(binding->behavior_dev);
    if (behavior == NULL) {
        return -ENODEV;
    }
    const struct pointer_mode_config *config = behavior->config;
    return dynamic_pointer_set_temp_mode(config->processor, config->mode, binding->param1, true);
}

static int pointer_mode_released(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    const struct device *behavior = zmk_behavior_get_binding(binding->behavior_dev);
    if (behavior == NULL) {
        return -ENODEV;
    }
    const struct pointer_mode_config *config = behavior->config;
    return dynamic_pointer_set_temp_mode(config->processor, config->mode, binding->param1, false);
}

static const struct behavior_driver_api pointer_mode_api = {
    .binding_pressed = pointer_mode_pressed,
    .binding_released = pointer_mode_released,
};

#define POINTER_SCROLL_INST(n)                                                                     \
    static const struct pointer_mode_config config_scroll_##n = {                                  \
        .processor = DEVICE_DT_GET(DT_INST_PHANDLE(n, processor)),                                 \
        .mode = DYNAMIC_POINTER_MODE_SCROLL,                                                       \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, &config_scroll_##n, POST_KERNEL,                  \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &pointer_mode_api);

#define POINTER_ENCODER_INST(n)                                                                    \
    static const struct pointer_mode_config config_encoder_##n = {                                 \
        .processor = DEVICE_DT_GET(DT_INST_PHANDLE(n, processor)),                                 \
        .mode = DYNAMIC_POINTER_MODE_ENCODER,                                                      \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, &config_encoder_##n, POST_KERNEL,                 \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &pointer_mode_api);

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_pointer_scroll
DT_INST_FOREACH_STATUS_OKAY(POINTER_SCROLL_INST)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_pointer_encoder
DT_INST_FOREACH_STATUS_OKAY(POINTER_ENCODER_INST)
#endif
