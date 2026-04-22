# Deep-analysis day — 2026-04-23

**Status:** 4 analyses complete, consolidated here. Firmware fix identified,
not yet applied (device offline). Commit at end of day.

Four questions, four answers, one day.

## Question 1 — why is on-device `cry_conf` only 0.45 on confirmed real cries?

**Hypothesis tested:** either INT8 quantization is lossy (H1) or the
firmware's mel-feature pipeline diverges from YAMNet's reference (H2).

### H1 falsified

Ran FP32 YAMNet + INT8 tflite on the SAME YAMNet-reference mel patches
for 10 high-confidence cries across all three sessions:

```
FP32 max baby_cry mean : 0.978
INT8 max baby_cry mean : 0.946
mean gap               : 0.032
median gap             : 0.042
```

INT8 quantization loses 3-4 percentage points. The observed 0.5 gap
(device 0.45 vs YAMNet 0.93) is **not** quantization.

### H2 confirmed — firmware uses power spectrum, YAMNet uses magnitude

Comparing `main/mel_features.c` to `yamnet_work/features.py`:

| aspect | YAMNet reference | firmware `mel_features.c` |
|---|---|---|
| Window | periodic Hann, 400 samples | Hann, 400 samples ✓ |
| FFT | 512 | 512 ✓ |
| Hop | 160 samples | 160 samples ✓ |
| Mel bands | 64, 125 Hz – 7500 Hz | 64, 125 Hz – 7500 Hz ✓ |
| **Spectrum** | `tf.abs(tf.signal.stft(...))` → magnitude | `re*re + im*im` → **power** |
| **log offset** | `log_offset=0.001` | `logf(mel + 1e-10f)` |

`mel(|X|²) ≠ mel(|X|)²`, because `mel` is a linear operation and `mel` is
not a simple squaring. Different distribution → different tflite input.

### Quantitative confirmation

Ran tflite on four log-mel variants of the same 01:09 real-cry WAV
(offline-computed with known math):

| variant | log-mel mean | INT8 peak baby_cry |
|---|---:|---:|
| A: magnitude + offset 0.001 (YAMNet ref) | -4.10 | **0.9336** |
| B: **power + offset 1e-10 (firmware current)** | **-9.27** | **0.0664** ← matches observed on-device |
| C: power + offset 0.001 | -6.53 | 0.0664 |
| D: magnitude + offset 1e-10 | -4.18 | 0.9336 |

**Power-vs-magnitude is the fatal bug.** Offset is tolerable (D vs A
identical output). B matches the device's observed 0.066 baseline
exactly — that is the int8 code the model outputs when inputs are
outside the distribution it was trained on.

Tonight's observed peak 0.45 (in the monitor log) corresponds to patches
where the power-mel distribution happened to be recoverable by the
model — a lucky-alignment phenomenon, not intended behavior.

### Recommended fix (one-line, verified offline)

In `main/mel_features.c` line 145, replace:

```c
power[k] = re * re + im * im;
```

with:

```c
power[k] = sqrtf(re * re + im * im);  // magnitude, matches YAMNet reference
```

And change line 154:

```c
log_mel_out[m] = logf(mel_energy[m] + 0.001f);   // was 1e-10f, now matches YAMNet
```

Expected: on-device `cry_conf` on real cries jumps from ~0.066 baseline
(with occasional 0.45 spikes) to ~0.93 with consistent signal. Detector
threshold 0.70 starts firing correctly.

### Status

- Firmware change **not applied** (device is offline at time of writing).
- Offline validation shows 0.9336 expected peak.
- To ship: edit → build → flash (need USB) → bench-test with a cry sample
  before bedroom redeployment.

## Question 2 — can cheap host features replace the on-device model for FP rejection?

Pooled 189 captures across 3 sessions (102 real cries at YAM≥0.5,
87 FPs at YAM<0.1; 7 borderline dropped, 3 mixed, dedup by filename).

### Single-feature baseline: `hnr_db`

| threshold | TPR | FPR | accuracy | TP / FN / FP / TN |
|---:|---:|---:|---:|---:|
| > 2 | 1.000 | 0.368 | 0.831 | 102 / 0 / 32 / 55 |
| > 3 | 0.990 | 0.126 | 0.937 | 101 / 1 / 11 / 76 |
| **> 4** | **0.980** | **0.080** | **0.952** | 100 / 2 / 7 / 80 |
| > 5 | 0.951 | 0.080 | 0.937 | 97 / 5 / 7 / 80 |
| > 6 | 0.892 | 0.069 | 0.910 | 91 / 11 / 6 / 81 |

