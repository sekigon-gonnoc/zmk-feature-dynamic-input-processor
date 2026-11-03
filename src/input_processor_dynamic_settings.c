/* SPDX-License-Identifier: MIT */

#include <stdint.h>

#include <zephyr/sys/util.h>

#include <zmk/dynamic_input_processor.h>
#include <zmk/dynamic_settings/store.h>

int dynamic_pointer_setting_changed(const struct dynamic_setting_descriptor *descriptor,
                                    const struct dynamic_setting_value *value,
                                    enum dynamic_setting_change_reason reason,
                                    const void *context) {
    ARG_UNUSED(descriptor);
    ARG_UNUSED(value);
    ARG_UNUSED(reason);
    return dynamic_input_processor_settings_after_change(context);
}
