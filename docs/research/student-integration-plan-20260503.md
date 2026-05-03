# Distilled student integration plan (cry-detect-01 firmware)

**Date:** 2026-05-03
**Target firmware:** `projects/cry-detect-01`
**Source artifact:** `chayuto/yamnet-cry-distill-int8` v0.2.0 candidate (EXP-008 INT8, 110 KB)
**Sibling repo:** `chayuto/yamnet-cry-distill-int8` — distillation training pipeline

## Why this plan exists

We've trained a tiny INT8 cry detector (110 KB, 80 K params) by
distilling YAMNet down. The training repo measured AUC 0.870 on a
public AudioSet test slice and verified the student tracks the
teacher well across all confidence tiers of our captures (frame
correlation 0.81 on high_pos, 0.29 on high_neg).

Those numbers are evaluator artifacts. The actual question for the
deployment is: **does the student behave like the teacher *on this
ESP32-S3, in this nursery, in real time?*** Nothing measured offline
proves that. This plan covers the firmware work to find out.

## Current state — what's already on the device

The cry-detect-01 firmware already runs an INT8 TFLite-Micro
inference loop. Key files in `projects/cry-detect-01/main/`:

| file | role |
|---|---|
| `yamnet.cc` / `yamnet.h` | TFLite-Micro wrapper. Loads `yamnet.tflite` from SPIFFS, allocates a tensor arena in PSRAM, runs `Invoke()` per 96×64 INT8 mel patch, exposes `cry_conf` from the baby-cry class index. |
| `mel_features.{c,h}` | Real-time mel-feature extraction from I²S audio. Produces the same 96×64 patches the training pipeline computes via librosa. |
| `detector.c` / `detector.h` | Threshold + N-consecutive-frame streak + hysteresis hold-time. **This is already the temporal smoothing the training-side analysis recommended** — `consec_frames` is the N-of-M voter, `hold_ms` is the hysteresis. |
| `auto_trigger.h` | Wires detector state transitions to capture-recording. |

That means the *architecture* is already deployment-correct. The only
thing being swapped is the model file and the cry-score formula.

## What needs to change

### Component 1: a parallel `student.cc` / `student.h` wrapper

Mirror `yamnet.cc` but with three specific differences. See the
**Memory and latency budget** section below for sizing:

```c
/* student.h — public API, parallels yamnet.h */
esp_err_t student_init(const char *model_path, size_t tensor_arena_kb);
typedef struct {
    int8_t  cry_raw_int8;     /* class 19 raw int8 */
    int8_t  baby_raw_int8;    /* class 20 raw int8 */
    float   cry_conf;         /* softmax[19] + softmax[20], in [0, 1] */
    int32_t latency_ms;
} student_result_t;
esp_err_t student_run(const int8_t *patch_96x64, student_result_t *result);
```

Key differences vs `yamnet.cc`:

1. **Output post-processing.** YAMNet uses sigmoid (multi-label),
   so dequantizing the raw int8 at index 20 directly yields the
   baby-cry probability. Our student uses softmax (single-label
   distribution); we must dequantize the *full 521-class output*,
   apply softmax in float32, and sum classes 19 + 20 to get the
   cry score. Existing helper at
   `yamnet_get_confidences()` (lines 213-224 of yamnet.cc)
   shows the dequant idiom.

2. **Smaller tensor arena.** The teacher's arena is sized for
   ~4 MB-of-weights YAMNet. Our 80 K-parameter student needs a
   fraction of that. Start with **64 KB** and verify
   `arena_used_bytes()` after `AllocateTensors()`.

3. **Same op set, smaller subset.** Our student uses
   `Conv2D`, `DepthwiseConv2D`, `FullyConnected`, `Softmax`,
   `Reshape`, `Mean` (for GAP), `BatchNormalization` (folded into
   Conv at TFLite export, so no Norm op needed at runtime).
   `register_ops()` in yamnet.cc already covers all of these;
   reuse the same OpResolver code path.

### Component 2: `CMakeLists.txt` update

