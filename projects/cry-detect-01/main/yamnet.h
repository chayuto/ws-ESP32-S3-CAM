#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YAMNET_BABY_CRY_CLASS_INDEX 20

typedef struct {
    float cry_conf;          /* dequantised, 0..1 sigmoid approx */
    int8_t cry_raw_int8;
    int32_t latency_ms;
} yamnet_result_t;

esp_err_t yamnet_init(const char *model_path, size_t tensor_arena_kb);
bool yamnet_has_classifier(void);
esp_err_t yamnet_run(const int8_t *patch_96x64, yamnet_result_t *result);

float yamnet_input_scale(void);
int yamnet_input_zero_point(void);

#ifdef __cplusplus
}
#endif
