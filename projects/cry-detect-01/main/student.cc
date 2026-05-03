/* student.cc — Distilled INT8 cry-detection student via TFLite Micro.
 *
 * Parallels yamnet.cc. When CONFIG_CRY_STUDENT_ENABLED the body below
 * implements the wrapper. Otherwise the four exported symbols compile
 * to no-op stubs so callers do not need to #ifdef around them.
 *
 * Phase A — runs alongside the YAMNet teacher, never replaces it.
 * See yamnet-cry-distill-int8 docs/research/student-integration-plan-20260503.md.
 */

#include "student.h"
#include "sdkconfig.h"

#include <string.h>

#if CONFIG_CRY_STUDENT_ENABLED

#include <stdio.h>
#include <math.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "timing.h"

static const char *TAG = "student";

#define NUM_CLASSES 521
#define CRY_IDX_1   19   /* Crying, sobbing */
#define CRY_IDX_2   20   /* Baby cry, infant cry */
#define SPEECH_IDX  0    /* Speech */

namespace {

constexpr int kOpsCount = 16;

uint8_t *g_model_buf     = nullptr;
size_t   g_model_len     = 0;
uint8_t *g_tensor_arena  = nullptr;
size_t   g_arena_size    = 0;

const tflite::Model *g_model            = nullptr;
tflite::MicroInterpreter *g_interpreter = nullptr;
TfLiteTensor *g_input                   = nullptr;
TfLiteTensor *g_output                  = nullptr;

bool  g_loaded            = false;
float g_input_scale       = 1.0f;
int   g_input_zero_point  = 0;
float g_output_scale      = 1.0f;
int   g_output_zero_point = 0;

using OpResolver = tflite::MicroMutableOpResolver<kOpsCount>;
OpResolver *g_resolver = nullptr;

esp_err_t load_model_from_path(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        ESP_LOGE(TAG, "implausible model size %ld", sz);
        return ESP_ERR_INVALID_SIZE;
    }
    g_model_buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!g_model_buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(g_model_buf, 1, sz, f);
    fclose(f);
    if ((long)got != sz) return ESP_FAIL;
    g_model_len = sz;
    ESP_LOGI(TAG, "loaded %ld bytes from %s", sz, path);
    return ESP_OK;
}

TfLiteStatus register_ops(OpResolver *r)
{
    /* Same op set as yamnet.cc. The student doesn't strictly need
     * Logistic / Mul / Pad but having them registered is free at
     * runtime and keeps the resolver schema parallel. */
    if (r->AddConv2D() != kTfLiteOk) return kTfLiteError;
    if (r->AddDepthwiseConv2D() != kTfLiteOk) return kTfLiteError;
    if (r->AddFullyConnected() != kTfLiteOk) return kTfLiteError;
    if (r->AddMaxPool2D() != kTfLiteOk) return kTfLiteError;
    if (r->AddAveragePool2D() != kTfLiteOk) return kTfLiteError;
    if (r->AddMean() != kTfLiteOk) return kTfLiteError;
    if (r->AddReshape() != kTfLiteOk) return kTfLiteError;
    if (r->AddSoftmax() != kTfLiteOk) return kTfLiteError;
    if (r->AddLogistic() != kTfLiteOk) return kTfLiteError;
    if (r->AddQuantize() != kTfLiteOk) return kTfLiteError;
    if (r->AddDequantize() != kTfLiteOk) return kTfLiteError;
    if (r->AddPad() != kTfLiteOk) return kTfLiteError;
    if (r->AddAdd() != kTfLiteOk) return kTfLiteError;
    if (r->AddMul() != kTfLiteOk) return kTfLiteError;
    if (r->AddRelu() != kTfLiteOk) return kTfLiteError;
    if (r->AddRelu6() != kTfLiteOk) return kTfLiteError;
    return kTfLiteOk;
}

}  /* anonymous namespace */

