# Cry-detect starter plan — options to lock before code

Project: pretrained audio classifier on the Waveshare ESP32-S3-CAM-GC2145. Starts from a downloadable model; builds up in stages. Training-free first go.

This doc captures the option space identified after `crying-detection-s3-ml-alternatives.md` recommended the pretrained path. Three decisions to lock before scaffolding `projects/cry-detect/`. Staged roadmap at the bottom.

---

## Decision 1 — Which YAMNet variant?

YAMNet is Google's audio-event classifier, trained on AudioSet (521 classes including `Baby cry, infant cry` at class index 20). Two public variants fit on S3:

| Variant | Params | Flash (INT8) | Input | Output | S3 latency (est.) | Host |
|---|---|---|---|---|---|---|
| **YAMNet-1024** (original) | 3.2 M | ~3.2 MB | 96×64 log-mel | **521-class logits** + 1024-D embedding | ~200 ms | `STMicroelectronics/yamnet` on HF |
| **YAMNet-256** (STMicro distilled) | ~0.9 M | ~400 KB | 96×64 log-mel | 256-D embedding only (no classifier head) | ~80 ms | same HF repo |
| Google `yamnet.tflite` (float16) | 3.2 M | ~15 MB | 16 kHz waveform | 521-class logits + embedding | — | tfhub; too large, no INT8 |

### Tradeoffs

| Axis | YAMNet-1024 | YAMNet-256 |
|---|---|---|
| Inference speed | slower (~200 ms / 1 s audio) | faster (~80 ms) |
| Stage 1 effort | **`if output[20] > threshold: alert`** — done | Need to compute reference cry embedding from ~5 exemplar clips, then cosine-similarity at runtime |
| Accuracy out of the box | 85–90 % zero-shot on cry class | 80–88 % zero-shot via cos-sim |
| Flash footprint | 3.2 MB (fits 16 MB flash comfortably) | 400 KB |
| PSRAM working set | ~1 MB activation | ~300 KB activation |
| Fine-tune later (Stage 4) | Add 1-layer head on 1024-D or intermediate embedding | Add 1-layer head on 256-D |

### Recommendation

**YAMNet-1024** for Stage 1. The 521-class head is the point — zero-shot works immediately on `output[20]`. YAMNet-256 is a downgrade for this specific use case because we lose the built-in classifier and have to construct a reference-embedding ourselves, which isn't zero-effort.

If Stage-1 latency (~200 ms per second of audio = 20 % CPU) turns out to pinch against the rest of the pipeline, switch to YAMNet-256 in Stage 2.

---

## Decision 2 — Runtime: TFLite Micro or ESP-DL?

YAMNet ships as `.tflite`. ESP-DL uses `.espdl` (FlatBuffers). Two paths:

| Path | How | Cost | Risk |
|---|---|---|---|
| **TFLite Micro** (`esp-tflite-micro` component) | Drop the `.tflite` directly into the project; call via TF interpreter | Add 100–200 KB runtime to flash; slower than ESP-DL on conv-heavy models | Well-trodden; YAMNet on TFLite Micro has working examples |
| **ESP-DL** via `.espdl` | Export YAMNet PyTorch/TF → ONNX → quantize with ESP-PPQ → `.espdl`; deploy via ESP-DL | ESP-PPQ pipeline is a one-time step; faster runtime (PIE vector ops) | YAMNet op coverage in ESP-PPQ v3.2.0 not yet verified for us; risk of blocked ops we'd have to work around |

### Tradeoffs

- TFLite Micro: **working in a day**, slower, larger runtime, battle-tested.
- ESP-DL: **faster at runtime**, sharper integration with ESP-SR pipeline, but first-time conversion has unknown pitfalls for YAMNet specifically.

### Recommendation

**TFLite Micro for Stage 1.** Stage 2+ can migrate to ESP-DL only if benchmarks show we need the headroom. The stage-1 goal is "does the detector work at all," not peak performance.

---

## Decision 3 — Feature pipeline: bring-your-own-mel or YAMNet expects waveform?

YAMNet's published `.tflite` has two input variants:

| Input | Shape | Who computes mel? |
|---|---|---|
| **Waveform** | `[16000]` float (1 s @ 16 kHz) | Model internally computes log-mel |
| **Mel patch** | `[96, 64]` float log-mel | You pre-compute with ESP-DSP or ESP-SR |

### Tradeoffs

