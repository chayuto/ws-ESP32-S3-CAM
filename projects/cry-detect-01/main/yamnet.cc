/* YAMNet inference via TFLite Micro.
 *
 * Compiled as C++ because the TFLite Micro API is C++. The .h companion
 * exposes a plain-C interface so the rest of the project can stay in C.
 */

#include "yamnet.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "timing.h"

static const char *TAG = "yamnet";

namespace {

constexpr int kOpsCount = 16;

uint8_t *g_model_buf     = nullptr;
size_t   g_model_len     = 0;
uint8_t *g_tensor_arena  = nullptr;
size_t   g_arena_size    = 0;

const tflite::Model *g_model             = nullptr;
tflite::MicroInterpreter *g_interpreter  = nullptr;
TfLiteTensor *g_input                    = nullptr;
TfLiteTensor *g_output                   = nullptr;

bool g_has_classifier   = false;
float g_input_scale     = 1.0f;
int g_input_zero_point  = 0;
float g_output_scale    = 1.0f;
int g_output_zero_point = 0;

using OpResolver = tflite::MicroMutableOpResolver<kOpsCount>;
OpResolver *g_resolver = nullptr;

esp_err_t load_model_from_spiffs(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8 * 1024 * 1024) {
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

} /* anonymous namespace */

extern "C" esp_err_t yamnet_init(const char *model_path, size_t tensor_arena_kb)
{
    tflite::InitializeTarget();

    esp_err_t err = load_model_from_spiffs(model_path);
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
    g_has_classifier = (out_size == 521);
    ESP_LOGI(TAG, "output size=%d, has_classifier=%d", out_size, (int)g_has_classifier);
    if (!g_has_classifier) {
        ESP_LOGE(TAG, "this model is embedding-only; fetch the 1024-class variant");
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

extern "C" bool yamnet_has_classifier(void)
{
    return g_has_classifier;
}

extern "C" float yamnet_input_scale(void)
{
    return g_input_scale;
}

extern "C" int yamnet_input_zero_point(void)
{
    return g_input_zero_point;
}

extern "C" esp_err_t yamnet_run(const int8_t *patch_96x64, yamnet_result_t *result)
{
    memcpy(g_input->data.int8, patch_96x64, 96 * 64);

    CRY_TIMER_START(t0);
    TfLiteStatus s = g_interpreter->Invoke();
    int32_t latency = CRY_TIMER_ELAPSED_MS(t0);
    if (s != kTfLiteOk) {
        ESP_LOGW(TAG, "invoke failed");
        return ESP_FAIL;
    }

    int8_t raw = g_output->data.int8[YAMNET_BABY_CRY_CLASS_INDEX];
    float logit = g_output_scale * (float)(raw - g_output_zero_point);
    float conf = 1.0f / (1.0f + expf(-logit));

    result->cry_raw_int8 = raw;
    result->cry_conf = conf;
    result->latency_ms = latency;
    return ESP_OK;
}

extern "C" int yamnet_num_classes(void)
{
    if (!g_output) return 0;
    int n = 1;
    for (int i = 0; i < g_output->dims->size; ++i) n *= g_output->dims->data[i];
    return n;
}

extern "C" void yamnet_get_confidences(float *out, int max_classes)
{
    if (!g_output || !out) return;
    int n = yamnet_num_classes();
    if (max_classes < n) n = max_classes;
    for (int i = 0; i < n; ++i) {
        int8_t raw = g_output->data.int8[i];
        float logit = g_output_scale * (float)(raw - g_output_zero_point);
        out[i] = 1.0f / (1.0f + expf(-logit));
    }
}
