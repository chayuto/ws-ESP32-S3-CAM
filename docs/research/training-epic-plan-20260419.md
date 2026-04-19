# Training epic — game plan (2026-04-19)

*Stage-2 training work. Covers PTQ recalibration, replay validation, a
staged afternoon of dry redeploys, and overnight 2 deployment. Written
after mining the night-20260418 artifacts and discovering the on-device
auto-detector literally never fired across 10 hours.*

## 1. Why this exists

Two facts together forced this plan:

1. **Data readiness is there.** The night-20260418 capture yielded **295
   clean cry positives + 831 clean ambient** patches under FP32 YAMNet
   oracle thresholds, totalling 1,126 — comfortably above the 300–500
   target for INT8 PTQ recalibration (see `retraining-roi-analysis.md`
   §3.1). Bench files top out at `cry=0.34`; incident files span
   0.88–0.98 — clean separation.
2. **On-device detector is dead.** Over 35,727 rows of `infer-20260419.jsonl`
   (10 h), `cry_conf` was pinned in **\[0.563, 0.651\]** — never reached
   0.7, let alone 0.85. State never transitioned. All 14 incident WAVs
   were captured by the RMS-based `CRY_AUTO_TRIG_*` fallback, not by the
   cry-class detector. Quiet-period top-10 shows classes 173/262/194/238
   all stuck at ~0.67 — textbook synthetic-PTQ sigmoid bias.

The recalibrated model is already on disk as
`projects/cry-detect-01/spiffs/yamnet-recalib.tflite` (500 real
log-mel patches, calibrated via the vendored Google YAMNet at
`tools/convert_yamnet.py`, which was patched in this session to use
`waveform_to_log_mel_spectrogram_patches` — the old API the script was
written against is gone upstream).

## 2. What "done" looks like for this epic

- Primary: `cry_conf` idle median drops from 0.577 to **≤ 0.20** on the
  recalibrated model, while incident-WAV max stays **≥ 0.85**.
- Secondary: `CRY_DETECT_THRESHOLD` can drop from 40 (≈0.31 logit) to
  something empirically-picked that separates the two by ≥ 3 σ.
- Tertiary: an overnight 2 capture with auto-trigger actually firing on
  cry_conf (not just RMS fallback), and recorded event WAVs labelled via
  the on-device event recorder with correct cry_conf metadata.
- Stretch: enough additional positives to push the binary-head dataset
  (tier 3.2) over the 500-positive threshold.

## 3. Environment reality

We cannot reliably reproduce cry in the lab this afternoon (no baby
on-demand, no calibrated speaker playback rig in place). That rules out
the "provocation round" idea from the verbal plan. Everything we do
before overnight 2 must:

- either run on the **host** against recorded WAVs, or
- run on the **device** as an *ambient soak* where we only validate
  no-false-positives, not true-positives.

True-positive validation is deferred to overnight 2 — the same way
overnight 1 provided the evidence for this plan.

## 4. Parameter changes

| Kconfig | Current | Proposed | Rationale |
|---|---:|---:|---|
| `CRY_DETECT_MODEL_PATH` | `/yamnet/yamnet.tflite` | keep path, swap contents | rename `yamnet.tflite` → `yamnet-orig.tflite` on SPIFFS, promote `yamnet-recalib.tflite` → `yamnet.tflite`. Zero firmware change. |
| `CRY_DETECT_THRESHOLD` | 40 (≈0.31 logit) | set from replay | picked numerically after §6 finishes, not by guess |
| `CRY_DETECT_CONSEC_FRAMES` | 6 | 3 | 6 was a bias workaround ("raised 3→6 on 2026-04-18 after threshold dropped 0.85→0.65 to catch true cries"). Clean PTQ removes the need; 3 frames restores ~2 s detection latency. |
| `CRY_REC_KEEP_FILES` | 50 | 200 | overnight 1 produced 22 files in one event; if auto-trigger fires properly tonight we could easily fill 50 before midnight. |
| `CRY_AUTO_TRIG_ENABLED` | y | y (keep) | RMS fallback caught all 14 incident WAVs last night. Don't disable until cry-class trigger is proven in the field. |

