# Crying detection on ESP32-S3 — ML approaches *not* covered in the C6 project

Companion to `<sibling-C6-repo>/14-crying-detection-research.md` (the C6 investigation, dated 2026-04-04). That doc landed on **TFLite Micro + MFCC + small CNN (Architecture C)** under C6 constraints (160 MHz single-core, no PSRAM, no FPU, no SIMD). This one maps the ML design space **S3 opens up that C6 had to reject** — so the S3 project can avoid rehashing the same path.

> Scope: audio-focused ML alternatives to the C6 recipe. Hardware baseline assumed: Waveshare ESP32-S3-CAM-GC2145 (dual-core LX7 @ 240 MHz, 8 MB PSRAM, PIE vector SIMD, FPU, ES8311 DAC, ES7210 4-ch ADC with dual PDM MEMS mics, GC2145 camera). Verified 2026-04-17.

---

## 1. What the S3 unlocks that C6 had to refuse

| Capability | C6 verdict | S3 verdict | Implication |
|---|---|---|---|
| **ESP-SR AFE** (AEC + BSS + NS + neural VAD) | ✗ needs PSRAM | ✓ supported — 1.1 MB PSRAM, 48 KB SRAM, 22 % CPU | Replace energy VAD with neural VAD; add acoustic echo cancellation if speaker plays back |
| **BSS beamforming** | ✗ | ✓ Espressif AFE does blind source separation across multiple mics | Steer listening toward the crib; reject TV-direction audio |
| **FPU (single-precision)** | ✗ software-emulated | ✓ hardware | Float mel spectrogram viable; no forced INT8 |
| **PIE vector SIMD** | ✗ | ✓ | 3–5× conv speedup vs reference kernels; larger CNNs become real-time |
| **PSRAM (8 MB)** | ✗ | ✓ 7.5 MB free to heap | Pretrained YAMNet / PANNs / small Transformer all fit |
| **Dual-core** | ✗ single-core | ✓ | Audio pipeline on core 1, Wi-Fi/LWIP on core 0 — no contention |
| **Camera (GC2145)** | ✗ no camera at all | ✓ | **Multi-modal**: confirm baby-in-frame before triggering on cry-like audio |
| **ES7210 4-ch ADC** | dual PDM via ES8311 | 4-channel designed for beamforming | Full BSS / DOA (direction of arrival) |
| **ESP-DL native runtime** | ✗ PSRAM-reliant | ✓ v3.2.0, Espressif-maintained | Alternative to TFLite Micro, uses PIE intrinsics directly |
| **Model zoo for audio events** | — | Partial (`esp-dl` + `esp-sr` ecosystems) | Pretrained ingestion paths exist |

Short version: every "not supported on C6" line in the C6 doc becomes "supported" on S3.

---

## 2. ML approaches the C6 couldn't do

Ranked by *added capability over the C6 recipe*, not by effort.

### 2.1 Pretrained audio embeddings (YAMNet-256 / PANNs) + linear head

The biggest delta. Instead of training a custom 2-conv CNN from scratch on ESC-50 + Donate-a-Cry, fine-tune only a small classifier head on top of a pretrained audio encoder that already "knows" 521 AudioSet classes.

| Model | Params | Input | Output | Fits on S3? |
|---|---|---|---|---|
| **YAMNet-1024** (orig) | 3.2 M | 96×64 log-mel | 521-logit + 1024-D embed | Yes (PSRAM) — ~3 MB flash |
| **YAMNet-256** (STMicro distilled) | ~0.9 M | 96×64 log-mel | 256-D embed | Yes easily — ~400 KB flash INT8 |
| **PANNs Cnn14-16K** | 80 M | 64×1001 | 527-class | Too big |
| **PANNs Cnn10 / MobileNetV2** | 5 M | 64×1001 | 527-class | Fits in PSRAM; slow |
| **AST-Tiny** | 6 M | 128×1024 mel | 527-class | Fits; ~500 ms/inf estimated |

