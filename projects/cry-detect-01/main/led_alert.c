#include "led_alert.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/esp-bsp.h"
#include "esp_io_expander.h"

static const char *TAG = "led";

static esp_io_expander_handle_t s_io;
static int s_pin_num;
static led_state_t s_state = LED_STATE_BOOT;
static TaskHandle_t s_task;

static void set_level(int on)
{
    /* active-LOW: on = 0, off = 1 */
    esp_io_expander_set_level(s_io, 1u << s_pin_num, on ? 0 : 1);
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
                 * 50 ms tick → full cycle every ~2500 ms. */
                static uint32_t phase = 0;
                phase = (phase + 1) % 50;
                int duty = phase < 25 ? phase : (50 - phase);    /* 0..25 */
                int on_ticks = duty * 2;                          /* 0..50 ms of on */
                set_level(on_ticks > 25 ? 1 : 0);
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
        }

        if (solid >= 0) {
            set_level(solid);
            level = solid;
        } else {
            int64_t dt_ms = (now - last_toggle) / 1000;
            if (dt_ms >= period_ms / 2) {
                level ^= 1;
                set_level(level);
                last_toggle = now;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
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
    set_level(1);   /* boot = on */

    BaseType_t ok = xTaskCreatePinnedToCore(
        led_task, "led", 2048, NULL, 2, &s_task, 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void led_alert_set(led_state_t s)
{
    s_state = s;
}