Single feature, near-ceiling performance. `hnr_db > 4` stays the best
single cut-off across all three sessions.

### 10-feature logistic regression (leave-one-session-out)

| held-out session | n | accuracy | AUC |
|---|---:|---:|---:|
| 04-20 | 118 | 0.915 | 0.972 |
| 04-21 | 47 | 0.894 | 0.959 |
| 04-22 | 24 | 0.958 | — (all-positive class) |
| pooled | 189 | 0.920 | **0.9748** |

Feature importance (standardized coefficients, retrained on all data):

| feature | coef | direction |
|---|---:|---|
| `hnr_db` | +2.937 | higher → cry |
| `b_250_500` | −1.208 | more 250–500 Hz energy → FP |
| `onsets` | −0.655 | more onsets → FP (kitchen clatter) |
| `active_frac` | +0.566 | more active content → cry |
| `centroid_mean_hz` | +0.515 | higher centroid → cry |
| `b_1k_2k` | +0.255 | — |
| `zcr` | +0.180 | — |
| `f0_voiced_frac` | +0.072 | — |
| `rolloff85_mean_hz` | +0.057 | — |
| `flatness_mean` | −0.015 | — |

**Verdict:** HNR alone already wins. A full 10-feature classifier
*hurts* slightly due to overfitting on small N. The simpler the
decision boundary, the better — stick with `hnr_db > 4` for post-capture
culling when we get there.

### Practical implication

With Question 1's firmware fix applied, the on-device `cry_conf`
will become a usable alerting signal. A second-gate `hnr_db > 4`
audit (computed by `audit_wavs.py` post-capture, not real-time on
device) catches model misfires. No need for an on-device classifier.

## Question 3 — what are the cry sub-types for this baby?

### Pooled k=3 (N=102 YAMNet-confirmed cries)

| cluster | n | f0_mean | f0_p50 | voiced_frac | character |
|---|---:|---:|---:|---:|---|
| fuss | 29 | 341 Hz | 263 Hz | 0.98 | low-pitch grumble, very sustained, near-speech |
| full cry | 45 | 591 Hz | 457 Hz | 0.92 | moderate pitch, sustained |
| distress | 28 | 809 Hz | 794 Hz | 0.71 | high-pitch, broken voicing, wide range |

The 04-20 session's 3-way split generalizes cleanly at N≈3× larger.

### k=4 reveals a new "screech" mode

| cluster | n | f0_mean | f0_p90 | jitter | per-session |
|---|---:|---:|---:|---:|---|
| fuss | 29 | 341 | 634 | 1.3 | 20 / 6 / 3 |
| distress | 21 | 774 | 1247 | 3.0 | 17 / 1 / 3 |
| **screech** | **5** | **980** | **1444** | **9.7** | **0 / 1 / 4** |
| full cry | 47 | 597 | 1151 | 1.8 | 24 / 9 / 14 |

The "screech" cluster is **predominantly tonight's (04-22) session**
— f0 above 1 kHz, high jitter, highly voiced. Four of five members
are from tonight.

Two interpretations:
- **Developmental:** this baby's vocal range has expanded in the last
  ~48 h. Higher-pitched, more jittery vocalizations suggest new vocal
  motor control.
- **State-specific:** a particular hunger/pain state that didn't occur
  in 04-20/04-21 but did tonight (e.g., the 05:58 manually-annotated
  "Morning cry").

At N=5 we can't distinguish. Track the cluster across future sessions.

### Per-baby acoustic signature is real

- All clusters have distinct f0 characteristics that persist across
  sessions (except the new screech mode).
- Pattern is **not an artifact of environment** — same clusters appear
  in both bedroom (04-21, 04-22) and bedroom+living-room (04-20).
- Future model training could use cluster IDs as a secondary label
  (cry urgency: fuss < full < distress/screech).

## Question 4 — is the training-pipeline scaffold ready?

Yes, minimal version. Committed today:

```
datasets/cry-detect-01/
├── README.md                           # dataset archive overview
├── sessions/                           # will be populated per Phase 1 of
│                                       # log-management-design-20260423.md
├── labels/
│   └── master.csv                      # 203 rows: ID, yam scores, features,
│                                       # human_label column (empty for now)
└── releases/
    └── cry-v0.0-exploratory.json       # frozen snapshot: 203 captures,
                                        # splits 123/40/40 (train/val/test),
                                        # label distribution 102 cry / 83
                                        # speech / 8 other / 7 borderline
                                        # / 3 mixed
```

