#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

esp_err_t audio_capture_init(uint32_t sample_rate_hz, int mic_gain_db);

/* Mel pipeline reads via this direct stream (legacy single-consumer path). */
size_t audio_capture_read(int16_t *dst, size_t want_samples, TickType_t timeout);
StreamBufferHandle_t audio_capture_stream(void);

/* Multi-consumer tap for streaming, recording, etc. Each call returns a
 * unique handle; capture task fans out PCM to every registered tap until
 * it is removed. */
typedef void *audio_tap_handle_t;

audio_tap_handle_t audio_capture_add_tap(size_t ring_bytes);
void audio_capture_remove_tap(audio_tap_handle_t h);
size_t audio_capture_tap_read(audio_tap_handle_t h, int16_t *dst, size_t max_samples, TickType_t timeout);
size_t audio_capture_tap_available(audio_tap_handle_t h);
