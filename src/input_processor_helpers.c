/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/dynamic_input_processor.h>

int dynamic_input_processor_params_validate(const struct dynamic_input_processor_params *params) {
    if (params == NULL) {
        return -EINVAL;
    }
    if (params->fine_coef == 0 || params->rough_coef == 0 || params->scroll_divisor == 0 ||
        params->encoder_divisor == 0) {
        return -ERANGE;
    }
    if (params->rotation_deg < -180 || params->rotation_deg > 180 || params->aml_layer > 31 ||
        params->aml_timeout_ms == 0 || params->aml_threshold == 0) {
        return -ERANGE;
    }
    if (params->shift_rates[0] == 0 || params->shift_rates[1] == 0 || params->shift_rates[2] == 0) {
        return -ERANGE;
    }
    if (params->shift_points[0] != 0 && params->shift_points[1] != 0 &&
        params->shift_points[1] <= params->shift_points[0]) {
        return -EINVAL;
    }
    uint8_t point3 = params->shift_points[2] == 0 ? UINT8_MAX : params->shift_points[2];
    uint8_t previous =
        params->shift_points[1] == 0 ? params->shift_points[0] : params->shift_points[1];
    if (params->shift_points[0] != 0 && previous != 0 && point3 <= previous) {
        return -EINVAL;
    }
    return 0;
}

uint32_t
dynamic_input_processor_shift_curve_q16(const struct dynamic_input_processor_params *params,
                                        uint64_t norm) {
    uint64_t curve_q16 = (uint64_t)params->shift_rates[2] * 65536;
    uint8_t point1 = params->shift_points[0];
    uint8_t point2 = params->shift_points[1];
    uint8_t point3 = params->shift_points[2] == 0 ? UINT8_MAX : params->shift_points[2];

    if (norm == 0 || point1 == 0) {
        return (uint32_t)curve_q16;
    }
    if (norm < point1) {
        return (uint32_t)((uint64_t)params->shift_rates[0] * 65536);
    }

    if (point2 == 0) {
        if (norm >= point3 || point3 <= point1) {
            return (uint32_t)curve_q16;
        }
        int32_t delta = (int32_t)params->shift_rates[2] - params->shift_rates[0];
        uint64_t offset = norm - point1;
        uint32_t span = point3 - point1;
        int64_t correction = (int64_t)delta * (int64_t)offset * 65536 / span;
        int64_t value = (int64_t)params->shift_rates[0] * 65536 + correction;
        return (uint32_t)(value > 0 ? value : 1);
    }

    if (norm < point2) {
        int32_t delta = (int32_t)params->shift_rates[1] - params->shift_rates[0];
        uint32_t span = point2 - point1;
        uint64_t offset = norm - point1;
        int64_t correction = (int64_t)delta * (int64_t)offset * 65536 / span;
        int64_t value = (int64_t)params->shift_rates[0] * 65536 + correction;
        return (uint32_t)(value > 0 ? value : 1);
    }

    if (norm >= point3 || point3 <= point2) {
        return (uint32_t)curve_q16;
    }

    int32_t delta = (int32_t)params->shift_rates[2] - params->shift_rates[1];
    uint64_t offset = norm - point2;
    uint32_t span = point3 - point2;
    int64_t correction = (int64_t)delta * (int64_t)offset * 65536 / span;
    int64_t value = (int64_t)params->shift_rates[1] * 65536 + correction;
    return (uint32_t)(value > 0 ? value : 1);
}

int dynamic_input_processor_scale_q16(int32_t input, uint32_t factor_q16, int32_t *remainder,
                                      int16_t *output) {
    if (output == NULL || factor_q16 == 0) {
        return -EINVAL;
    }
    int64_t accumulated = (int64_t)input * factor_q16;
    if (remainder != NULL) {
        accumulated += *remainder;
    }
    int64_t scaled = accumulated / 65536;
    if (remainder != NULL) {
        *remainder = (int32_t)(accumulated - scaled * 65536);
    }
    if (scaled > INT16_MAX) {
        *output = INT16_MAX;
    } else if (scaled < INT16_MIN) {
        *output = INT16_MIN;
    } else {
        *output = (int16_t)scaled;
    }
    return 0;
}

int dynamic_input_processor_divide_q16(int32_t input, uint8_t divisor, int32_t *remainder,
                                       int16_t *output) {
    if (output == NULL || divisor == 0) {
        return -EINVAL;
    }
    int64_t denominator = (int64_t)divisor * 65536;
    int64_t accumulated = (int64_t)input * 65536;
    if (remainder != NULL) {
        accumulated += *remainder;
    }
    int64_t scaled = accumulated / denominator;
    if (remainder != NULL) {
        *remainder = (int32_t)(accumulated - scaled * denominator);
    }
    if (scaled > INT16_MAX) {
        *output = INT16_MAX;
    } else if (scaled < INT16_MIN) {
        *output = INT16_MIN;
    } else {
        *output = (int16_t)scaled;
    }
    return 0;
}