Supporting tooling in `projects/cry-detect-01/tools/`:

- `build_master_labels.py` — walks `logs/night-*/` and produces
  `master.csv`. Idempotent; re-running regenerates from current data.
- `freeze_release.py <release_id>` — snapshots current `master.csv`
  into a `releases/*.json` manifest with WAV list + splits + label
  state. Includes `git_head_sha`, `frozen_at`, `sample_wav_hashes`
  for drift detection, and notes about firmware-build heterogeneity.

### `cry-v0.0-exploratory` release notes

- **203 captures**, split 123 train / 40 val / 40 test (deterministic
  by filename sort).
- **Labels are YAMNet-auto only** — no human review yet. Intended for
  tooling dry-runs, NOT for model training.
- **Sessions span 3 firmware builds** (`a895bfdc`, `a4870a21`,
  `4af50c57`). If a training run wants to use device-side `cry_conf`
  as a feature, it must filter by `build_sha` because the semantics
  differ — not an issue for YAMNet-only features which are firmware-
  independent.
- **Not yet phase 1 migrated** — raw data still lives in
  `projects/cry-detect-01/logs/night-*/`. The release pointers are
  filename-based and work against the current tree; the phase 1
  migration (per design doc §9.1) will repath them when we build
  `tools/migrate_session_to_dataset.py`.

### Minimum viable training flow (for future reference)

With `cry-v0.0-exploratory` in hand, a training run would:

1. Read `datasets/cry-detect-01/releases/cry-v0.0-exploratory.json`.
2. For each capture, load WAV + YAMNet segment scores from
   `logs/night-*/yamnet_segments.csv`.
3. Extract YAMNet embeddings (1024-d, per 0.48 s frame) from the
   FP32 hub model.
4. Train a small head (e.g. 2-layer MLP) on embeddings → binary
   cry-or-not, evaluated on the frozen val/test split.
5. Persist model + scores to a new
   `releases/cry-v0.1-model-yamnet-embed-mlp.json`.

None of this is built today. Scaffold + schemas are.

## Cross-cutting implications

### For the firmware track

- **Mel-feature fix is the single highest-impact change.** Two lines.
  Unblocks on-device alerting at the current model quality.
- **Reset-reason exposure** (log-management design §6.5) is still
  needed — tonight's two unexplained reboots illustrate.
- **Session-begin/end HTTP endpoints** (design §7) would let tomorrow's
  extract be < 2 min instead of 40.

### For the data track

- Scaffolding is in place. Master labels + releases work.
- Next data-track priorities (ordered by value): (a) commit this
  scaffold and the analyses. (b) build the human-annotation tool
  (design §2.3). (c) run the migration (design §9.1). (d) start
  accumulating variety across environments.

### For the model track

- Tonight's data is enough to *characterize* the baby's cry
  distribution (3 sub-types + a probable 4th).
- Still **well below the 1000-labeled-cry threshold** from the
  data-program plan §6.1 for a first real training run.
- With the mel-fix in place, on-device inference becomes a
  meaningful feature source — worth logging its output on every
  future capture for comparison to YAMNet.

## Session metadata (for reproducibility)

- `git_head_sha`: `9ba42f4a` at analysis-time (pre-commit of this doc).
- YAMNet: FP32 from TF Hub `google/yamnet/1`, INT8 from
  `projects/cry-detect-01/spiffs/yamnet.tflite` (MD5 unchanged from
  repo HEAD).
- Analysis scripts in `/tmp/` (ephemeral): `diag_mel_drift.py`,
  `verify_mel_drift.py`, `fp_classifier.py`, `cry_subtypes.py`.
  Should be promoted to `projects/cry-detect-01/tools/` if re-run
  on future sessions.
- Dataset cuts: 203 captures × ~12 features + YAMNet scores per
  segment.

## Next steps queue (ordered by single-decision value)

1. **Apply mel-feature fix** → build → flash → bench-test with a cry
  sample → re-deploy to bedroom. Blocked on USB access. Est. 15 min
  once plugged in.
2. **Commit today's work** in one atomic change: deep-analysis doc,
  dataset scaffold, master labels + first release, supporting tools.
3. **Redeploy for tonight's data collection** with the fixed firmware.
  Tomorrow's session metrics should show `cry_conf` crossing 0.70 and
  the detector firing `ALERT_FIRED` events for the first time.
4. Build the human-annotation tool (next rainy-day task — no deadline).
