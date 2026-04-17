# Pre-built `.espdl` model inventory — what you can skip training for

Motivation: decide whether training is avoidable. Triggered by "training isn't fun" — checking if a community multi-class `.espdl` exists anywhere public so we can skip the training loop entirely.

**Short answer: no usable multi-class `.espdl` exists in public.** You can skip training only if you accept single-class detection, or accept non-streaming (~6 s/frame) snapshot mode.

## Full inventory (verified 2026-04-17)

| Source | Model(s) | Classes | Input | S3 latency | Real-time? |
|---|---|---|---|---|---|
| [`espressif/esp-detection`](https://github.com/espressif/esp-detection) | `espdet_pico_224_224_cat.espdl` | 1 (cat) | 224×224 | 126 ms | ✓ 7.9 FPS |
| `espressif/esp-detection` | `espdet_pico_160_288_cat.espdl` | 1 (cat) | 160×288 | 115 ms | ✓ 8.7 FPS |
| `espressif/esp-detection` | `espdet_pico_416_416_cat.espdl` | 1 (cat) | 416×416 | 450 ms | ⚠️ 2.2 FPS |
| `espressif/esp-detection` | dog, hand variants | 1 each | 224 or 160×288 | ~126 ms | ✓ |
| [`espressif/esp-dl/models/coco_detect/`](https://github.com/espressif/esp-dl) | `yolo11n_s8_v1_s3.espdl` | **80 (COCO)** | 640×640 | 26 162 ms | ❌ 0.04 FPS |
| `espressif/esp-dl/models/coco_detect/` | `yolo11n_320_s8_v3_s3.espdl` | **80 (COCO)** | 320×320 | 6 184 ms | ❌ 0.16 FPS |
| [`espressif/esp-who`](https://github.com/espressif/esp-who) | `HumanFaceDetect` | 1 (face) | 240×240 | ~70 ms | ✓ 10–15 FPS |
| `espressif/esp-who` | `PedestrianDetect` | 1 (person) | 240×240 | ~120 ms | ✓ ~8 FPS |
| `espressif/esp-who` | `CatFaceDetect` | 1 (cat face) | 240×240 | ~100 ms | ✓ ~10 FPS |
| [`cnadler86/mp_esp_dl_models`](https://github.com/cnadler86/mp_esp_dl_models) | MicroPython bindings for the above | no new models | — | — | — |
| HuggingFace (searched) | **No `.espdl` models found** | — | — | — | — |
| Broader GitHub | **No community multi-class ESPDet-Pico or YOLO26 `.espdl`** | — | — | — | — |

## Signal

- The only **multi-class pre-quantized** model published anywhere for S3 is Espressif's own **YOLO11n COCO** at 80 classes — and it takes **6–26 seconds per frame**. Useful for snapshot-style interactions, not for live streaming.
- Every **fast** model (≤ 200 ms) is **single-class**.
- Nobody has published a **multi-class ESPDet-Pico** or a smaller-than-YOLO11n multi-class detector. This is where the training-free shortcut would live if it existed. It doesn't.

## What this means for the first project

Three training-free options remain:

1. **Option X — Multi-model live composite.** Stack 2–3 single-class `esp-who` / ESPDet-Pico models per frame, merge detections, overlay all. Gets you face + person + cat in one live MJPEG at ~5 FPS. 0 training.
2. **Option Y — Single-class + voice.** `espdet_pico_cat` + ESP-SR wake word + ES8311 TTS. "What do you see?" → "A cat, 78 %." Uses board's unique audio hardware. 0 training.
3. **Option W — Snapshot mode, 80-class.** `yolo11n_320_s8_v3_s3.espdl` triggered by the user button (GPIO 15). Capture → ~6 s inference → single annotated JPEG returned over HTTP. Not a live stream, but multi-class without training. 0 training.

Training remains the only path to **multi-class live MJPEG**. Since that's the unfun part, pick from X / Y / W above for a genuinely training-free first project.

## Sources

- [espressif/esp-detection](https://github.com/espressif/esp-detection) — ESPDet-Pico models (cat/dog/hand only)
- [espressif/esp-dl](https://github.com/espressif/esp-dl) — YOLO11n COCO `.espdl` in `models/coco_detect/`
- [ESP-DL YOLO11n deploy tutorial](https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_deploy_yolo11n.html) — S3 latency numbers
- [espressif/esp-who](https://github.com/espressif/esp-who) — face / pedestrian / cat-face detectors
- [cnadler86/mp_esp_dl_models](https://github.com/cnadler86/mp_esp_dl_models) — MicroPython bindings
- HuggingFace — searched `espdl`, `ESP-DL`, `ESPDet`; no detection models found