**Leave alone:** mic gain (36 dB), sample rate (16 kHz), hold_ms (5000),
preroll/postroll (10/30 s), noise-floor warmup (300 s), log retention
(14 d), SSE max (2), stream ring (32 KB), tensor arena (1536 KB).

## 5. Afternoon rounds (revised for uncontrolled environment)

Each round is flash + boot + soak + measurement, ~45 min wall-clock.

### Round 0 — host-side replay harness (13:00–13:45)

Tool: `projects/cry-detect-01/tools/replay_yamnet.py` (new — to be
written). Loads `spiffs/yamnet-recalib.tflite` with `tflite_runtime` or
`tensorflow.lite.Interpreter`, computes log-mel patches identically to
the firmware's `mel_features.c`, runs inference patch-by-patch over each
of the 22 WAVs, dumps a CSV with columns
`(file, seg_idx, cry_conf_int8, cry_conf_float, delta_vs_fp32)`.

Success criterion: for the 14 incident WAVs the max `cry_conf_float`
should be ≥ 0.85; for the 8 bench WAVs it should be ≤ 0.4. If both hold,
pick `CRY_DETECT_THRESHOLD` at the midpoint logit value.

Also compute: **idle-period simulation** — run the first 2 hours of
quiet audio from the *boot jsonl* (actually we don't have raw quiet
audio, only 22 event WAVs — so use bench-file "silence between spoken
phrases" segments, i.e. segments in `segments.csv` with
`active_frac < 0.05`, as the idle proxy).

### Round 1 — flash + ambient soak (13:45–14:45)

Swap SPIFFS model, sdkconfig patches from §4, `/build`, `/flash`,
monitor. 45 min of quiet-room logging. Pull `/metrics` every 5 min via
curl.

Success criteria:
- Zero `state` transitions (auto-trigger stays idle) during the soak.
- `cry_conf` p95 ≤ chosen threshold − safety margin.
- Inference fps, overrun bytes, heap within §5 of overnight 1 steady
  state.

If cry_conf sits stubbornly near the threshold → raise threshold, or
raise CONSEC_FRAMES back to 4–5.

### Round 2 — replay-WAV smoke test (14:45–15:30)

Can we play a recorded cry WAV back into the mic? **Only if a portable
speaker is to hand.** If yes: play `cry-20260419T045426+1000.wav` (the
37.3% duty-cycle file) at moderate volume near the mic; confirm:

- `cry_conf` crosses threshold
- `/metrics state=alert` briefly
- an event WAV lands on `/sdcard/events/`
- SSE fires

If no speaker: **skip this round entirely**. Budget the time for Round 3
instead.

### Round 3 — final ambient soak (15:30–17:00)

Longer soak, maybe with intermittent normal-household noise (talking,
kitchen sounds, TV on low). Still validating *no-false-positives*; we
don't yet know the true-positive behaviour without a real cry.

Collect: one `/metrics` sample every 2 min, full `/files/get?path=...`
pull of the current `infer-YYYYMMDD.jsonl`, no-alert invariant.

### Round 4 — freeze + pre-flight (17:00–18:00)

Git diff review, commit the sdkconfig changes (with threshold in the
message), SD free-space check, NTP sync sanity, log retention running
(`/metrics` `logret_*` counters), audio overrun < 100 B/s steady state
(we measured 1 B/s this morning, should stay that way).

### Buffer (18:00–19:00)

Dinner. Pad for any failed round.

### Overnight 2 (19:00 – next morning)

Deploy. Don't touch. Expect real cry events to finally fire via the
cry-class path, not just RMS fallback.

## 6. Risks / what can derail this

1. **Replay doesn't show a clean separation.** If the recalibrated
   model's incident-WAV max stays below 0.85, PTQ alone isn't enough —
   we fall through to the binary-head path (tier 3.2) which we can't
   start today (needs more positives). Outcome: overnight 2 runs with
   the original model, and tomorrow's work pivots to collecting more
   positives.
2. **Recalibrated model behaves identically in-fw but differently
   in-host.** Possible if our on-device log-mel pipeline drifts from
   Google's reference. Mitigation: in the replay harness, dump one
   patch as float binary and compare byte-for-byte against the patch
   the firmware computed on the same WAV (needs a small debug endpoint
   — skip if too expensive, compare distributions instead).