Usage patterns:
1. **Zero training** — cosine-similarity between live 256-D embedding and a reference "Baby cry" embedding computed once on a few exemplar clips. Threshold on similarity. Pretrained YAMNet covers "Baby cry, infant cry" as one of its 521 classes natively — no training at all needed.
2. **Linear head fine-tune** — freeze YAMNet, train a 2-class softmax on top (cry vs not). ~10 labeled examples per class, <1 min on CPU. Much less data than training a CNN from scratch.
3. **kNN lookup** — cache N reference cry/not-cry embeddings, classify new audio by k nearest neighbours. On-device adaptation to *this baby's* cry over time.

**Not in the C6 doc** because YAMNet-1024 is 3 MB and doesn't fit in 452 KB DRAM. YAMNet-256 might fit on C6 but at ~200 ms inference with no SIMD / FPU it's marginal. On S3 it's comfortable.

### 2.2 Full ESP-SR AFE pipeline as pre-processing

Drop the energy VAD the C6 doc recommended. Replace with:

```
ES7210 4-ch → AFE (AEC → BSS → NS → VADNet neural) → mel spectrogram → classifier
```

- **AEC**: if the device plays back audio (white noise, lullabies) through ES8311, AFE cancels it from the mic input — otherwise playback would trigger false-positive "crying" detections.
- **BSS**: directionally isolates the dominant audio source. If you place the device with crib behind and TV in front, BSS can suppress the TV.
- **NS/NSNet**: deep noise suppression — boosts SNR before classification.
- **VADNet** (neural, 15K-hour trained): massively better than energy VAD; false-accept rate on ambient noise is ~5× lower.

Cost: 22 % CPU + 1.1 MB PSRAM (Espressif quoted). Budget: S3 has plenty.

**Not in the C6 doc** because C6 only supports WakeNet9s + VADNet — no AFE, no BSS, no AEC, no NSNet.

### 2.3 Multi-modal: audio + vision

Specific to this board (camera present):

```
Audio (cry-like)                    Vision (GC2145)
    │                                     │
[YAMNet head] → cry_conf          [HumanFaceDetect or ESPDet-Pico-baby]
    │                                     │
    └───── AND gate ────┬──────────────┘
                        │
                 "baby crying, verified"
```

Two stages, two models, one confirmation. Collapses false-positive rate (pets, TV, music) because crying-audio + baby-in-frame is a *much* rarer conjunction than either alone.

Cost model:
- HumanFaceDetect @ ~70 ms on S3. Run only when cry_conf > low threshold (~40 %).
- Audio pipeline runs continuously at ~5 % CPU.
- Vision kicks in maybe 1 % of the time. Total extra CPU: ~1 % average.

**Not in the C6 doc** because C6 has no camera. This is the single most S3-specific angle.

### 2.4 Beamforming + direction-of-arrival via ES7210 4-ch

Audio-only variant of multi-modal disambiguation. BSS gives a direction estimate; we can learn "the crib is at 30° azimuth" and weight detections from that direction 5× higher. Rejects TV (other direction), pets (other direction).

**Not in the C6 doc** because C6's two mics go through ES8311 (mono ADC); can't do real 2-mic DOA without separate ADCs.

### 2.5 Transformer-based audio models

Audio Spectrogram Transformer (AST) and variants treat mel patches like ViT patches. Strong accuracy on AudioSet. Tiny variants (~6 M params) are feasible on S3 via PSRAM but slow (~500 ms est.). Mostly interesting as a research angle, not a production choice.

**Not in the C6 doc** because transformers at any useful size exceed C6 RAM.

### 2.6 Raw waveform models (SincNet, M5)

Skip hand-crafted mel features entirely. 1-D convolutions directly on 16 kHz audio. M5 (~0.5 M params) is a reference architecture.

Pros: the network learns filters tuned to cry spectra; not bound by mel-scale design choices.
Cons: more compute than mel-spectrogram CNN (no free dimensionality reduction upfront).

Estimated S3 cost: 60–120 ms/inf on 1.5 s audio. Plausible.

**Not in the C6 doc** — raw-waveform convs without FPU/SIMD on C6 would blow the CPU budget.

### 2.7 Temporal/sequence models for cry-pause pattern

