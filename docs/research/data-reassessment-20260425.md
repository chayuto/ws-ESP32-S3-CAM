# Data re-assessment — what we have, what it's good for, what to do

**Date:** 2026-04-25
**Scope:** every capture cry-detect-01 has produced since project
inception (2026-04-18 → 2026-04-25, 8 calendar days, 4 extract
sessions, 3 firmware builds incl. today's mel-fix).
**Context:** companion to `data-vault-redesign-20260425.md` (which
describes the LABELING pipeline). This doc asks the question one
level up: *given the labeled data, what can we do with it?*

## 0. Headline

We have **318 unique captures** across 8 calendar days, **250
training-eligible** under the current ensemble. Enough to do
**three concrete things now**, plus one thing soon, plus one thing
to defer:

| action | usability | size | when |
|---|---|---|---|
| ~~Re-PTQ the on-device tflite with real-data calibration~~ | ❌ tested 2026-04-25, regresses on FPs (see §A) | — | revisit with mixed/INT16 strategy later |
| Train a small cry-vs-not-cry head on YAMNet embeddings | ✅ ready | 250 high-tier | this week |
| Calibrate firmware threshold from real on-device data | ⚠️ one-session sample | 27 captures from 04-24 | next week (need 2-3 more sessions) |
| Build a cry sub-type / urgency classifier | ⚠️ cluster IDs are heuristic, not validated | 27-50/cluster | defer until human-validated urgency labels exist |
| Public dataset release | ❌ too small, single-baby, single-env | — | when ≥ 1000 captures across ≥ 3 environments |

**Originally** we recommended re-PTQ as the highest-value single
action. We tried it 2026-04-25; **it regresses** (real cries lose
0.03–0.19 confidence; FPs jump from 0.0 to 0.79). See §A for the
diagnosis. The mel-fix already gave us the win we needed for the
on-device model.

The current single highest-value action is **(B) train a small
cry head on YAMNet embeddings** as a 3rd ensemble oracle. ~2 h of
work, 250 training-eligible captures available, doesn't touch
firmware.

## 1. Inventory (what we have)

### 1.1 Captures over time

| date | n | notes |
|---|---:|---|
| 2026-04-18 | 8 | bench / capacity tests |
| 2026-04-19 | 62 | first extended capture day, mostly bench |
| 2026-04-20 | 32 | bedroom bedtime + 1 am incident |
| 2026-04-21 | 72 | living-room daytime + bedroom overnight |
| 2026-04-22 | 19 | bedroom only |
| 2026-04-23 | 94 | mid-day to morning cluster |
| 2026-04-24 | 22 | first FULL post-mel-fix session; firmware `9a786780` |
| 2026-04-25 | 9 | post-midnight cry-cluster tail, partial extract |
| **Total** | **318** | unique by filename |

8 calendar days, **2 weeks** wall-clock. The data-program plan
§6.1 set 1000 cries / 4-6 weeks as the threshold for first real
training. We're at **125 confident cries / 2 weeks** — on track
to hit 250-300 cries by week 4, 500-1000 by month 2.

### 1.2 Time-of-day distribution

```
hour    n
 5h    21    morning wake cluster
 9h    18    daytime
10h    28    daytime
11h    22    daytime
13h    33    daytime peak (lunch / post-lunch fuss?)
14h    13
15h    12
17h    11
18h    28    pre-bedtime
19h    48    bedtime peak
20h    21    bedtime tail
night  ~27   sparse (00-04, baby asleep)
```

**Daytime-skewed.** Bedtime concentrates around 18-20h. The big
nighttime cry events (1 am incident on 04-21, 23:47-00:08 cluster
on 04-24/25) appear as small spikes that get washed out in this
overall histogram.

### 1.3 Environment

- **Bedroom:** dominant. ~80% of captures. Single placement
  (~1 m from cot, fixed outlet on 04-21 onwards).
- **Living-room:** brief 04-21 morning + a daytime fragment on 04-23.
  ~15% of captures.
- **Workbench:** bench tests on 04-18 + a few during reflashes. ~5%.

**Single-environment risk:** the model trained on this would learn
"cry vs bedroom-quiet + bedroom-speech-mostly". Generalization to
a different room or with kitchen / TV background is unproven.

### 1.4 Confidence-tier breakdown (post-redesign labels)

```
high_pos     125  ← oracle-confident cry (training positives)
high_neg     125  ← oracle-confident non-cry (training negatives)
medium_pos    36  ← cry-leaning, single oracle confident
medium_neg    21  ← not-cry-leaning
low           11  ← oracle disagreement (most interesting subset)
```

**78% of captures are training-eligible.** The 11 `low`-tier are
research-mode cases (next section).

### 1.5 Cry sub-type composition (high_pos only)

| cluster | n | f0_mean | character |
|---|---:|---:|---|
| full_cry | 48 | ~590 Hz | sustained moderate-pitch |
| fuss | 45 | ~340 Hz | low-pitch grumble, near-speech |
| distress | 27 | ~810 Hz | high-pitch broken |
| screech | 5 | ~1040 Hz | very high, jittery (NEW MODE per deep-analysis §Q3) |

**Fuss + full_cry dominate (74% of cries)** — the everyday cry
distribution. Distress (1 am wake, hunger-wake) is well-covered.
Screech is too small (n=5) to fit a sub-type classifier.

### 1.6 Negatives (high_neg) breakdown

```
speech-dominated   95   yam_speech_score >= 0.5 → caregiver / TV / other
other (silence)    30   yam_speech_score < 0.5 → ambient noise
```

**76% of negatives are caregiver speech.** Model trained on this
would over-learn "cry vs adult speech" rather than "cry vs general
noise". Synthetic-noise augmentation should compensate at training
time.

### 1.7 Low-tier disagreements (n=11) — the interesting subset

Three interpretable groups:

**Group A — speech masking** (8 of 11): yam_speech_score ≥ 0.99.
Caregiver speech overlaps with possibly-baby vocalization. YAMNet
heavily penalizes its baby_cry score; feat_clf sees voice-like
acoustic features (HNR 6-8) and votes cry. Examples: 18:12, 18:19,
12:42, 08:36, 09:15, 15:44, 09:26, 12:01.

**Group B — voice-like with no YAMNet category** (3 of 11): the
04-24 11:17 / 13:58 / 14:33 captures. Low yam_speech (0.0-0.3),
high HNR (8.9-9.4), feat_clf cry≈0.55. **YAMNet doesn't classify
them as anything strongly; feat_clf sees a voiced cry-shaped
signal.** These are the most worth listening to — they may be:
- under-represented baby vocalizations (cooing, babbling, whimper
  variants) that YAMNet undersees, OR
- non-baby voiced sounds (humming?) that the feat_clf overcalls.

**Group C — borderline YAMNet** (0 of 11 currently): would be a
yam_cry_score in 0.3-0.5 range with feat_clf disagreeing. Doesn't
appear here; YAMNet is fairly decisive on the 318 captures.

**Action:** listen to the 3 Group B captures + a sample of Group A
to characterize. If Group B contains real cry sub-modes YAMNet
misses, that's a critical finding for retraining strategy.

## 2. Usability matrix — what data is good for what

### A. Re-PTQ the on-device YAMNet — ❌ EMPIRICALLY REJECTED 2026-04-25

- **Original hypothesis:** real-data calibration of the INT8 PTQ
  would close the residual 3-4 pp gap from FP32 YAMNet
  (`deep-analysis-20260423.md` §Q1).
- **What we tried:** built `tools/repTQ_yamnet.py` to walk all 318
  captures and produce 954 calibration patches (peak / mid / low
  energy per WAV via YAMNet reference feature pipeline). Ran PTQ
  with these as `representative_dataset`. Wrote candidate to
  `/tmp/yamnet_v2_realdata.tflite`. **Did NOT overwrite the
  deployed `spiffs/yamnet.tflite`.**
- **Empirical outcome on 04-24 cry cluster + earlier confirmed
  FPs (offline test, FP32-mel input):**

| WAV | YAMNet GT | current peak | new peak | delta |
|---|---:|---:|---:|---:|
| 23:47 cry  | 0.996 | 0.934 | 0.832 | **−0.10** |
| 23:56 cry  | 0.976 | 0.934 | 0.848 | −0.09 |
| 23:58 cry  | 0.988 | 0.934 | 0.898 | −0.04 |
| 1 am 04-21 | 0.997 | 0.934 | 0.824 | −0.11 |
| bedtime 04-20 | 0.999 | 0.934 | 0.742 | **−0.19** |
| 1 am 04-19 | 0.999 | 0.934 | 0.848 | −0.09 |
| morning FP | 0.008 | 0.000 | **0.629** | **+0.63 (FP!)** |
| morning FP2 | 0.001 | 0.000 | **0.785** | **+0.78 (FP!)** |

  Real cries lost 0.03–0.19 peak confidence; confirmed FPs jumped
  from 0.0 → 0.6–0.79. The new model would alert constantly on
  adult speech.

- **Diagnosis:** real captures span a NARROW log-mel distribution
  (min −6.82, max +4.53, mean −3.17, std 1.53). PTQ packed int8
  levels tightly around that centre, losing dynamic range at the
  tails where cries vs FPs differ. The original synthetic
  calibration (Gaussian, std 3) covered a much wider distribution
  → quantizer had granularity to spare on the actual data range.
  Synthetic-broad beats real-narrow when test distribution sits
  inside calibration distribution.
- **Decision: keep the current `spiffs/yamnet.tflite`.** Revisit
  with one of:
  - mixed real + synthetic calibration (broaden the tails),
  - INT16 output type (more headroom on the cry-class side),
  - dedicated baby-cry model trained from scratch on this data,
    PTQ'd with its own representative dataset.
- **Lesson:** "real-data calibration" is intuitive but not always
  better. PTQ is sensitive to the BREADTH of the calibration
  distribution, not just its representativeness. Document this so
  we don't try the same thing again.

### B. Train a per-baby cry classifier head — ✅ READY NOW

- **Need:** ≥ 100 confident cries + ≥ 100 confident non-cries
  with embeddings.
- **Have:** 125 / 125 high-tier; YAMNet embeddings (1024-d per
  0.96 s) trivially extractable.
- **Status:** could train a 2-layer MLP today on 125+125 captures
  with 80/20 train/val. Expected to beat YAMNet alone for THIS
  baby's voice — same logic as the feature classifier already
  hitting 92 % accuracy.
- **Cost:** ~1-2 hours (script + train + evaluate).
- **Outcome:** a per-baby cry-conf model that COULD eventually
  replace the on-device YAMNet head. For now, just a strong
  third oracle in `ensemble_audit.py`.

### C. Calibrate firmware detector threshold — ⚠️ ONE-SESSION SAMPLE

- **Need:** captures with on-device cry_conf timeline + YAMNet
  ground truth, under stable firmware.
- **Have:** 04-24 only (build `9a786780`, 27 captures with infer
  log coverage). Pre-fix sessions had broken cry_conf so they're
  irrelevant.
- **Recommendation:** based on 04-24 alone, lower `base_threshold`
  0.70 → 0.50 (per `night-session-20260424.md` §4). This catches
  88% of cries at 0% FP. **Don't** ship this until 2-3 more
  post-fix sessions confirm.
- **Cost:** 1-line firmware change, but timing-sensitive. Wait
  for next week's data.

### D. Cry sub-type / urgency classifier — ⚠️ HEURISTIC LABELS

- **Need:** per-capture urgency label (fuss / cry / distress /
  emergency) ideally validated by caregiver.
- **Have:** k=4 KMeans cluster IDs as a proxy. Cluster names are
  heuristic (assigned by f0 ordering); we have no causal ground
  truth that cluster=distress means "actually distressed".
- **Recommendation:** **defer**. Without validated labels, training
  on cluster IDs builds a model of acoustic similarity, not
  urgency. Misleading downstream.
- **What unblocks it:** either a few sessions where the caregiver
  notes the cry context (hungry / pain / tired), OR a published
  cry-corpus that maps acoustic clusters to known urgency.

### E. Public-release dataset — ❌ DEFER

- **Need:** ≥ 1000 captures, multi-baby ideally, multi-environment.
- **Have:** 318, single baby, ~80% bedroom.
- **Defer until:** month 2-3 minimum. Earlier release is too
  sample-biased to be useful to anyone else.

### F. Acoustic-baseline mapping (per environment) — ⚠️ PARTIAL

- **Need:** continuous heartbeat data across 24 h cycles in each
  environment.
- **Have:** bedroom heartbeat data is dense (04-21, 04-22, 04-24
  full overnight). Living-room has a partial day. Daytime
  bedroom is OK. Outside is zero.
- **Recommendation:** when collecting more data, deliberately
  spend a daytime session in a noisy room (kitchen, living-room
  with TV) to populate the FP-environment side.

### G. Re-evaluate firmware regressions — ✅ ALWAYS USABLE

- **Need:** WAVs (immutable, our existing 200 + future captures).
- **Have:** 200 WAVs.
- **Use:** any future tflite or firmware change can be regression-
  tested by feeding our captured WAVs through it offline. The
  tooling already exists (`/tmp/diag_mel_drift.py`, etc.).

## 3. What's missing (gaps)

In rough order of impact on training quality:

1. **Multi-environment data.** Bedroom is overrepresented at ~80%.
   Need a deliberate session in living-room with caregiver
   activity, kitchen during cooking, perhaps outdoor.

2. **Daytime non-cry distribution.** We have a lot of "loud
   transient → captured by auto-RMS" daytime FPs that turn out to
   be non-cry, but the breakdown is mostly speech (95 of 125
   negatives). Want more household-noise negatives (water running,
   appliance, distant traffic).

3. **Babbling / cooing / laughter examples.** YAMNet's `babbling`
   and `baby_laughter` classes exist but our captures barely
   surface those (most baby vocalizations during the day are
   cry-adjacent or quiet). For a robust per-baby model, we want
   the not-cry-but-baby-vocalization category populated.

4. **Pain vs hunger vs fuss labels.** Currently we have acoustic
   sub-type clusters but no causal context. A simple caregiver
   web form (`/label?ctx=feed|change|wake|fuss`) timestamped to
   captures would resolve this. Plan §4.1 already specs this; not
   yet built.

5. **Cross-baby data.** Single-baby data risks overfit. If a
   second household ever joins, that single change improves model
   generalization more than 10× the current data volume would.

6. **Holdout sessions.** Currently the auditor splits 80/20 on
   filename hash. Better: hold out entire SESSIONS (e.g., one in
   five) so test scores reflect generalization to new nights, not
   just new captures.

## 4. Concrete recommendations (ranked)

Ordered by expected value × ease.

### Now (this week)

1. **Re-PTQ the tflite + reflash.** Highest impact, cheapest. See §A.
   - One host command: `hf/convert_yamnet.py --audio-dir <high-tier WAVs>`.
   - Reflash only the spiffs partition.
   - Validate on the 04-24 cluster: peak `cry_conf` should stay
     ≥ 0.93 with potentially smoother distribution.

2. **Train v0.1 cry head on YAMNet embeddings + commit it as a
   3rd oracle.** See §B.
   - Add to `ensemble_audit.py` as `embed_clf_prob`.
   - Doesn't deploy on device yet; it's a host-side label
     refinement.
   - With three independent oracles, low-tier
     count should drop further.

3. **Listen to + characterize the 3 Group B disagreement
   captures** (04-24 11:17, 13:58, 14:33). Decide: novel cry mode
   YAMNet misses, or feat_clf false alarm? Either way, document
   in a 1-page note.

### Next (next 1-2 weeks of capture)

4. **Hold out by session, not by row.** Update `freeze_release.py`
   so test set = whole sessions. Adjust v0.1 release accordingly.

5. **Run a deliberate kitchen / living-room daytime session.**
   Diversify negatives.

6. **Lower firmware `base_threshold` 0.70 → 0.50.** After 2-3 more
   post-fix sessions confirm the 88% TPR / 0% FPR result holds.

### Later (month 2)

7. **Caregiver-context tagging.** Plan §4.1 spec: simple web form
   on `http://device/label?ctx=...`. Adds urgency labels for free.

8. **Sub-type classifier v0.1** (only after caregiver-context
   data exists).

9. **First public-release-grade dataset.** When ≥ 1000 captures
   across ≥ 3 environments.

## 5. The data isn't going stale

A common worry is that early-week captures become useless as the
model evolves. Here's why our data ages well:

- **WAVs are immutable.** Every future model can be evaluated
  against them.
- **YAMNet ground truth is firmware-independent.** Captures from
  pre-mel-fix sessions still have valid YAMNet labels.
- **The ensemble re-audit on schema/oracle changes** propagates
  any improvement back to old captures.
- **What ages:** firmware-side `cry_conf` per session. We've
  flagged that with `build_sha` provenance; analyses filter or
  un-transform appropriately.

## 6. Summary

We have **318 captures, 250 training-eligible, 8 days of bedroom-
dominant audio**. That's enough for:

- A meaningful re-PTQ of the on-device model (do this now).
- A v0.1 per-baby head trained on YAMNet embeddings (do this now).
- Confidence-aware label vectors that downstream training can
  weight (already wired into `cry-v0.1-ensemble.json`).

It's NOT enough for:

- A cry sub-type / urgency classifier (need validated labels).
- Public release (need scale + diversity).
- Solid threshold calibration on the current firmware (need 2-3
  more post-fix sessions).

The single biggest unblocker isn't more data — it's **re-PTQ** to
let the model we already have actually use the calibration we now
have. Everything else is incremental.
