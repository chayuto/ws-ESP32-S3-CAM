#pragma once

#include "esp_err.h"

typedef enum {
    LED_STATE_BOOT,         /* solid on */
    LED_STATE_CONNECTING,   /* 1 Hz blink */
    LED_STATE_SYNCING,      /* 4 Hz blink */
    LED_STATE_IDLE,         /* off */
    LED_STATE_ALERT,        /* solid on */
    LED_STATE_ERROR,        /* 0.5 Hz blink */
    LED_STATE_STREAMING,    /* slow breathing — privacy indicator */
} led_state_t;

esp_err_t led_alert_init(int expander_pin);
void led_alert_set(led_state_t s);

/* 0 = off, 100 = full brightness. Applied as software-PWM duty to the
 * "on" phase of every state. Persisted in NVS so it survives reboot. */
void led_alert_set_brightness(int percent);
int led_alert_get_brightness(void);
