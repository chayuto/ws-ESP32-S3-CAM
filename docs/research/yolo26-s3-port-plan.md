# Reference plan — porting YOLO26n to ESP32-S3

> **Status: archived reference, not an active plan.** Milestones 1–2 (ESPDet-Pico + MJPEG overlay + multi-class retrain) remain the committed path. This document captures the technical route for a YOLO26-on-S3 port so the information isn't lost if we pick it up later. Keeping it here avoids re-deriving the workflow from scratch.

## Why archived

- YOLO26n on S3 lands at ~1.5–2.5 s/frame after tuning (= 0.4–0.7 FPS). Past the "live demo" threshold, into "refreshing slideshow".
- The architectural appeal (NMS-free, DFL-free) only becomes a user-visible win if we also keep the NMS-free head, which requires a custom `Yolo26Postprocessor` that doesn't exist in ESP-DL yet.
- The ESPDet-Pico path already works at 7.9 FPS with a known postprocessor and is the right foundation for the streaming pipeline.

The plan is kept for completeness; revive if a compelling reason appears (e.g., an ESP-DL v3.3 that ships YOLO26 ops natively, or a publication angle that requires the YOLO26 name specifically).

## Anchors

Two reference points the plan draws from:
- Espressif's [YOLO11n-on-ESP-DL tutorial](https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_deploy_yolo11n.html) — template for the workflow.
- [`BoumedineBillal/yolo26n_esp`](https://github.com/BoumedineBillal/yolo26n_esp) — the only existing YOLO26 port, targets ESP32-P4 (NPU). S3 lacks the NPU; their P4 intrinsics don't port cleanly.

## Workflow

### 1. Model prep (host, PyTorch)
1. `pip install ultralytics`; download `yolo26n.pt`.
2. **Shrink input to 224×224** (from stock 640×640). ~8× FLOPs reduction. Retrain briefly at 224 to recover accuracy.
3. Export to ONNX with a modified head that moves bbox decode out of the network into post-processing — same trick as ESP-DL's `export_onnx.py`. Reduces quantization error from softmax-on-grid.
4. **Head choice**:
   - *Simpler path*: keep **only the one-to-many head**. Reuse ESP-DL's stock `dl::detect::Yolo11Postprocessor` unchanged. You lose YOLO26's NMS-free selling point but keep the DFL-free backbone wins.
   - *Full YOLO26 path*: keep the one-to-one head. Requires a custom `Yolo26Postprocessor` (see step 4 below).

### 2. Quantization (host, ESP-PPQ)
```python
from esp_ppq.api import QuantizationSettingFactory, get_target_platform
setting = QuantizationSettingFactory.espdl_setting()
# Mixed INT16 on the detection head preserves 1–2 pp mAP at ~5 % latency cost:
setting.dispatching_table.append("/model.X/.../Conv", get_target_platform("esp32s3", 16))
setting.weight_split = True
setting.weight_split_setting.method = "balance"
```
- Target `esp32s3`, **not** `esp32p4` (different op kernels).
- PTQ baseline: expect 3–4 pp mAP drop (40.9 → ~37).
- Recover with QAT (`yolo26n_qat.py`, adapted from Espressif's `yolo11n_qat.py`). Target ~38–39 mAP.
- Output: `yolo26n_224_s8_s3.espdl`, ~1.8 MB.
- Calibration: 300–500 COCO-val images resized to 224×224.

### 3. ESP-IDF project scaffold
```yaml
# main/idf_component.yml
dependencies:
  espressif/esp-dl: "^3.2.0"
  waveshare/esp32_s3_cam_ovxxxx: "^1.0.0"
  espressif/esp_new_jpeg: "^0.6.0"
```
Partition table: either enlarge `factory` to ~4 MB, or add a 3 MB SPIFFS `model` partition for the `.espdl`. Requires `CONFIG_PARTITION_TABLE_CUSTOM=y` + `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`.

Model loading: `dl::Model("/spiffs/yolo26n.espdl")` (SPIFFS) or CMake `EMBED_FILES` (in-flash).

### 4. Inference pipeline (device C++)
```cpp
dl::image::ImagePreprocessor preprocessor(
    model_input, {MEAN}, {STD}, dl::image::DL_IMAGE_CAP_RGB565_BIG_ENDIAN);
preprocessor.preprocess(fb->buf, fb->width, fb->height);   // letterbox + INT8 quantize
model.run();
auto results = yolo26_postprocess(model.get_outputs(), 0.25f /*conf*/, 0.45f /*iou*/);
```
- One-to-many-only head (simple path): reuse ESP-DL's `dl::detect::Yolo11Postprocessor` verbatim.
- NMS-free head path: custom `Yolo26Postprocessor`, ~150 lines — dequantize `(1, 300, 6)` output, confidence filter, sort, cap to top-K. Port from the Billal `Yolo26Processor`; drop P4 intrinsics; let the compiler auto-vectorize for S3 PIE.

### 5. S3-specific tuning

| Knob | Default | Tuned | Expected win |
|---|---|---|---|
| Input resolution | 640×640 | 224×224 | ~8× FLOPs, ~4× latency |
| Activation placement | PSRAM | Tile hot layers into ~100 KB internal SRAM via `dl::MemoryManagerGreedy` | 15–30 % |
| Core affinity | unpinned | inference on core 1, Wi-Fi on core 0 | smoother FPS |
| Detection-head precision | INT8 | INT16 via dispatching table | +1–2 pp mAP, ~5 % latency |
| Compiler optimization | `-Og` | `-O2` | 10–20 % |

Realistic landing: **~1.5–2.5 s/frame at 224×224**, mAP ~37–39.

### 6. If that's not fast enough (optional)

1. **Prune** YOLO26n to ~1.0 M params; fine-tune. Target ~0.8 s, ~35 mAP.
2. **Distill** YOLO26 into an ESPDet-Pico-shaped backbone. Keeps YOLO26 training-time gains (ProgLoss, STAL, MuSGD) while deploying a graph known to hit 126 ms on S3. Arguably the most honest "YOLO26 on S3" — YOLO26-trained weights, S3-shaped graph.
3. **Drop the P5 detection head**. ~15 % latency win, small mAP cost on small-object-heavy scenes.

### 7. Validation

- Host: run the quantized `.espdl` through ESP-PPQ's simulator; check mAP vs original ONNX.
- Device: log per-stage timings with `esp_timer_get_time()` around preprocess / inference / postprocess.
- Cross-check: one device-captured frame → JPEG → host Ultralytics CPU inference → compare bbox coordinates within ±2 px tolerance.

## Risk register

| Risk | Mitigation |
|---|---|
| ESP-PPQ v3.2.0 lacks YOLO26-op coverage (one-to-one head's `TopK`/`ArgMax` variants) | Contribute the op or substitute host-side; or take the one-to-many-only route (§1 step 4) |
| ESP-DL missing a custom op (ESP-PPQ flags quantization error) | Crib from Billal's repo — several already solved, though P4-specific |
| Flash/PSRAM footprint overrun | 1.8 MB model fits comfortably; worst case split via SPIFFS |
| Latency misses 2 s | Pivot to §6 option 2 (distill into ESPDet-Pico backbone) |

## Effort estimate

- Stage 1–2 (training + quantization): 1–2 days host compute.
- Stage 3–4 (device pipeline): 1 day — not novel if Path A's pipeline already exists.
- Stage 5 (tuning): 1–2 days.
- Stage 6 (if needed): +2–3 days for prune/distill.
- **Total: 4–7 days** once Path A's camera → overlay → MJPEG pipeline is solid.

## Sources

- [ESP-DL: How to deploy YOLO11n](https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_deploy_yolo11n.html)
- [ESP-DL: How to quantize model](https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_quantize_model.html)
- [BoumedineBillal/yolo26n_esp](https://github.com/BoumedineBillal/yolo26n_esp)
- [Ultralytics YOLO26 model docs](https://docs.ultralytics.com/models/yolo26/)
- [YOLO26 paper (arXiv 2509.25164)](https://arxiv.org/abs/2509.25164)
