# docs/research/

Pre-project research for the ESP32-S3-CAM-GC2145 workspace. Every non-trivial finding that informs a project decision is captured here *before* code starts, so we don't lose it to conversation scroll. Chronological order:

| # | File | What's in it |
|---|---|---|
| 1 | [`yolo26-and-esp-dl-2026.md`](yolo26-and-esp-dl-2026.md) | YOLO26 (Ultralytics 2026-01) architecture + variant table; ESP-DL v3.2.0 state; published S3/P4 latency numbers for YOLO11n, YOLO26n, ESPDet-Pico; ESP-PPQ quantization workflow |
| 2 | [`feasibility-s3-camera-project.md`](feasibility-s3-camera-project.md) | Reassessment: full YOLO26n on S3 = 10–26 s/frame, not real-time. Three paths (ESPDet-Pico / slow-YOLO26 / swap to P4) with arithmetic on why each behaves as it does. Recommended project shape + core/memory budget. Updated 2026-04-17 after prior-art survey |
| 3 | [`prior-art-survey.md`](prior-art-survey.md) | What's actually shipped on GitHub vs what's missing. Conclusion: the on-device-YOLO + RGB565-overlay + MJPEG-stream combo is not a public example. Four stackable novelty angles ranked by effort |
| 4 | [`yolo26-s3-port-plan.md`](yolo26-s3-port-plan.md) | *Archived reference.* Step-by-step plan for porting YOLO26n to ESP32-S3 (training → ESP-PPQ quantization → custom postprocessor → S3 tuning). Not an active milestone; captured in case Milestone 3 is revived |
| 5 | [`pretrained-espdl-inventory.md`](pretrained-espdl-inventory.md) | Full audit of pre-built `.espdl` models publicly available (Espressif repos, HuggingFace, community). Confirms no multi-class training-free fast option exists; defines Options X/Y/W for training-free projects |
| 6 | [`crying-detection-s3-ml-alternatives.md`](crying-detection-s3-ml-alternatives.md) | ML approaches for baby-cry detection on S3 that the sibling C6 project (`ESP32-C6-Touch-AMOLED-1.8/14-crying-detection-research.md`) had to reject due to resource limits: pretrained YAMNet, ESP-SR AFE, multi-modal audio+vision, beamforming. Training-free shortlist included |
| 7 | [`cry-detect-starter-plan.md`](cry-detect-starter-plan.md) | Options-locking doc for `projects/cry-detect/`: YAMNet variant (1024 vs 256), runtime (TFLite Micro vs ESP-DL), feature input (waveform vs mel-patch), 4-stage roadmap ending with optional linear-head fine-tune |
| 8 | [`hw-verification-stage1.md`](hw-verification-stage1.md) | Ground-truth hardware audit for Stage 1 against the vendor BSP source. Confirms **SD card does NOT have the C6 SPI-sharing issue** (S3 has native SDMMC host; no onboard display). Defines web-UI-as-runtime-display approach. Boot sequence, pinout, partition table, risk log |

## Current working plan (as of 2026-04-17)

Direction switched from vision to audio after reviewing the sibling C6 project's cry-detection work. Current focus: **pretrained audio classifier on S3** via YAMNet, building up in stages. See `cry-detect-starter-plan.md` for the staged roadmap (Stage 1 = YAMNet zero-shot, Stages 2–3 add AFE + vision gate, Stage 4 optional linear-head fine-tune).

Vision project options (cat/MJPEG overlay paths) remain catalogued in `pretrained-espdl-inventory.md` as alternates.

Archived alternatives:
- Multi-class ESPDet-Pico retrain (training loop, de-prioritised).
- YOLO26n port to S3 (see `yolo26-s3-port-plan.md`).

If any of these change or a new finding emerges, update the relevant doc above (or add a new one) before the implementation starts.