The C6 doc notes the cry-pause cadence at ~0.5–1 Hz is a highly distinctive feature — but its recommended CNN operates on a single 1.5 s window. A Temporal Convolutional Network (TCN) or small GRU reads a longer context (e.g. 5–10 s of mel frames) and captures the bursty pattern explicitly.

On S3 with PSRAM a 20–50 K-parameter TCN is cheap. Likely +1–2 pp accuracy vs the window-CNN on temporally structured sounds (crying, snoring, alarms) where "does it repeat?" matters.

**Not in the C6 doc** because sequence-model activation memory at 5–10 s context is awkward without PSRAM.

### 2.8 Multi-class cry-cause classification (not binary)

Research datasets (Baby Chillanto DB1) label cries by cause — hunger, pain, tired, discomfort. Requires more labeled data and a larger output head but the backbone cost is the same. On S3 with PSRAM headroom this is feasible; on C6 the C6 doc stuck to binary.

---

## 3. Runtime choice — ESP-DL vs TFLite Micro vs ESP-Skainet

The C6 doc recommended TFLite Micro because ESP-DL historically needed PSRAM and had thinner C6 support. On S3 all three are in play:

| Runtime | Strength | Weakness | S3 latency (ref) |
|---|---|---|---|
| **ESP-DL v3.2.0** | Native Espressif, PIE-vector-accelerated, `.espdl` FlatBuffers zero-copy, integrates with ESP-SR | Smaller model zoo for audio; newer tooling | Fastest for INT8 CNN |
| **TFLite Micro (`esp-tflite-micro`)** | Huge ecosystem, Edge Impulse exports directly, YAMNet-tflite ready-made | ANSI C kernels; PIE SIMD less fully leveraged than ESP-DL | 1.5–2× slower than ESP-DL on same model |
| **ESP-Skainet** | Integrated AFE + WakeNet + MultiNet pipeline, factory-tested | Speech-centric; sound-event detection is indirect | Best for wake-word-style triggers only |

**Revised recommendation for S3 (different from C6):** use **ESP-SR AFE for pre-processing** (the pipeline Espressif ships), then **ESP-DL for the classifier** if building from a `.espdl` model, or **TFLite Micro** if bringing a YAMNet-distilled / Edge Impulse export. They can coexist — AFE outputs PCM, the classifier doesn't care about the runtime that produced the features.

---

## 4. Training-free shortlist (matches user preference)

The user dislikes training loops. Here's the subset of §2 that skips custom training entirely:

| Approach | Training required? | S3 latency | Accuracy floor | Notes |
|---|---|---|---|---|
| **YAMNet-1024 zero-shot "Baby cry" logit** | None | ~200 ms | 85–90 % | Use the 521-class head directly, threshold on the Baby-cry-infant-cry logit |
| **YAMNet-256 + cosine similarity to reference embedding** | None (compute reference once) | ~80 ms | 80–88 % | Compute 5 reference cry embeddings, take mean, cos-sim at runtime |
| **YAMNet-256 + 1-layer linear head** | ~2 min (on CPU, ~100 labeled clips) | ~80 ms | 92–95 % | Freeze encoder, train softmax only — not really "training" in the heavy sense |
| **ESP-SR AFE + VADNet pre-filter only** | None | AFE 22 % CPU | N/A (not a classifier) | Strong noise-suppressed audio feed into ANY downstream classifier |
| **Multi-modal: YAMNet-1024 cry-logit + face/baby-detect** | None | ~200 ms + 70 ms | 93–96 % (FP ↓) | Two zero-shot models combined; see §2.3 |

**My read:** the cleanest "no training" path is **YAMNet-1024 zero-shot logit on the `Baby cry, infant cry` class + ESP-SR AFE pre-processing + optional face-detect verification**. All three components are pretrained, downloadable, and run natively on S3 without writing any training loop.

---

## 5. How this differs from the C6 recommendation

