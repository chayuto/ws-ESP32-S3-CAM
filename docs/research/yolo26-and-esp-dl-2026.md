# YOLO26 + ESP-DL — state of the art as of April 2026

Research for the first project on this workspace: can we run YOLOv26 object detection on the Waveshare ESP32-S3-CAM-GC2145 and draw bboxes + labels + confidence on the streamed video? This file documents the facts; `feasibility-s3-camera-project.md` draws the conclusion.

> All numbers below are **verbatim from cited sources**. Citations at the bottom. Verified 2026-04-17.

---

## 1. What YOLO26 actually is

- Released by **Ultralytics on 2026-01-14** after a preview at YOLO Vision 2025 (London, 2025-09-25).
- Paper: *YOLO26: Key Architectural Enhancements and Performance Benchmarking for Real-Time Object Detection* — arXiv 2509.25164, last revised 2026-03-16.
- Unified model family: detection, segmentation, classification, pose, OBB.

### Architectural changes vs YOLO11/YOLO12

| Change | Effect |
|---|---|
| **Distribution Focal Loss (DFL) removed** | Simplified regression head; quantization-friendly (no softmax over discrete bins) |
| **NMS-free end-to-end head** (one-to-one) | No post-processing NMS kernel needed; fixed output shape `(N, 300, 6)` |
| **Dual-head** (one-to-one + one-to-many coexist) | Can still use NMS path during training or if deployer prefers |
| **ProgLoss** (Progressive Loss Balancing) | Training stability |
| **STAL** (Small-Target-Aware Label Assignment) | Better on small objects |
| **MuSGD optimizer** (Muon + SGD) | Faster convergence |

The two changes that matter for embedded deployment are **DFL removal** and **NMS-free inference**: both shrink the post-processing footprint and make INT8 quantization cleaner than YOLO11/12.

### Official variant table (COCO, 640×640)

| Model | Params (M) | FLOPs (B) | mAP50-95 | CPU ONNX (ms) | TensorRT10 GPU (ms) |
|---|---|---|---|---|---|
| **YOLO26n** | **2.4** | **5.4** | **40.9** | **38.9 ± 0.7** | 1.7 ± 0.0 |
| YOLO26s | 9.5 | 20.7 | 48.6 | 87.2 ± 0.9 | 2.5 ± 0.0 |
| YOLO26m | 20.4 | 68.2 | 53.1 | 220.0 ± 1.4 | 4.7 ± 0.1 |
| YOLO26l | 24.8 | 86.4 | 55.0 | 286.2 ± 2.0 | 6.2 ± 0.2 |
| YOLO26x | 55.7 | 193.9 | 57.5 | 525.8 ± 4.0 | 11.8 ± 0.2 |

- **YOLO26n is 43 % faster on CPU than YOLO11n** (Ultralytics claim).
- **INT8 quantization: <1 % mAP drop** in most cases (Ultralytics / OpenVINO tutorial).

### Export formats

ONNX · TensorRT · TFLite · CoreML · OpenVINO · NCNN · RKNN. No first-party ESP-DL export — but ESP-PPQ consumes ONNX.

---

## 2. ESP-DL status (Espressif's DL runtime)

- **Latest release: v3.2.0, 2025-10-23**.
- Quantization tool: **ESP-PPQ** (fork of ppq). Consumes ONNX/PyTorch/TF → emits `.espdl` (FlatBuffers container, zero-copy, supports INT8 + INT16).
- **ESP-IDF requirement: v5.3+** (we run v5.5.3 → fine).
- **YOLO26 model added 2026-03-12** — contributed by BoumedineBillal (the `yolo26n_esp` repo).

### Existing COCO detection benchmarks *on ESP32-S3* (ESP-DL YOLO11n tutorial)

This is the canonical "YOLO on S3" reference point.

| Model | Input | Flash | PSRAM | Preprocess | **Inference** | Postprocess | mAP50-95 |
|---|---|---|---|---|---|---|---|
| yolo11n_s8_v1_s3 | 640×640 | 8 MB | 8 MB | 51.5 ms | **26 162 ms** | 58.6 ms | 0.307 |
| yolo11n_s8_v2_s3 | 640×640 | 16 MB | 16 MB | 51.7 ms | **15 981 ms** | 59.2 ms | 0.331 |
| yolo11n_s8_v3_s3 | 640×640 | 8 MB | 8 MB | 51.7 ms | **26 057 ms** | 58.0 ms | 0.359 |
| yolo11n_320_s8_v3_s3 | 320×320 | 8 MB | 8 MB | 15.1 ms | **6 184 ms** | 17.6 ms | 0.277 |

**Read the inference column**: 6–26 *seconds* per frame. YOLO11n on a bare S3 is **not real-time**. This matters because YOLO26n is the same parameter class (2.4 M vs ~2.6 M for YOLO11n) and the same ~5 B-FLOP ballpark — architecture changes do not rescue S3's compute deficit.

### First-party YOLO26 on ESP: `BoumedineBillal/yolo26n_esp`

