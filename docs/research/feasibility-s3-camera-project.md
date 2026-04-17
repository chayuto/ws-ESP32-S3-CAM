# Feasibility: YOLO26 + annotated MJPEG on the Waveshare ESP32-S3-CAM-GC2145

Reassessment after the research in `yolo26-and-esp-dl-2026.md`. Verified against hardware notes in project `CLAUDE.md` and `/hardware-specs`.

## The ask

Extend the vendor `01_simple_video_server` example so the MJPEG stream shows bounding boxes, class labels, and confidence — using **YOLO26**, the 2026-01 Ultralytics release.

## Short answer

**YOLO26n at the published 640×640 / 512×512 input is not real-time on ESP32-S3.** Best published S3 data point for a full YOLO-class model (YOLO11n, same parameter scale as YOLO26n) is **15–26 s per frame**. YOLO26's architecture (DFL-free, NMS-free) helps accuracy-under-quantization and simplifies postproc but does not make the backbone meaningfully cheaper. The only YOLO26-on-Espressif port that exists is **P4-only (1.77 s/frame)** — S3 lacks the P4's NPU.

**But the project is still viable** if we pick one of these three paths. Recommended order:

### Path A (recommended) — ESPDet-Pico, multi-class retrain, YOLO26-style head

Espressif's own `esp-detection` repo ships a 0.36 M-parameter YOLOv11-derivative that hits **~7.9 FPS at 224×224** on S3 with ~0.70 mAP (single-class demo). We retrain it multi-class on COCO or a custom class set, keep the ESP-DL postprocessor path, and draw the YOLO26-style dual-head improvements back in only if training stability is a problem. Lose: the "YOLO26" name. Gain: actually runs at usable framerate on the board we have.

### Path B — YOLO26n proper, accept ~1 FPS

Port Billal's `yolo26n_esp` Yolo26Processor to S3, quantize YOLO26n INT8 QAT, and accept **~0.1–0.5 FPS** (estimate: 10–20 s/frame at 512×512 on S3, given P4 is 5–10× faster and hits 1.77 s there). The stream would show annotations that refresh every few seconds — not a live demo, but a valid research artifact and a true YOLO26 port. First public YOLO26 on S3 if nobody else has shipped one.

### Path C — change hardware

Buy an **ESP32-P4** board (~$15–25) where YOLO26n @ 512×512 lands at ~1.8 s = **0.56 FPS with NPU**. Still not "live" but 10× closer. Defeats the purpose of this workspace; noting it for completeness.

## Why YOLO26 on S3 is slow — the arithmetic

- YOLO26n is 2.4 M params, 5.4 GFLOPs per forward pass at 640×640.
- S3 peak INT8 throughput via PIE vector instructions: roughly 2–3 GOP/s sustained for conv workloads (ballpark from ESP-DL numbers; no official FLOPs/s spec).
- Theoretical floor: 5.4 GFLOPs / 3 GOP/s = 1.8 s/frame best-case, *before* memory stalls from PSRAM bandwidth (8 MB OPI @ 80 MHz ≈ 640 MB/s).
- Measured: YOLO11n at 5 GFLOPs runs in 26 s on S3 — dominated by PSRAM tensor shuttling, not compute.
- At 320×320 (¼ the FLOPs), YOLO11n is still 6 s — confirming the bottleneck is the activation-tensor PSRAM traffic, not raw ops.
- Lowering input to 224×224 and cutting params 7× (ESPDet-Pico) gets us to 126 ms = 7.9 FPS. That's the regime we have to live in.

## Recommended project shape (Path A)

**Name:** `projects/mjpeg-detect/`

**Pipeline:**

```
GC2145 (QVGA RGB565, 2 FB in PSRAM)
   ↓ esp_camera_fb_get()
preprocess: letterbox → 224×224 → INT8 quantize (dl_image_preprocessor)
   ↓
ESP-DL inference (ESPDet-Pico, INT8, ~126 ms)
   ↓
postprocess: decode + NMS (dl_detect_postprocessor)
   ↓
draw bboxes + labels + confidence on original RGB565 frame (custom)
   ↓
soft JPEG encode (esp_new_jpeg, q≈12)
   ↓
MJPEG stream on port 81 (unchanged from 01_simple_video_server)
```

**Core budget** (QVGA stream + single-model inference):

