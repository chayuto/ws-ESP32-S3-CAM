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

## 7f. Missed cry — 2026-04-19 ~15:48 AEST (bedroom soak)

User report: "she cried and woke up" during the bedroom soak. Device
never fired an alert and never auto-triggered a WAV — confirmed miss.

**Evidence from `/sdcard/cry-20260419.log` (30 s snapshots):**

| Time (AEST)   | RMS    | Floor p95 | Notes                             |
|---------------|--------|-----------|-----------------------------------|
| 14:02 – 15:47 | 80–115 | 420 → 184 | flat nursery baseline (90 min)    |
| **15:48:32**  | **531.6** | 184    | **5.5× baseline, 2.9× floor**     |
| 15:49:02+     | 90     | 184       | immediate return to baseline      |
| 16:00:32      | 128    | 184       | `speech=0.622` — caregiver enters |

`cry_conf_1s_max` stayed ≤ 0.501 across the entire window; no cry
class ever budged off the INT8 quantisation floor.

**Why every layer missed it:**

1. **Auto-rms trigger (`auto_trigger.c`): 5× floor_p95 threshold.**
   The 15:48 spike (531 / 184 = 2.9×) came in *below* the threshold
   because the floor was still elevated from the 14:00 noise
   cluster. Bedroom floor decays slowly — by 15:48 it had only
   dropped to 184 (from a peak of ~1770 at 13:49). A pure ratio
   check is blind to a brief cry riding on a still-hot floor.

2. **Classifier: `cry_conf_1s_max=0.475` at 15:48.** Either the cry
   was < 1 s of actual vocalisation (below the ≥ 960 ms YAMNet
   patch needed to matter), or the INT8 model genuinely doesn't
   rank her cry as `cry_baby`. Can't disambiguate without the
   audio, which we don't have because auto-trigger didn't fire.

3. **30 s snapshot cadence blurs the envelope.** A one-snapshot
   spike means the event lasted somewhere between one sample and
   30 s — we can't narrow it further from the log alone.

**Implications for auto-trigger redesign:**

- Add an **absolute RMS** arm alongside the 5× floor-ratio arm.
  Trigger on `rms > max(5×floor_p95, ABS_FLOOR)` with
  `ABS_FLOOR ≈ 400` (above the 15:48 spike threshold but well
  above the quiet-bedroom 80–115 baseline).
- Consider **short-burst protection**: if rms crosses 5×floor for
  even a single 32 ms audio frame, arm a ~1.5 s recording window
  so we don't rely on sustained loudness.
- Lower the snapshot cadence (30 s → 5 s) during the noise-floor
  warm period OR log peak-since-last-snapshot so a sub-second
  spike still shows up in the JSONL.

**Implications for classifier training (the actual epic):**

This is **Stage 2 ground truth**: a confirmed real cry that our
current model missed silently. Even without a WAV of the 15:48
event itself, the miss tells us:

- Real bedroom crying can be brief (< 1 s vocalisation bursts),
  which the YAMNet 960 ms patch and 1 s conf-max window may
  straddle awkwardly.
- We need collection gear that is *more eager* than the
  classifier: capture on any anomalous RMS, even sub-threshold,
  so Stage 2 labelling has candidates to review.

**Decision:** defer the fix to post-overnight-2, bundle with §7d
and §7e. For tonight's overnight-2 collection, drop the device's
auto-trigger ratio from 5× to 3× floor_p95 *if the infer log
shows the floor settling < 60* — otherwise leave it alone and
collect the miss data.

## 7g. Regression introduced by 9ceec4a — drain loop corrupts mel state

Commit `9ceec4a` ("Fix YAMNet dense-layer silent-skip") repaired the
model weights but introduced a runtime regression. Host replay of
the shipped INT8 `yamnet.tflite` against captured WAVs gave
`cry_baby = 0.718` on real cry audio, but the on-device pipeline
output `cry_conf = 0.500` (= `sigmoid(0)`, i.e. raw INT8 == zero
point — model saw pathological input).

**Evidence (host replay vs device, same WAV, same tflite):**

| WAV (label)          | RMS  | Device `cry_conf` | Replay `cry_baby` | Peak class (replay) |
|----------------------|------|-------------------|-------------------|---------------------|
| Cry3 (morning, 05:04)| 66   | 0.624             | 0.718             | `Baby cry, infant cry` |
| Cry5 (morning, 05:07)| 106  | 0.618             | 0.718             | `Crying, sobbing` |
| Cry6 (afternoon, 13:47)| 61 | **0.500**         | 0.718             | `Baby cry, infant cry` |
| auto-rms-22x (13:54) | 2143 | **0.501**         | 0.718             | `Baby cry, infant cry` |