3. **Threshold chosen from 22 WAVs doesn't generalise overnight.**
   Low prior probability — FP32 oracle already agreed on 1,826 segments
   — but real.
4. **SD card reseat failure repeats** (see
   `deployment-move-sd-failure-20260418.md`). Unrelated to training;
   separate risk.

## 7. What this plan explicitly does not do

- **No binary-head training (tier 3.2).** Not enough positives yet
  (295 vs. ~500 target). That's work for after overnight 2 or 3.
- **No full fine-tune (tier 3.4).** Per the ROI doc §3.4 it's
  strictly worse engineering.
- **No personalisation (tier 3.3).** Requires weeks of passive capture;
  defer to Stage 3.

## 7a. Root cause found mid-session — `dense` layer never loaded

First replay of the recalibrated model showed **negative** correlation
with the FP32 oracle. Diagnostic FP32-keras run revealed the actual bug:

- `.yamnet_work/yamnet.py:103` creates the final `Dense(521)` without
  `name=`. Keras names it `dense`.
- `yamnet.h5` stores those weights at `logits/logits/kernel:0` and
  `logits/logits/bias:0`.
- `model.load_weights(..., by_name=True, skip_mismatch=True)` looked
  for a layer named `logits`, didn't find one, silently skipped it.
- Result: every INT8 `.tflite` we have flashed since day 1 contained
  a **randomly-initialised 1024×521 classifier head** on top of the
  otherwise-trained MobileNetV1 backbone.

That single bug explains:

- Why idle `cry_conf` pegged at 0.57–0.62 — random dense + double
  sigmoid (see §7b) produces near-constant output regardless of input.
- Why the 14-WAV incident never tripped the threshold — the classifier
  had no signal to give.
- Why PTQ recalibration on bench-only WAVs *worsened* correlation —
  PTQ can't fix a random head; it just rescales noise.

Fix (already applied to `tools/convert_yamnet.py`): after
`load_weights(...)`, open the h5 manually and
`model.layers[dense].set_weights([kernel, bias])` matched by shape.

Replay on the re-converted model:

| Metric | Broken (before) | Fixed (after) |
|---|---|---|
| Correlation with FP32 oracle | +0.25 | **+0.95** |
| Incident files `dequant_max` | 0.60–0.73 | **0.934** (saturated) |
| Bench files `dequant_max`    | 0.57–0.67 | **0.00–0.50** |
| Separation margin            | OVERLAP   | **+0.43 (safe)** |
| Threshold midpoint           | 0.63 unusable | **0.72 clean** |

## 7b. Secondary bug — double sigmoid in `yamnet.cc`

