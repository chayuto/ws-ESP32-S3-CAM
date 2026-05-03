#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Distilled student model wrapper — parallels yamnet.h.
 *
 * Used for Phase A side-by-side validation: when CONFIG_CRY_STUDENT_ENABLED,
 * the inference task runs the student on the same mel patches the teacher
 * sees and surfaces parallel cry_conf / speech_conf / latency / version
 * across /metrics, JSONL logs, and the SSE inference event. The detector's
 * trigger path is untouched — the teacher still drives detector_submit().
 *
 * All four functions are safe to call unconditionally; when the feature
 * is compile-time disabled they're stubs (zero-fill the result, return
 * ESP_OK or "off") so the caller does not need to #ifdef around them.
 */

typedef struct {
    int8_t  cry_raw_int8;     /* raw int8 logit at class 19 (Crying, sobbing) */
    int8_t  baby_raw_int8;    /* raw int8 logit at class 20 (Baby cry, infant cry) */
    float   cry_conf;         /* softmax(logits)[19] + softmax(logits)[20], in [0, 1] */
    float   speech_conf;      /* softmax(logits)[0] (Speech) — for caregiver-suppression telemetry */
    int32_t latency_ms;       /* TFLite Invoke wall time */
} student_result_t;

/* Loads the .tflite from SPIFFS, allocates a tensor arena in PSRAM,
 * verifies input/output shapes (96×64 INT8 in, 521 INT8 out).
 *
 * When CONFIG_CRY_STUDENT_ENABLED=n this is a no-op that returns ESP_OK
 * — call it unconditionally from app_main. */
esp_err_t student_init(const char *model_path, size_t tensor_arena_kb);

/* True iff the model has been loaded and AllocateTensors succeeded. */
bool student_is_loaded(void);

/* Runs one inference on a 96×64 INT8 mel patch. Result is populated
 * with zeros when the feature is disabled or the model isn't loaded;
 * caller does not need to check is_loaded() first. */
esp_err_t student_run(const int8_t *patch_96x64, student_result_t *result);

/* Build-time version tag (CONFIG_CRY_STUDENT_VERSION, e.g. "v0.2.0").
 * Returns "off" when the feature is compile-time disabled.
 * Always returns a valid NUL-terminated string. */
const char *student_version(void);

#ifdef __cplusplus
}
#endif
