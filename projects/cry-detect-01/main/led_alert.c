#include "led_alert.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "bsp/esp-bsp.h"
#include "esp_io_expander.h"

static const char *TAG = "led";

static esp_io_expander_handle_t s_io;
static int s_pin_num;
static led_state_t s_state = LED_STATE_BOOT;
static TaskHandle_t s_task;

/* Brightness 0..100; applied as software-PWM duty over a 10-tick (500 ms)
 * cycle on top of whatever cadence the state machine commands. */
static int s_brightness = 100;
#define LED_PWM_TICKS_PER_CYCLE 10

static void nvs_load_brightness(void)
{
    nvs_handle_t h;
    if (nvs_open("led", NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 100;
        if (nvs_get_i32(h, "bright", &v) == ESP_OK) {
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            s_brightness = (int)v;
        }
        nvs_close(h);
    }
}

static void nvs_save_brightness(void)
{
    nvs_handle_t h;
    if (nvs_open("led", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "bright", s_brightness);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void set_level(int on)
{
    /* active-LOW: on = 0, off = 1 */
    esp_io_expander_set_level(s_io, 1u << s_pin_num, on ? 0 : 1);
}

/* Software-PWM dim wrapper: within a 10-tick (500 ms) cycle, on for
 * (brightness/10) ticks, off for the remainder. Called every 50 ms. */
static void set_level_dimmed(int logical_on)
{
    static int s_phase = 0;
    s_phase = (s_phase + 1) % LED_PWM_TICKS_PER_CYCLE;
    if (!logical_on || s_brightness <= 0) {
        set_level(0);
        return;
    }
    if (s_brightness >= 100) {
        set_level(1);
        return;
    }
    int on_ticks = (s_brightness * LED_PWM_TICKS_PER_CYCLE + 50) / 100;
    set_level(s_phase < on_ticks ? 1 : 0);
}

static void led_task(void *arg)
{
    int64_t last_toggle = 0;
    int level = 0;

    while (1) {
        int64_t now = esp_timer_get_time();
        int period_ms = 0;
        int solid = -1;

        switch (s_state) {
            case LED_STATE_BOOT:       solid = 1; break;
            case LED_STATE_IDLE:       solid = 0; break;
            case LED_STATE_ALERT:      solid = 1; break;
            case LED_STATE_CONNECTING: period_ms = 1000; break;
            case LED_STATE_SYNCING:    period_ms = 250;  break;
            case LED_STATE_ERROR:      period_ms = 2000; break;
            case LED_STATE_STREAMING: {
                /* Slow software-PWM "breathing" at ~0.4 Hz.
                 * Brightness applied as max duty ceiling. */
                static uint32_t phase = 0;
                phase = (phase + 1) % 50;
                int duty = phase < 25 ? phase : (50 - phase);    /* 0..25 */
                int on_ticks = duty * 2;                          /* 0..50 */
                int want_on = on_ticks > 25 ? 1 : 0;
                set_level_dimmed(want_on);
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
        }

        if (solid >= 0) {
            set_level_dimmed(solid);
            level = solid;
        } else {
            int64_t dt_ms = (now - last_toggle) / 1000;
            if (dt_ms >= period_ms / 2) {
                level ^= 1;
                last_toggle = now;
            }
            set_level_dimmed(level);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void led_alert_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    nvs_save_brightness();
    ESP_LOGI(TAG, "brightness set to %d%%", s_brightness);
}

int led_alert_get_brightness(void)
{
    return s_brightness;
}

esp_err_t led_alert_init(int expander_pin)
{
    esp_err_t err = bsp_io_expander_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "io expander init failed: 0x%x", err);
        return err;
    }
    s_io = bsp_get_io_expander_handle();
    if (!s_io) {
        ESP_LOGE(TAG, "no io expander handle");
        return ESP_FAIL;
    }
    s_pin_num = expander_pin;
    nvs_load_brightness();
    set_level_dimmed(1);   /* boot = on (at configured brightness) */
    ESP_LOGI(TAG, "led init: brightness=%d%%", s_brightness);

    BaseType_t ok = xTaskCreatePinnedToCore(
        led_task, "led", 2048, NULL, 2, &s_task, 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void led_alert_set(led_state_t s)
{
    s_state = s;
}