```cmake
idf_component_register(
    SRCS
        ...existing sources...
        "student.cc"        # new
        ...
    ...
)
```

### Component 3: `detector.c` integration — phased

The detector module is already model-agnostic; it consumes a `cry_conf`
float and decides state. Two phased rollouts:

**Phase A — side-by-side logging (build-time flag `CONFIG_STUDENT_PARALLEL`)**

After `yamnet_run()` produces its `cry_conf`, *also* call `student_run()`
on the same `patch_96x64` buffer. **Log both confidences** but only
let the teacher's confidence drive `detector_submit()`.

```c
yamnet_result_t y_result;
student_result_t s_result;
yamnet_run(patch, &y_result);
#if CONFIG_STUDENT_PARALLEL
student_run(patch, &s_result);
log_pair(y_result.cry_conf, s_result.cry_conf);  /* breadcrumb */
#endif
detector_submit(y_result.cry_conf);  /* teacher still drives state */
```

This collects the on-device truth-data we need: simultaneous
teacher and student scores on the same audio, with clock alignment.
After 1-2 nights of recording, we know whether the student tracks
on-device matches what we measured offline.

**Phase B — student drives detector**

Once Phase A confirms agreement, swap which model drives state:

```c
detector_submit(s_result.cry_conf);
```

The threshold needs recalibration (see **Threshold calibration**
below). Keep teacher running for the first few nights as a
fallback / spot-check, gated on a Kconfig flag.

### Component 4: SPIFFS image

Add `model.tflite` (the student) to the SPIFFS image alongside
`yamnet.tflite` (the teacher). Both fit easily in the existing
SPIFFS partition; combined ~4.1 MB. In `partitions.csv` the
`spiffs` partition is currently sized for the teacher; it needs
~110 KB more.

### Component 5: `tools/fetch_model.sh` extension

Currently fetches the YAMNet teacher from
`chayuto/yamnet-mel-int8-tflm`. Add a parallel fetch for the
student from `chayuto/yamnet-cry-distill-int8`:

```sh
# Teacher (existing)
hf download chayuto/yamnet-mel-int8-tflm yamnet.tflite \
    --local-dir spiffs/

# Student (new)
hf download chayuto/yamnet-cry-distill-int8 model.tflite \
    --local-dir spiffs/
```

## Memory and latency budget

Heuristics from the training-side measurements (CPU, but
representative of relative sizing):

| | YAMNet teacher | Student | Ratio |
|---|---:|---:|---:|
| Parameters | ~4 M | 80 K | 50× smaller |
| .tflite size | ~4 MB | 110 KB | 36× smaller |
| Tensor arena (estimate) | ~600 KB | ~64 KB | 9× smaller |
| Inference latency on CPU | ~12 ms | ~1 ms | 12× faster |

**On ESP32-S3 (240 MHz Xtensa LX7), expect:**

- Tensor arena < 96 KB (allocate 128 KB for headroom)
- Inference per patch: 2-5 ms (well under the 480 ms patch hop)
- Power impact vs teacher: lower (faster inference + smaller weight reads)

The 240 MHz Xtensa is roughly comparable to a 3-4 GHz x86 core for
TFLite-Micro INT8 inner loops on a per-cycle basis but with much
fewer cycles. The teacher already runs comfortably real-time, so
the student is guaranteed to.

## Threshold calibration

Offline measurements indicate:

- **Best-F1 threshold on AudioSet test:** 0.047 (FP32) / similar (INT8).
- **Captures-side cry-conf distribution** (from the offline coverage
  eval, EXP-008 INT8):

| tier | n | student_max mean | student_max p10 | student_max p90 |
|---|---:|---:|---:|---:|
| high_pos | 197 | 0.816 | (TBD) | (TBD) |
| medium_pos | 35 | 0.623 | (TBD) | (TBD) |
| low | 81 | 0.422 | (TBD) | (TBD) |
| medium_neg | 12 | 0.159 | (TBD) | (TBD) |
| high_neg | 150 | 0.056 | (TBD) | (TBD) |

(Exact percentiles in
`docs/experiments/eval_home_captures_coverage_exp008_int8.json`,
gitignored.)