| Dimension | C6 doc (2026-04-04) | S3 proposal (2026-04-17) |
|---|---|---|
| Pre-processing | Energy VAD | ESP-SR AFE (AEC + BSS + NS + VADNet) |
| Features | Mel spectrogram computed by hand via ESP-DSP | Same *or* fed straight from AFE output |
| Classifier | Custom 2-conv CNN trained from scratch | Pretrained YAMNet-256 + optional linear head |
| Training data | 2K+ positives, 2K+ negatives, augmented | 0 (zero-shot) or ~100 clips (linear-head) |
| Framework | TFLite Micro | ESP-SR + ESP-DL (or TFLite Micro if YAMNet-tflite direct) |
| Unique to S3 | — | Beamforming, multi-modal audio+vision, AFE |
| RAM | 40–60 KB tensor arena | 1.1 MB PSRAM AFE + ~1 MB PSRAM YAMNet — comfortable |
| Recommended accuracy | 92–96 % (trained CNN) | 92–96 % (YAMNet + AFE + optional face gate) |
| Training effort | **Medium–high** (data collection + training pipeline) | **Zero** (pretrained) to **low** (linear head) |

The C6 doc's core recommendation still works on S3 — it just stops being *necessary*. On S3 we can substitute pretrained/off-the-shelf components and achieve the same or better accuracy with no training loop.

---

## 6. Open questions worth a short experiment before committing

1. **Does YAMNet-256 fit and hit <100 ms on S3?** STMicro shipped it for STM32N6; S3 has PIE vector but no dedicated NN accelerator. Needs a benchmark run. YAMNet `.tflite` to `.espdl` conversion via ESP-PPQ is the direct path.
2. **YAMNet's `Baby cry, infant cry` logit on real-world noisy audio** — does it hold 85–90 % out of the box, or does it need thresholding and smoothing before it's production-viable? Easy to test on ESC-50 clips.
3. **ESP-SR AFE latency budget** — the 22 % / 1.1 MB PSRAM figure is a published number; need to measure actual on this hardware with ES7210 4-ch at 16 kHz.
4. **GC2145 rolling-shutter + low-light** — if the baby is asleep in the dark, can the face detector find a face at all? Might force us into thermal/IR assumption that doesn't match this SKU. If face detection is unreliable, the multi-modal §2.3 gate becomes less valuable.

---

## Sources

**Pretrained audio models**
- [STMicroelectronics/yamnet on HuggingFace](https://huggingface.co/STMicroelectronics/yamnet) — YAMNet-256 and YAMNet-1024 INT8 TFLite
- [TensorFlow YAMNet transfer-learning tutorial](https://www.tensorflow.org/tutorials/audio/transfer_learning_audio)
- [Converting YAMNet to TFLite for microcontrollers (Medium)](https://medium.com/@antonyharfield/converting-the-yamnet-audio-detection-model-for-tensorflow-lite-inference-43d049bd357c)
- [PANNs overview (Medium)](https://medium.com/@martin.hodges/identifying-sounds-using-python-application-and-a-pann-based-audio-model-e2a7dad60508)

**ESP-SR / AFE on S3**
- [ESP-SR Audio Front-end docs (S3)](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html)
- [Espressif ESP-AFE solutions](https://www.espressif.com/en/solutions/audio-solutions/esp-afe)
- [Alexa-certified ESP AFE on S3 (CNX-Software)](https://www.cnx-software.com/2021/07/30/amazon-alexa-certified-esp-afe-leverages-esp32-s3-ai-dsp-instructions/)

**Runtimes**
- [espressif/esp-dl](https://github.com/espressif/esp-dl)
- [esp-tflite-micro ESP Component](https://components.espressif.com/components/espressif/esp-tflite-micro)
- [espressif/esp-skainet](https://github.com/espressif/esp-skainet)

**Datasets / prior baby-monitor work** (consolidated from C6 doc, kept for convenience)
- [ESC-50](https://github.com/karolpiczak/ESC-50), [Donate a Cry](https://github.com/gveres/donateacry-corpus), [UrbanSound8K](https://urbansounddataset.weebly.com/urbansound8k.html)
- [E-Nanny (Hackster)](https://www.hackster.io/nafihahmd/e-nanny-esp32-based-smart-baby-sleep-monitoring-system-9e68fb)
- [avidadearthur/esp32-baby-monitor](https://github.com/avidadearthur/esp32-baby-monitor)

**Companion research**
- Sibling C6 investigation: `<sibling-C6-repo>/14-crying-detection-research.md`