extern "C" esp_err_t student_init(const char *model_path, size_t tensor_arena_kb)
{
    /* TFLite micro target init is shared with yamnet.cc; calling twice
     * is a documented no-op. */
    tflite::InitializeTarget();

    esp_err_t err = load_model_from_path(model_path);
    if (err != ESP_OK) return err;

    g_model = tflite::GetModel(g_model_buf);
    if (g_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "schema mismatch: model=%u runtime=%d",
                 (unsigned)g_model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    g_arena_size = tensor_arena_kb * 1024;
    g_tensor_arena = (uint8_t *)heap_caps_malloc(g_arena_size, MALLOC_CAP_SPIRAM);
    if (!g_tensor_arena) {
        ESP_LOGE(TAG, "arena alloc %u KB failed", (unsigned)tensor_arena_kb);
        return ESP_ERR_NO_MEM;
    }

    g_resolver = new OpResolver();
    if (register_ops(g_resolver) != kTfLiteOk) {
        ESP_LOGE(TAG, "op resolver registration failed");
        return ESP_FAIL;
    }

    static tflite::MicroInterpreter *interpreter_storage = nullptr;
    interpreter_storage = new tflite::MicroInterpreter(
        g_model, *g_resolver, g_tensor_arena, g_arena_size);
    g_interpreter = interpreter_storage;

    TfLiteStatus allocate_status = g_interpreter->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "arena used: %u / %u bytes",
             (unsigned)g_interpreter->arena_used_bytes(), (unsigned)g_arena_size);

    g_input  = g_interpreter->input(0);
    g_output = g_interpreter->output(0);

    if (g_input->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "expected int8 input, got type %d", g_input->type);
        return ESP_FAIL;
    }
    g_input_scale       = g_input->params.scale;
    g_input_zero_point  = g_input->params.zero_point;
    g_output_scale      = g_output->params.scale;
    g_output_zero_point = g_output->params.zero_point;

    int out_size = 1;
    for (int i = 0; i < g_output->dims->size; ++i) {
        out_size *= g_output->dims->data[i];
    }
    if (out_size != NUM_CLASSES) {
        ESP_LOGE(TAG, "expected %d output classes, got %d", NUM_CLASSES, out_size);
        return ESP_FAIL;
    }

    g_loaded = true;
    ESP_LOGI(TAG, "ready  version=%s  in_scale=%.4f in_zp=%d  out_scale=%.4f out_zp=%d",
             CONFIG_CRY_STUDENT_VERSION,
             (double)g_input_scale, g_input_zero_point,
             (double)g_output_scale, g_output_zero_point);
    return ESP_OK;
}

extern "C" bool student_is_loaded(void)
{
    return g_loaded;
}

extern "C" esp_err_t student_run(const int8_t *patch_96x64, student_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    /* Initialise to safe defaults so a not-loaded call returns zeros
     * instead of garbage. */
    memset(result, 0, sizeof(*result));

    if (!g_loaded) return ESP_ERR_INVALID_STATE;

    memcpy(g_input->data.int8, patch_96x64, 96 * 64);

    CRY_TIMER_START(t0);
    TfLiteStatus s = g_interpreter->Invoke();
    int32_t latency = CRY_TIMER_ELAPSED_MS(t0);
    if (s != kTfLiteOk) {
        ESP_LOGW(TAG, "invoke failed");
        return ESP_FAIL;
    }

    /* The student outputs raw logits (no softmax in graph). Dequantise
     * all 521 outputs, then apply softmax with the standard log-sum-exp
     * trick for numerical stability. cry_conf is the post-softmax mass
     * on classes 19+20. */
    float logits[NUM_CLASSES];
    float max_logit = -1e30f;
    for (int i = 0; i < NUM_CLASSES; ++i) {
        int8_t raw = g_output->data.int8[i];
        logits[i] = g_output_scale * (float)(raw - g_output_zero_point);
        if (logits[i] > max_logit) max_logit = logits[i];
    }
    float sum_exp = 0.0f;
    for (int i = 0; i < NUM_CLASSES; ++i) {
        sum_exp += expf(logits[i] - max_logit);
    }
    float inv_sum = (sum_exp > 1e-12f) ? (1.0f / sum_exp) : 0.0f;

    float p_cry    = expf(logits[CRY_IDX_1] - max_logit) * inv_sum;
    float p_baby   = expf(logits[CRY_IDX_2] - max_logit) * inv_sum;
    float p_speech = expf(logits[SPEECH_IDX] - max_logit) * inv_sum;

    result->cry_raw_int8  = g_output->data.int8[CRY_IDX_1];
    result->baby_raw_int8 = g_output->data.int8[CRY_IDX_2];
    result->cry_conf      = p_cry + p_baby;
    result->speech_conf   = p_speech;
    result->latency_ms    = latency;
    return ESP_OK;
}

extern "C" const char *student_version(void)
{
    return CONFIG_CRY_STUDENT_VERSION;
}

#else  /* !CONFIG_CRY_STUDENT_ENABLED — stubs for unconditional callers */

extern "C" esp_err_t student_init(const char *model_path, size_t tensor_arena_kb)
{
    (void)model_path;
    (void)tensor_arena_kb;
    return ESP_OK;
}

extern "C" bool student_is_loaded(void)
{
    return false;
}

extern "C" esp_err_t student_run(const int8_t *patch_96x64, student_result_t *result)
{
    (void)patch_96x64;
    if (result) memset(result, 0, sizeof(*result));
    return ESP_OK;
}

extern "C" const char *student_version(void)
{
    return "off";
}

#endif  /* CONFIG_CRY_STUDENT_ENABLED */