Morning captures pre-date 9ceec4a; their 0.6+ was random-init
classifier noise (per §7a). Afternoon captures post-date 9ceec4a;
their 0.500 is a *new* failure mode — fixed model + broken runtime.

**Root cause:** the pre-yamnet drain loop at `main.c:212-220` was
`while(ring > 1/4 full) { read hop; mel_features_push(hop); }`. The
drain fires ~12 hops back-to-back in < 1 ms before `take_patch`. The
STFT/windowing state inside `mel_features_push` is not designed for
back-to-back calls without real producer spacing — rapid-fire pushes
leave the window buffer in a state that produces a near-uniform mel
patch, which quantises to raw INT8 min across all 521 classes →
dequant 0 → sigmoid 0.500.

**Fix (tonight):** remove `mel_features_push(pcm, dn);` from the
drain loop. Drain still vacates the ring (no overrun regression),
but drained audio is discarded. The outer `while(1)` iteration
reads a fresh hop → pushes to mel with normal producer spacing →
`take_patch` returns a valid patch.

Trade-off: patch content lags the acoustic event by one `yamnet_run`
(~650 ms). Crying is continuous; acceptable for now. A cleaner
redesign (drain, reset mel state, read one fresh hop, take patch)
is a follow-up.

**Validation status:** shipped 20:00 AEST pre-overnight-2. Quiet-room
poll shows `overrun=0`, `fps=1.47`, `cry_conf=0.500` (correct for
silence). Cry-audio verification blocked on real crying or physical
playback near the mic. Go/no-go decision for overnight-2: if the
first real cry produces `cry_conf > 0.5`, the fix is confirmed;
otherwise we have a second regression.

## 7h. Debugging infrastructure gap — boot-time model self-test

The §7g regression survived a full 4 h bedroom soak because we had
no way to tell, after each flash, whether the model was actually
producing expected output. Symptoms (`cry_conf` pinned at 0.500)
were indistinguishable from "quiet room, no cry" until we did a
host-side replay against recorded WAVs — which required six hours
of investigation and pulling 74 MB of audio off SD.

**Proposed: on-device boot-time self-test.**

- Bake one known cry WAV (~1 s) and one known speech WAV into the
  firmware binary (SPIFFS or embedded blob, ~32 KB each at 16 kHz).
- On boot, after `yamnet_init()`, feed each WAV's log-mel patches
  through the live inference path and record `cry_conf_max` over
  each clip.
- Log a structured line:
  `model-sanity: cry_wav=0.71 speech_wav=0.52 margin=+0.19 PASS`
  or
  `model-sanity: cry_wav=0.50 speech_wav=0.50 margin=+0.00 FAIL`
- Expose via `/metrics` so deployment scripts can check before
  leaving a fresh flash in the bedroom.
- Optional: flash a red LED pattern on FAIL.

**Why this is high-leverage:**

- Catches any regression (model weights, input features, output
  read, quantisation, drain-loop-style state corruption) within 5
  seconds of boot, no soak required.
- Requires no cry audio at deployment time — the test cry is
  embedded.
- Trivially automatable in CI: after build, flash, wait for boot,
  curl `/metrics`, assert `model_sanity_pass == true`.

**Cost estimate:** one afternoon. No new dependencies.

## 7i. Debugging infrastructure gap — mel pipeline observability

When §7g broke the mel state, the only downstream symptom was
`cry_conf = 0.500`. There was no direct signal from the mel layer
itself. Log surface for the entire log-mel pipeline today is zero.

**Proposed: per-patch mel stats in the snapshot CSV + `/metrics`.**

Add to `cry-YYYYMMDD.log` snapshot rows:

- `mel_min`, `mel_max`, `mel_mean` — range of the 96×64 patch
  (expected: wide range on real audio, ≈ 0 on corrupted state).
- `mel_zero_frac` — fraction of mel cells at input zero point
  (flag > 0.9 = flat patch, broken input).
- `mel_frames_pushed` — count since last `take_patch`
  (normal ≈ 3–5; a value > 50 means drain-burst or similar).
- Drain accounting: `drain_hops_avg` per inference cycle.

In `/metrics` JSON:

- `mel_patch_variance` (rolling last-10 patches).
- `mel_pathological_count` — patches flagged as flat.

**Why this matters:**

- A flat mel patch (mean ≈ max ≈ min) is a smoking gun for
  pipeline breakage. With these fields in the snapshot log, §7g
  would have been spot-diagnosable from the first minute of the
  bedroom soak.
- `drain_hops_avg` lets us see whether the drain is doing zero,
  occasional, or constant work — a canary for any future
  ring/consumer imbalance.