| Resource | Estimate |
|---|---|
| Camera FB (320×240 RGB565 × 2) | ~300 KB PSRAM |
| Preproc scratch (224×224 INT8 × 3) | ~150 KB PSRAM |
| ESPDet-Pico activation tensors | ~400 KB PSRAM (guess; measure) |
| JPEG encode scratch | ~100 KB PSRAM |
| LWIP + Wi-Fi | ~85 KB SRAM |
| **Total PSRAM active** | **~1 MB of 7.5 MB available** — comfortable |

**Core assignment** (dual-core S3):
- Core 0: Wi-Fi / LWIP / HTTP server tasks (default pinning).
- Core 1: camera pull → preprocess → inference → annotate → JPEG encode pipeline.

**Annotation rendering** (RGB565, no antialiasing):
- Bbox: four 2-px horizontal/vertical line spans.
- Label chip: 8×8 bitmap font, solid bg, overlay at top-left of box.
- Confidence: render as 2-digit percent, same font.
- All CPU; negligible vs inference.

**Framerate target:** 5–7 FPS annotated stream at QVGA. Preview (unannotated) path from vendor example runs ~13 FPS — we explicitly trade half that for labels.

## What we need next (not doing now)

1. Decide: Path A (recommended) vs Path B (YOLO26 with ~1 FPS).
2. Clone `espressif/esp-detection` into `ref/` for the Path A reference, and `BoumedineBillal/yolo26n_esp` if we keep Path B in mind.
3. Pin ESP-DL v3.2.0 via component manager; confirm it loads on our IDF v5.5.3.
4. Choose a class set (COCO-80 is large; a 10-class custom set will train faster and quantize cleaner).
5. Generate the `.espdl` offline on a workstation (the quantization step uses PyTorch + ESP-PPQ, not an on-device task).
6. Decide partition strategy: embed `.espdl` in `main/` (~1–2 MB) vs SPIFFS partition (easier to re-flash just the model).

## Open questions worth a quick experiment before committing

- **PSRAM bandwidth headroom**: is there a measurable FPS gap between putting activations in internal SRAM (fits only if <~200 KB) vs PSRAM? The ESP-DL static memory planner handles this — worth checking the log output on first inference.
- **Camera format**: stock example grabs RGB565 QVGA. Does the ESP-DL preprocessor accept RGB565 directly, or do we need a software color-convert pass? (Likely yes — `dl_image_preprocessor.hpp` is documented to do color conversion + crop + resize + normalize + quantize in one shot.)
- **Drawing primitive library**: does ESP-DL ship an overlay utility, or do we write a 50-line RGB565 rect/line/glyph kernel? (Former if available, latter is easy.)

---

## Verdict

**Do Path A.** "YOLOv26 on the ESP32-S3-CAM-GC2145" in the strict sense (= ship a real YOLOv26n model and run it) is a months-slow stream — not a product. A "YOLO-family real-time detector with bbox/label/confidence drawn on the stream" using ESPDet-Pico is a weekend project that actually works. If the research ambition is specifically "first public YOLOv26-on-S3", queue that as Path B after Path A proves the pipeline.

## Updated 2026-04-17 — prior-art check (see `prior-art-survey.md`)

After surveying what's public, Path A is **more differentiated than first assumed**. Espressif's own `esp-detection` repo only ships a terminal-print demo on a single still image; nobody has published the camera → inference → RGB565 overlay → MJPEG stream pipeline on S3. FOMO-based tutorials are ubiquitous but produce centroids, not true bboxes. Offboard-Python YOLO demos are ubiquitous but run on a PC.

So Path A is not "reinventing a well-trodden tutorial" — it's filling the integration gap the Espressif README leaves open. The four stackable novelty angles (core pipeline / multi-class retrain / YOLO26 port / voice-query overlay) are all documented in `prior-art-survey.md`.

**Revised sequencing recommendation:**

1. **Milestone 1 — Path A core.** Wire ESPDet-Pico into `01_simple_video_server`, add RGB565 bbox/label/confidence overlay, stream MJPEG. Target ~5–7 FPS at QVGA. Validates the pipeline; publishable as "ESPDet-Pico live MJPEG overlay on Waveshare ESP32-S3-CAM-GC2145".
2. **Milestone 2 — multi-class retrain.** Swap the cat-only model for a multi-class ESPDet-Pico trained on COCO-subset (10–20 classes). Document the retraining recipe.
3. **Milestone 3 (optional) — YOLO26 path.** Port YOLO26n to S3 as a second selectable model; accept ~1–3 s/frame at 224×224. First public S3 port.
4. **Milestone 4 (optional) — voice-query overlay.** ESP-SR wake → read top-3 detections via ES8311 TTS. First public ESP-SR + ESP-DL combo demo.