The clean separation between high_pos (0.82 mean) and high_neg
(0.06 mean) suggests a per-window threshold around **0.20–0.30**
for deployment, with `consec_frames=5` for temporal smoothing —
expected to give <5 false-alert clusters per 8-hour night.

The proper calibration is on-device:

1. Record 10 minutes of ambient nursery audio (no baby crying).
2. Run student over the recording, log per-frame cry-conf.
3. Pick threshold = max(cry_conf) + 0.05 margin.
4. Verify on a known cry recording (one of the 197 high_pos
   captures replayed via the device's own audio path).

## Side-by-side validation plan

Phase A (parallel logging) generates the ground truth. Specifically:

1. Flash `CONFIG_STUDENT_PARALLEL=y` build, with both models loaded.
2. Run for 8 hours of nursery audio (one full night).
3. Log every patch's `(timestamp, teacher_cry_conf, student_cry_conf)`
   to `breadcrumb` (existing breadcrumb module).
4. Pull the breadcrumb log via existing nightly-cycle export.
5. Offline analysis (in training repo): compute per-frame Pearson
   correlation, plot teacher vs student over time, identify
   divergences.

The acceptance criterion: **≥ 0.85 frame-level Pearson correlation
between on-device teacher and on-device student over the night.**
Anything below that means quantization or feature-pipeline drift,
not just model differences.

## Risks and mitigations

| risk | likelihood | mitigation |
|---|---|---|
| Student arena estimate wrong, `AllocateTensors` fails | medium | Use the existing pattern: pass arena KB via Kconfig, log `arena_used_bytes`, iterate. |
| Mel features differ between librosa (training) and on-device (inference) | medium | The teacher already runs on-device with this mel pipeline and matches its training-time AUC reasonably well. Same pipeline, same student behavior expected. Verify in Phase A logging. |
| Op missing from MicroMutableOpResolver | low | Student uses standard ops covered by `register_ops()` in yamnet.cc. If softmax (probably; YAMNet uses sigmoid) is missing it's a one-line add. |
| Threshold drift across deployments | high | Document that threshold calibration is per-device. Add a Kconfig for `CONFIG_CRY_DETECT_THRESHOLD` and a one-page calibration recipe. |
| Student over-confidence creates false alerts in production | medium | Phase A side-by-side logging surfaces this before Phase B switchover. Don't release Phase B until on-device disagreement <15 %. |

## Implementation order

1. **Add `student.cc` / `student.h`** — copy yamnet.cc, change
   model-loading path, add softmax-based cry-conf computation.
   ~2 hours work.
2. **Add CMakeLists changes + Kconfig flag** — ~30 minutes.
3. **Verify on-device boot** — flash, watch logs, confirm
   `student_init` succeeds and `arena_used_bytes` is sane. ~1 hour.
4. **Add side-by-side logging in detector path** — ~1 hour.
5. **Run one night with both models, pull breadcrumb log** — ~9 hours
   (8 of which is overnight).
6. **Analyze breadcrumb log offline** — score correlation + plots.
   ~30 minutes.
7. **Write up findings as `student-on-device-validation-<date>.md`**
   under sibling-repo `docs/experiments/`. ~1 hour.
8. **Decide:** if correlation ≥ 0.85, proceed to Phase B (student
   drives detector). If lower, debug.

Total to first on-device measurement: **one work day + one overnight
run**. Total to Phase B switchover: **2-3 work days** including
debug headroom.

## Cross-references

- Source pipeline + offline eval: <https://github.com/chayuto/yamnet-cry-distill-int8>
- Methodology — teacher-as-filter: `yamnet-cry-distill-int8/docs/research/methodology-teacher-as-filter.md`
- Captures-coverage findings (private): `yamnet-cry-distill-int8/docs/experiments/eval_home_captures_coverage_*.json` (gitignored)
- Existing on-device YAMNet integration: `projects/cry-detect-01/main/yamnet.{cc,h}`
- Existing detector logic (already does N-of-M voting): `projects/cry-detect-01/main/detector.{c,h}`
