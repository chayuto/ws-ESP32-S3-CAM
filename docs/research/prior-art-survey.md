# Prior-art survey — what's already shipped on ESP32-S3

Motivation: before committing to Path A (ESPDet-Pico + bbox overlay + MJPEG), check whether this is already done N times on GitHub. Verdict at the bottom.

## Categories of existing work

### 1. ESP32-CAM + **offboard** YOLO (PC does the work) — very common

Dozens of tutorials, at least a dozen GitHub repos. Shape:

- ESP32-CAM streams MJPEG/JPEG over HTTP.
- A Python script on a PC pulls frames, runs YOLOv3/v5/v8 in OpenCV or Ultralytics, draws boxes, displays locally.

Examples: `Scicrop/esp32-cam-yolo`, `Curovearth/Esp32_yolo_gtts`, `azizmeca1/esp32cam-yolov3-object-detection`, tutorials from MakerGuides, Electronic Clinic, How2Electronics.

**This is not what we want.** No on-device inference, no on-device overlay.

### 2. XIAO ESP32-S3 Sense + **Edge Impulse FOMO** — very common in tutorials

Edge Impulse's FOMO architecture is the go-to "TinyML object detection" stack on S3. Hundreds of blog posts / Hackster / Seeed tutorials.

**Crucial limitation:** FOMO outputs **centroids, not bounding boxes**. Quote from the Hackster/MlSysBook docs: *"The object's size is not detected with FOMO, as with MobileNet SSD or YOLO, where the Bounding Box is one of the model outputs."*

So FOMO tutorials:
- Run on-device ✓
- Draw "box" (really a fixed-size marker at the centroid) on a TFT display ✓
- Do NOT produce true YOLO-style variable-size bboxes ✗
- Do NOT stream annotated frames via MJPEG ✗

### 3. ESP-DL / esp-detection — Espressif's own stack, *minimal demo*

`espressif/esp-detection` (ESPDet-Pico, YOLOv11-derivative, hits 7.9 FPS at 224×224 on S3) is the current Espressif answer to on-device YOLO. But the official example:

- Runs inference on **a single input image** (not camera).
- Prints results to the **serial terminal**.
- Does **not** draw on the frame. Does **not** stream. Does **not** wire the camera.

Quote from the repo: *"enables customized model inference on a single input image and prints the results to the terminal."*

The ESP-DL library does ship a `dl_image_preprocessor.hpp` and postprocessor helpers, but wiring `esp-detection` ↔ `esp_camera` ↔ `esp_new_jpeg` ↔ HTTP server into an end-to-end pipeline is **left as an exercise** for integrators.

### 4. ESP-DL display variants (TFT, not streamed)

A handful of hobby projects draw FOMO or ESP-WHO (person/face detection) bboxes directly on a TFT display (e.g., Seeed XIAO with TFT shield, ESP32-S3-Box, DFRobot "Edge Computing AI Camera"). These use LovyanGFX/LVGL, not the ESP-DL image pipeline, and target a physical screen, not a browser.

### 5. MJPEG streaming on S3 — very common, no ML

`s60sc/ESP32-CAM_MJPEG2SD`, `arkhipenko/esp32-cam-mjpeg`, every RandomNerdTutorials post on camera streaming. Rock-solid streaming, zero detection.

## What *does not* exist publicly (as of April 2026)

Searched: "ESP32-S3 YOLO bounding box MJPEG web stream", "ESP-DL MJPEG stream bounding box overlay HTTP", "Waveshare ESP32-S3-CAM-GC2145 YOLO", "esp-detection demo github video". No hit for the specific combo below:

1. **On-device** YOLO-class detector (with true bboxes, not FOMO centroids), **drawing boxes on the frame**, **streamed as MJPEG** to a browser, on an ESP32-S3.
2. Same, using **`esp-detection` / ESPDet-Pico** (too new — the repo is a 2026 release).
3. Same, on the **Waveshare ESP32-S3-CAM-GC2145** (niche sensor, small user base).
4. Any YOLO26 deployment on **ESP32-S3** (only P4 exists: `BoumedineBillal/yolo26n_esp`).

## Novelty angles that remain, ranked