The INT8 model output is already a probability (softmax baked in via
Google's `classifier_activation='sigmoid'`). But firmware
`yamnet.cc:194` does `cry_conf = 1.0f / (1.0f + expf(-logit))` on the
dequantised output — treating the probability as a logit. This
compresses the [0, 0.934] probability range to [0.5, 0.718].

Impact:
- `cry_conf` as reported by `/metrics` / `events.log` is sigmoid-of-
  probability, not raw probability.
- For threshold comparison, cry_conf of 0.718 corresponds to
  dequant=0.934 (max achievable on an incident file).
- Not strictly wrong — just requires threshold pick in the compressed
  space.

Defer the fix (edit `yamnet.cc` to drop the second sigmoid) to a
separate PR; pick tonight's threshold in the current
`sigmoid(probability)` space.

## 7c. Tertiary bug — `CRY_DETECT_THRESHOLD` Kconfig is dead code

`main.c:361` hardcodes `detector_init(0.65f, ...)`. The `int40 ≈ 0.31`
Kconfig documentation describes a path that doesn't exist. Fix this
alongside the §7b cleanup — for tonight we simply edit the hardcode.

## 7d. Quaternary issue — audio-overrun tail under long soak

Observed during the bedroom soak on 2026-04-19 (build `ef60239-dirty`,
post-dense-fix, threshold 0.70, drain-to-1/4 applied):

- T+4 min:  `audio_overrun_bytes=0, audio_overrun_events=0`
- T+30 min: `audio_overrun_bytes=311552, audio_overrun_events=252`
  — ≈ 1.2 KB/s average drop, ≈ 0.05% of the 256 KB/s raw stream.

The drain-to-1/4 heuristic (`main.c:207`) eliminated overruns in the
60 s living-room soak but leaks under long run + noisier input (RMS
≈ 150 vs 47 living-room). Root cause likely periodic consumer stall
(SD flush, metrics burst, Wi-Fi TX) letting the producer fill past
the 32 KB capacity between drains.

**Decision:** *defer*. A patch-based detector tolerates occasional
~30 ms gaps (each 960 ms YAMNet patch is independent, and a real cry
lasts seconds); rushing a fix pre-overnight-2 risks reintroducing a
worse regression uncharacterised against real crib audio. Collect
overnight-2 data first (overrun rate vs RMS vs p95_inference_ms over
10+ hours) before picking an approach.

**Candidate fixes, cheapest first** (pick after data in hand):

1. **Ring buffer 32 → 64 KB.** One-line change in `audio_capture.c`,
   free in PSRAM. Absorbs consumer stalls without touching the drain
   heuristic. Least risky — start here unless data shows something
   stranger.
2. **Drain-to-/8** (from /4). Cheap, but pushes CPU spent draining
   and may fight with the watermark policy.
3. **Drop-oldest semantics with watermark log.** Change the stream
   buffer policy so producer overwrites oldest on full, and count
   the overwrite explicitly rather than losing it at the FreeRTOS
   layer. Cleanest observability story; biggest diff.

Track as a post-overnight-2 task; not in the critical path for
tonight's data collection.

## 7e. Manual-trigger UX — 40 s cooldown blocks rapid labeling

Reported during the 2026-04-19 bedroom soak. The web-UI capture card
(`www/index.html:128-187`) disables *every* `.rec-btn` for 40.5 s
after any successful trigger via `setBusy(true)` + `setTimeout(...,
40500)` (line 168). User perceives this as "Cry1 already exists" —
buttons greyed, label input looks stale, and on reload the user
forgets the last sequence number they typed.

Nothing is actually colliding: the WAV filename is already
timestamped (`cry-YYYYMMDDTHHMMSSZ.wav`, `event_recorder.c:75`) and
the `note=` query param is pure metadata logged to
`triggers.jsonl`. The backend only returns 409 when
`s_recording==true`, which is cleared the moment the recorder task
finishes writing — usually before the 40 s client cooldown ends.

**Workaround while overnight-2 runs:**

```
curl -sS -X POST "http://192.168.1.100/record/trigger?note=cry-$(date +%H%M)"
```

Bypasses the UI entirely, label is auto-timestamped, no cooldown
beyond the server's `s_recording` gate.

**Proper fix** (post-overnight-2, firmware reflash — `index.html`
is `EMBED_TXTFILES`):

1. Replace the blanket 40 s `setBusy` with a poll of
   `/record/status`. Re-enable as soon as `recording:false`. Only
   disable the button that's currently firing.
2. Pre-populate `#rec_note` with `cry-HHMM` on page load and on
   focus — eliminates the "I can't remember the last seq number"
   class of problem.
3. Replace the generic `⏳ Already recording — try again in a
   moment` string with a live countdown driven by
   `/record/status` (seconds remaining of the 40 s window).

Bundle with the §7d audio-overrun fix to amortise the reflash cost.

## 8. Deliverables

- This doc, committed.
- `projects/cry-detect-01/tools/replay_yamnet.py` (new).
- Patched `projects/cry-detect-01/tools/convert_yamnet.py` (done
  earlier — API drift fix, all-patches-per-wav).
- `projects/cry-detect-01/spiffs/yamnet-recalib.tflite` (done — 500
  real patches).
- Host report: `logs/ptq-replay-20260419.md` summarising the replay
  numbers and threshold pick.
- sdkconfig diff for the new threshold / consec_frames / keep_files.
- Overnight 2 capture directory `logs/night-20260419/` (tomorrow morning).