- **Target: ESP32-P4 only.** No S3 numbers published.
- YOLO26n INT8 QAT @ 512×512 → **1.77 s / frame** on P4 (= 0.56 FPS).
- YOLO11n baseline on P4 @ 640×640 → 2.75 s / frame.
- mAP50-95: 36.5 % @ 512×512, 38.5 % @ 640×640.
- **Why P4 and not S3**: ESP32-P4 has a dedicated NPU (matmul accelerator), S3 only has the PIE vector unit. Empirically P4 is ≈ 5–10× faster than S3 on the same DL workload.
- Extrapolating: YOLO26n @ 512 on S3 ≈ **10–18 s / frame** — worse than useless for a video feed.

### The path Espressif ships for "YOLO-on-S3 that actually runs": `espressif/esp-detection`

- Project name: **ESP-Detection** — based on Ultralytics YOLOv11, not YOLO26.
- Flagship model: **ESPDet-Pico** — 0.36 M parameters (≈ 7× smaller than YOLO26n).
- ESP32-S3 benchmarks:

| Variant | Input | **ESP32-S3 latency** | **FPS** | mAP50-95 (cat-only) |
|---|---|---|---|---|
| espdet_pico_224_224_cat | 224×224 | 126.2 ms | **~7.9** | 0.699 |
| espdet_pico_160_288_cat | 160×288 | 115.5 ms | **~8.7** | 0.712 |
| espdet_pico_416_416_cat | 416×416 | 449.5 ms | ~2.2 | 0.766 |

- Trained on single class (cat) because that's the demo; the architecture scales to multi-class.
- Deployment: `idf.py set-target esp32s3 && idf.py flash monitor`. Uses the same ESP-DL runtime.

### Other S3-viable options

- **YOLOX-Nano** — Elecrow HMI example reports 4–6 FPS on S3 with Vector SIMD (input size not documented; likely ≤ 320×320).
- **Espressif human-face-detect / pedestrian-detect / cat-face-detect** — pre-built, 10+ FPS on S3.

---

## 3. Quantization path (ESP-PPQ) — summary

1. Train / download Ultralytics weights (`.pt`).
2. Run ESP-DL's modified `export_onnx.py` — moves bbox decoding from inference to post-processing to reduce quant error.
3. Quantize with ESP-PPQ:
   ```python
   quant_setting = QuantizationSettingFactory.espdl_setting()
   # For tricky layers, mixed-precision:
   quant_setting.dispatching_table.append("/model.2/cv2/conv/Conv", get_target_platform("esp32s3", 16))
   quant_setting.weight_split = True
   ```
   - 8-bit PTQ only: 30.7 % mAP on YOLO11n (3.7 pp drop).
   - 8-bit PTQ + mixed-precision + weight split: 33.4 %.
   - 8-bit QAT (`yolo11n_qat.py`): 36.0 % (best).
4. Emits `.espdl` → ship in `main/` or embed in a SPIFFS/FAT partition.
5. C++ on device: `dl_detect_base.hpp` + `dl_detect_yolo11_postprocessor.hpp` + `dl_image_preprocessor.hpp`.

Input format: RGB, 0–1 normalized, per-tensor INT8 scale baked into `.espdl`.

For YOLO26's NMS-free head the postprocessor is simpler — the one-to-one head already outputs the final `(N, 300, 6)` — but ESP-DL's stock postprocessor is YOLO11-shaped; a YOLO26 postprocessor either reuses the dual-head's one-to-many path (NMS needed) or a custom `Yolo26Processor` (as in the Billal repo).

---

## 4. Sources

**YOLO26 primary**
- [YOLO26 paper (arXiv 2509.25164)](https://arxiv.org/abs/2509.25164) — architecture + benchmarks, last revised 2026-03-16
- [Ultralytics YOLO26 model docs](https://docs.ultralytics.com/models/yolo26/) — variant table, mAP, CPU/GPU latency
- [Ultralytics blog: Meet YOLO26](https://www.ultralytics.com/blog/meet-ultralytics-yolo26-a-better-faster-smaller-yolo-model) — release notes, DFL removal, NMS-free
- [Roboflow YOLO26 overview](https://blog.roboflow.com/yolo26/) — variant specs, edge deployment framing
- [LearnOpenCV YOLO26 deployment](https://learnopencv.com/yolov26-real-time-deployment/) — INT8 quantization-friendliness
- [Ultralytics YOLO Evolution (arXiv 2510.09653)](https://arxiv.org/abs/2510.09653) — v5/v8/v11/v26 comparison

**ESP-DL / ESP32-S3**
- [espressif/esp-dl on GitHub](https://github.com/espressif/esp-dl) — v3.2.0, YOLO26 added 2026-03-12
- [ESP-DL: How to deploy YOLO11n](https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_deploy_yolo11n.html) — canonical workflow, S3 benchmarks
- [ESP-DL: How to quantize model](https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_quantize_model.html) — ESP-PPQ reference
- [espressif/esp-detection](https://github.com/espressif/esp-detection) — ESPDet-Pico, the only fast YOLO on S3
- [BoumedineBillal/yolo26n_esp](https://github.com/BoumedineBillal/yolo26n_esp) — first YOLO26n on ESP (P4-only, 1.77 s/frame)
- [Elecrow: YOLOX-Nano on ESP32 HMI](https://www.elecrow.com/blog/real-time-person-counting-on-esp32-hmi-with-yolox-nano.html) — 4–6 FPS YOLOX-Nano on S3