- Waveform input: easiest wiring — feed ES7210 PCM directly. But the in-graph mel computation is slow and un-quantized.
- Mel-patch input: you handle STFT + mel filterbank via ESP-DSP (~0.25 ms/frame — already well-characterised in the C6 doc), feed quantized mel patch in. Faster, but writing the mel extractor is a task.

### Recommendation

**Mel-patch input.** We'll need a mel extractor for Stage 2 (AFE integration) anyway, and the C6 research already defined the parameter set: 16 kHz, 32 ms frames, 16 ms hop, 40 mel bands (YAMNet uses 64 — adjust). Cheaper inference, and forward-compatible with Stage 2.

---

## Decision 4 (minor) — Project scaffolding

- **Location:** `projects/cry-detect/`
- **Seed from:** `ref/demo/ESP32-S3-CAM-OVxxxx/examples/ESP-IDF-v5.5.1/02_esp_sr/` — already has ES7210 + I²S + ESP-SR wired up. Strip WakeNet, keep the audio capture harness.
- **Dependencies:**
  ```yaml
  dependencies:
    espressif/esp-tflite-micro: "^1.3.4"
    espressif/esp-dsp: "^1.5.0"
    waveshare/esp32_s3_cam_ovxxxx: "^1.0.0"
  ```
- **Partition:** YAMNet-1024 at ~3.2 MB forces a non-default partition table — either enlarge `factory` to 5 MB, or add a SPIFFS partition named `model` for the `.tflite`. The `02_esp_sr` example already uses a custom partition (`model` SPIFFS 5.9 MB) — reuse the same layout, drop the ESP-SR model for YAMNet.

---

## Staged roadmap

| Stage | Adds | Training? | Expected accuracy | Est. effort |
|---|---|---|---|---|
| **1** | YAMNet-1024 via TFLite Micro, mel-patch input, energy-VAD gate, threshold on class-20 logit, LED + MJPEG placeholder overlay | None | 85–90 % | 3–4 days |
| **2** | Replace energy VAD with **ESP-SR AFE** (AEC + BSS + NS + VADNet). Feed AFE output to mel extractor | None | 88–92 % (↓ false-positive on TV/music) | +2 days |
| **3** | **Vision gate**: when cry-logit ∈ [low, high], run HumanFaceDetect on a camera frame; confirm only if both fire | None | 93–96 % (joint FP rate << either alone) | +2–3 days |
| **4** *(optional)* | Fine-tune a 1-layer linear head on ~100 labeled cry / not-cry clips. Freeze YAMNet backbone. | ~5 min on CPU | 95–97 % | +1 day (data collection is the bottleneck) |

Stages 1–3 are all pretrained-only. Stage 4 is the first time a training step enters — and it's a softmax fit, not a CNN-from-scratch loop.

---

## Open questions to answer during Stage 1

1. **Actual S3 latency** of YAMNet-1024 via TFLite Micro at INT8 — the ~200 ms estimate is from Google+STMicro benches; our number may differ.
2. **Real-world FP rate on TV/music/adult speech** — does the "85–90 %" number hold in a noisy room? Record 30 min of household ambient, run the classifier offline, count FPs.
3. **Is the `Baby cry, infant cry` logit separable from `Crying, sobbing` (class 21) and `Baby laughter` (class 22)?** — these are adjacent AudioSet classes. May need a combined rule (class-20 OR class-21 above threshold).
4. **Mel-patch parameter match** — YAMNet expects 96 frames × 64 mel bands, STFT at specific FFT size. Confirm exact values from the TFHub model card before wiring the extractor.

---

## Sources

- [STMicroelectronics/yamnet (HuggingFace)](https://huggingface.co/STMicroelectronics/yamnet) — INT8 TFLite for YAMNet-256 and YAMNet-1024
- [TensorFlow Hub YAMNet](https://tfhub.dev/google/yamnet/1) — original 3.2 M model, AudioSet class list
- [AudioSet ontology](https://research.google.com/audioset/ontology/baby_cry_infant_cry.html) — class 20 = `Baby cry, infant cry`
- [`esp-tflite-micro` component](https://components.espressif.com/components/espressif/esp-tflite-micro)
- [ESP-SR Audio Front-end (S3)](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html)
- Companion doc: `crying-detection-s3-ml-alternatives.md` (this dir)
- Sibling C6 research: `/Users/chayut/repos/ESP32-C6-Touch-AMOLED-1.8/14-crying-detection-research.md`
