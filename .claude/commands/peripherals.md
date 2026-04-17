# Peripheral cookbook — ESP32-S3-CAM-GC2145

Use the vendor BSP `waveshare/esp32_s3_cam_ovxxxx` for anything nontrivial. It hides the CH32V003 IO-expander quirks, I²C bus init, camera pin config, and codec addresses. Header: `ref/demo/ESP32-S3-CAM-OVxxxx/examples/ESP-IDF-v5.5.1/04_dvp_camera_display/components/waveshare__esp32_s3_cam_ovxxxx/include/bsp/esp32_s3_cam_ovxxxx.h`

## Dependency Line

```yaml
# main/idf_component.yml
dependencies:
  waveshare/esp32_s3_cam_ovxxxx: ^1.0.0
```

## Standard Init Sequence

```c
#include "bsp/esp-bsp.h"

bsp_i2c_init();                    // GPIO 7/8 shared bus for everything
bsp_io_expander_init();            // CH32V003 — needed before display/touch/LED
bsp_audio_init(NULL);              // default duplex mono 16-bit 22050 Hz

// Then any of:
esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();     // ES8311 DAC
esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();  // ES7210 ADC
esp_camera_init(&(camera_config_t)BSP_CAMERA_DEFAULT_CONFIG);    // GC2145
// bsp_display_start() only if an LCD is fitted
```

---

## Camera (GC2145)

Driver auto-detects the sensor. BSP provides a filled-in config macro:

```c
camera_config_t cfg = BSP_CAMERA_DEFAULT_CONFIG;
// cfg: pin_xclk=38, pin_pclk=41, pin_vsync=17, pin_href=18, D0-D7 = 45/47/48/46/42/40/39/21
// cfg: RGB565, QVGA (320×240), 2 framebuffers in PSRAM, XCLK 20 MHz, JPEG q=12
esp_camera_init(&cfg);
sensor_t *s = esp_camera_sensor_get();
s->set_framesize(s, FRAMESIZE_UXGA);    // 1600×1200 if you want max
s->set_pixformat(s, PIXFORMAT_JPEG);    // or RGB565/YUV422
```

Boot log expectation: 3 `E (…)` SCCB probe-miss lines for OV2640/OV5640/GC0308 before `I (…) gc2145: Detected Camera sensor PID=0x2145`. Not errors.

- SCCB shares the main I²C (no separate pins)
- PWDN and RESET are NC — sensor is always powered; no software power-cycle
- XCLK ≥ 16 MHz enables EDMA to PSRAM (faster throughput)

## Audio Out (ES8311 → speaker)

```c
esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
esp_codec_dev_sample_info_t fs = {
    .sample_rate = 16000, .channel = 1, .bits_per_sample = 16,
};
esp_codec_dev_open(spk, &fs);
esp_codec_dev_set_out_vol(spk, 70);     // 0-100
esp_codec_dev_write(spk, wav_samples, len);
esp_codec_dev_close(spk);
```

- ES8311 I²C address via `esp_codec_dev` convention = **0x30 (8-bit)**, not 0x18 (7-bit). The driver shifts right internally.
- No external amplifier pin — speaker enable is internal to ES8311 or wired direct.

## Audio In (ES7210 4-ch ADC → mic array)

```c
esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
esp_codec_dev_sample_info_t fs = {
    .sample_rate = 16000, .channel = 2, .bits_per_sample = 16,
};
esp_codec_dev_open(mic, &fs);
int16_t buf[320];  // 20 ms at 16 kHz stereo
esp_codec_dev_read(mic, buf, sizeof(buf));
```

- ES7210 is the **I²S slave**; it provides MIC1 + MIC2 on this board (two mic positions for beamforming)
- ES7210 default I²C address = **0x40 (7-bit)**
- Boot log reports: "STD mode, dir: RX, data_bit: 32, slot_bit: 32" — native is 32-bit; driver handles conversion to 16-bit

