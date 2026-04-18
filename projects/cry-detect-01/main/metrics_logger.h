#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Verbose 1 Hz inference logger.
 *
 * Writes one JSON object per line to /sdcard/infer-YYYYMMDD.jsonl (or
 * /logs/... if SD not mounted). Schema is self-describing so we can grow
 * fields without breaking old parsers. File rotates at local midnight.
 *
 * Payload includes: full timestamp, uptime, inference counters, top-10
 * classes over all 521 AudioSet classes, the 20 curated watched-class
 * confidences, audio RMS + noise floor, system heap / psram / rssi /
 * die-temp, and per-task stack high-water marks.
 *
 * This runs in parallel with sd_logger's CSV — CSV stays human-readable
 * for eyeballing, JSONL is the rich stream for analysis / retraining.
 */

esp_err_t metrics_logger_init(void);

/* Called from the inference task after every yamnet_run. Copies the 521-
 * class output under a mutex. Non-blocking on the caller in the common case.
 * cry_conf and latency_ms are already in the metrics snapshot but we pass
 * them through so the logger row aligns with the exact inference that
 * produced the top-k. */
void metrics_logger_publish_inference(const float *conf_521,
                                      float cry_conf,
                                      int32_t latency_ms);

#ifdef __cplusplus
}
#endif