| # | Angle | Novelty | Effort |
|---|---|---|---|
| 1 | **ESPDet-Pico wired into the vendor video-server with on-frame RGB565 bbox overlay + MJPEG** | High — no public example | Medium (2–3 days) |
| 2 | **Multi-class ESPDet-Pico retrain** (the shipped model is cat-only) with a documented recipe | Medium | Medium (training compute required) |
| 3 | **YOLO26n-on-S3 port**, even at 1–3 s/frame — first public | High — first S3 port | High (3–5 days; QAT, custom postproc) |
| 4 | **GC2145-specific quirks** (different AWB/AEC than OV2640, rolling shutter, lens) documented for ML pipelines | Low–medium | Low (as a side-product of #1) |
| 5 | **Voice-query overlay using the dual-mic array + ES8311 TTS** ("what do you see?" → ESP-SR wake → read top-3 detections aloud) | High — nobody combines ESP-SR + ESP-DL publicly | High (two subsystems, shared I²S) |

## Verdict

The user's instinct *"a lot of people done it on S3"* is half-right:
- **FOMO-on-S3 with centroid markers** — yes, done to death.
- **ESP32-CAM streaming + PC-side YOLO** — yes, done to death.
- **Fully on-device true-bbox YOLO + annotated MJPEG on S3** — **not a public project as of 2026-04.** ESPDet-Pico is the right tool, but nobody has published the streamed overlay pipeline yet.

So Path A (ESPDet-Pico + MJPEG overlay) is **still differentiated work** — it's what the Espressif README *implies* but doesn't actually ship. And stacking Path A with any of angles #2 / #3 / #5 above turns it into a clearly novel artifact.

## Path A novelty — sharpened (2026-04-17)

Confirmed after a targeted search. `espressif/esp-detection` ships **three single-class demo models** (cat, dog, hand); the framework *supports* multi-class via `espdet_run.py` but **no public multi-class `.espdl` checkpoint exists**, and the official runtime example is still "single image → terminal print". The four concrete novelty bullets for Path A as scoped:

1. **The integration Espressif's README implies but doesn't ship** — gluing `esp-detection` to `esp_camera` → RGB565 overlay → `esp_new_jpeg` → HTTP MJPEG. Biggest single novelty; fills the "left as an exercise" gap.
2. **First publicly released multi-class ESPDet-Pico checkpoint + training recipe** — COCO subset (10–20 classes), released `.espdl` + `espdet_run.py` invocation.
3. **True variable-size bboxes + labels + confidence drawn in the RGB565 camera framebuffer**, then software-JPEG-encoded. FOMO-on-TFT draws centroids; offboard YOLO draws boxes on a PC; Path A does bboxes on-device in the streamed frame.
4. **GC2145 sensor specifics** (niche sensor, different AEC/AWB than the OV2640 used in ~99 % of ESP32-CAM tutorials) documented as a side-product.

### Why it's not already done

ESPDet-Pico is a 2026 release. Most "YOLO on ESP32" tutorials predate it and use OpenCV-on-PC or FOMO. The tooling that makes Path A feasible only landed this year, so the public backlog hasn't caught up. Early-mover window.

---

## Sources

- [espressif/esp-detection](https://github.com/espressif/esp-detection) — official ESPDet-Pico repo (terminal-only demo)
- [XIAO ESP32S3 Object Detection (MlSysBook)](https://mlsysbook.ai/contents/labs/seeed/xiao_esp32s3/object_detection/object_detection.html) — FOMO-centroid limitation quote
- [TinyML Object Detection with XIAO ESP32S3 Sense (Hackster)](https://www.hackster.io/mjrobot/tinyml-made-easy-object-detection-with-xiao-esp32s3-sense-6be28d) — representative FOMO tutorial
- [Object Detection with ESP32-CAM and YOLO (MakerGuides)](https://www.makerguides.com/object-detection-with-esp32-cam-and-yolo/) — representative offboard-YOLO tutorial
- [ESP32-CAM_MJPEG2SD](https://github.com/s60sc/ESP32-CAM_MJPEG2SD) — streaming-only stack, widely deployed
- [BoumedineBillal/yolo26n_esp](https://github.com/BoumedineBillal/yolo26n_esp) — first YOLO26-on-ESP, P4 only
- [Seeed Xiao ESP32-S3 Sense + TFT Display (Edge Impulse forum)](https://forum.edgeimpulse.com/t/seeed-xiao-esp32-s3-sense-camera-object-detection-with-tft-display/10209) — TFT-overlay not MJPEG