## Speech Recognition (ESP-SR)

```yaml
# main/idf_component.yml
dependencies:
  espressif/esp-sr: ^2.1.5
  waveshare/esp32_s3_cam_ovxxxx: ^1.0.0
```

Partition table must include a `model` spiffs partition ≥ 3 MB (≥ 5900 KB for wake+command). Model binary `srmodels.bin` is auto-generated and flashed at the `model` offset.

Key Kconfig (via `idf.py menuconfig` → "ESP Speech Recognition"):
- **Wake word**: `CONFIG_SR_WN_WN9_HIESP` (Hi ESP), `CONFIG_SR_WN_WN9_HILEXIN`, `CONFIG_SR_WN_WN9_ALEXA`, `CONFIG_SR_WN_WN9_JARVIS_TTS`, plus ~20 others
- **VAD**: `CONFIG_SR_VADN_WEBRTC` (default, cheap) or `CONFIG_SR_VADN_VADNET1_MEDIUM` (neural, S3+)
- **AFE template**: AEC(SR_LOW_COST) → SE(BSS) → VAD → WakeNet → (optional MultiNet)

Vendor example code pattern (`02_esp_sr/main/main.c`):
```c
Speech_Init();                                // BSP wraps AFE setup
Speech_register_callback(on_event);
// Callback fires with ESP_SR_EVT_AWAKEN when wake word heard,
// then ESP_SR_EVT_CMD with phrase id after a recognized command,
// or ESP_SR_EVT_CMD_TIMEOUT after a few seconds of silence.
```

## CH32V003 IO Expander

```c
bsp_io_expander_init();                           // Required before LCD/LED/touch
esp_io_expander_handle_t io = bsp_get_io_expander_handle();
// Pins on CH32V003:
//   P0 = touch reset, P1 = LCD backlight (PWM), P2 = LCD reset, P6 = red LED
esp_io_expander_set_level(io, IO_EXPANDER_PIN_NUM_6, 1);   // red LED on
uint16_t vbat_raw = bsp_get_io_expander_adc();              // battery ADC
```

The CH32V003 is a real RISC-V microcontroller but the firmware is fixed — treat it as a smart I²C peripheral. Its pins are **not ESP32-S3 GPIOs** and cannot be reassigned.

## Buttons

```c
#include "iot_button.h"
// GPIO 0 (BOOT) and GPIO 15 (user). Both active-LOW with external pull-up.
button_config_t cfg = {
    .type = BUTTON_TYPE_GPIO,
    .gpio_button_config = { .gpio_num = 15, .active_level = 0 },
};
button_handle_t btn = iot_button_create(&cfg);
iot_button_register_cb(btn, BUTTON_PRESS_UP, on_release, NULL);
```

## SD Card (SDMMC 1-bit)

```c
bsp_sdcard_cfg_t cfg = {0};
bsp_sdcard_sdmmc_mount(&cfg);           // mounts at BSP_SD_MOUNT_POINT
FILE *f = fopen(BSP_SD_MOUNT_POINT"/test.txt", "w");
```

CLK=16, CMD=43, D0=44 — only 1 data line wired, so capped at SDMMC 1-bit speeds.

## Gotchas

- **I²S is duplex** — calling `bsp_audio_init()` twice (once per path) returns the same channels; the second call is idempotent.
- **Camera + LCD + SD all want PCLK-like high-speed pins** — the BSP schedules them, don't try to reassign.
- **No onboard speaker amplifier pin**; if a project expects `BSP_POWER_AMP_IO`, it's `GPIO_NUM_NC` here (the ES8311 drives the speaker directly).
- **ES7210 slave mode means the ESP32-S3 is the I²S master** — required so the S3 supplies MCLK. Don't try to set the codec master.
- **ESP-SR model partition must be flashed** — the `model` SPIFFS partition (5.9 MB) holds `srmodels.bin`. Flashing only the app breaks wake word detection.