- `mel_zero_frac` is a cheap proxy for "is the mic producing
  signal at all?" at the input side of the model.

**Cost estimate:** ~60 LOC in `mel_features.c` + schema bump in
`sd_logger.c`. No memory impact (stats computed inline on the
patch already in cache).

## 7j. Other debugging gaps (ranked, deferred)

Not urgent enough for tonight but worth planning:

1. **Host-side golden-WAV CI gate.** `replay_yamnet.py` already
   exists. Wrap it in a shell test that runs on every firmware-
   adjacent commit: replays 5 golden WAVs (3 cry, 2 speech),
   asserts cry_conf > 0.5 on cry and < 0.5 on speech. Would have
   blocked `9ceec4a` at commit time.
2. **Denser snapshot cadence or peak aggregates.** 30 s aliases
   sub-minute events (see §7f). Either 5 s cadence *or* add
   `rms_peak_30s`, `cry_conf_peak_30s`, `mel_energy_peak_30s`
   columns so a burst still lands in the log.
3. **`/debug/replay?wav=X` endpoint.** Run the on-device model
   against a stored `EVENTS/*.wav` and return JSON with per-patch
   `cry_conf`. A/B any firmware change against a fixed audio
   reference in 1 second.
4. **Live spectrogram in the web UI.** A 5 s rolling mel heatmap
   makes "mic sees something but model is idle" obvious at a
   glance. Also makes the §7i flat-patch case visible.
5. **`triggers.jsonl` audibility ground truth.** Cry1 afternoon
   was user-labelled "Cry1" but the replay showed no cry-class
   activation — likely the baby wasn't audibly crying at the
   moment of the tap. Add a post-capture "confirmed audible?"
   tag (parent review), keep that subset as the gold set. Defer
   until Stage 2 collection is stable.

## 7k. Post-flash living-room soak — measurement context 2026-04-19 20:00 AEST onward

Context for any future reader interpreting today's post-20:00 on-device
logs: the device was **not in the bedroom** during validation of the
§7g drain fix. It was on a living-room surface with the mother/father
present, TV/conversation continuous, and at least one cough event.
This is the activity backdrop for every snapshot and metric captured
from build `a895bfdc` (flashed 2026-04-19 19:50 AEST) until it is
physically moved back to the bedroom.

**What the room sounds like (per on-device classifier, 50 min window
20:00–20:50):**

| Class     | Hits (≥0.60) | Peak  | Character                              |
|-----------|--------------|-------|----------------------------------------|
| speech    | ~37          | 0.730 | Continuous TV/conversation throughout  |
| cough     | 3            | 0.718 | Single loud cough @ 20:49 + 2 tail     |
| cry_baby  | 1 (brief)    | 0.591 | Sub-snapshot hit @ 20:50:50, post-cough |

RMS 20–729; floor_p95 settled to 515 after ~15 min warm-up; no
auto-trigger fires (5× floor arm requires rms > 2575 which nothing
reached); zero alerts (threshold 0.70, peak cry was 0.591).

**Why this matters for calibration:**

- The §7g fix is validated *in a noisy living-room environment*, not
  in isolation-booth conditions. Speech class saturating at 0.730 and
  coexisting cleanly with zero cry_baby activations across 37 speech
  snapshots confirms discrimination, not just pipeline health.
- The bedroom-soak baseline (§7f) was 80–115 RMS on a flat floor.
  Living-room baseline is 20–729 with near-continuous speech. Floor
  tracking, auto-trigger ratios, and any threshold choices based on
  bedroom numbers will behave differently in this environment.
- The single cry_baby=0.591 hit at 20:50:50 has only 0.018 headroom
  below the saturation ceiling (0.718). A real cry needs to push to
  that ceiling to clear the 0.70 alert threshold. This is tight.
  Revisit threshold after the first unambiguous cry capture.

**Before overnight-2:** device must be moved to the bedroom. When
moved, record the timestamp here so future log readers can split
pre/post-move data cleanly.

**Move to bedroom: 2026-04-19 ~21:56 AEST.** Post-move signature
(first poll at 21:56:12):

- rms=92, floor_p95=184 (matches bedroom soak baseline from earlier
  today before the §7g reflash — validates the mic/floor behaviour
  is consistent with the bedroom environment)
- overrun=1 (vs. 381 accumulated in 2 h living-room soak — bedroom
  has much less sustained audio pressure on the ring)
- Watched classes all at INT8 noise floor (0.500/0.517) — correct
  for a quiet empty-bedroom moment
- fps=1.46, heap/psram flat, state=idle

Overnight-2 ground-truth capture starts from here. Any activation
≥ 0.60 or auto-trigger fire after 21:56 is bedroom-context data.

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
