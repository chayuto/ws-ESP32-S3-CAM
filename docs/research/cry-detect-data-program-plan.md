# Cry-detect-01 data program plan

**Written:** 2026-04-21
**Status:** draft — informs tonight's deployment + forward roadmap
**Supersedes:** the "implications for next iteration" section of `night-session-20260420.md` for long-term direction

## 0. Goal reframe

Prior framing: "make the on-device cry detector useful as a baby-monitor alerting signal".

New framing: **"collect a labeled per-baby cry dataset and use it to train a model that actually works for this household"**. The shipping product is the *data flywheel*, not the current firmware.

Consequences of the reframe:

- The current firmware becomes **scaffolding**, not product. It does not need to alert well, only to capture well.
- **YAMNet is the labeler, not the detector**. The on-device model's output is ignored for production decisions; YAMNet (or a successor) runs host-side during audit.
- **FPs are training data**, not noise. The 22 kitchen captures from 04-21 morning are 22 labeled negative examples — destroying them discards training signal.
- **Data loss is the only unrecoverable failure.** Every other problem is recoverable with more data; a lost night is lost forever. This is the single strongest constraint on tonight's deployment choices.

## 1. Tier 1 — data-safety guarantees

These are preconditions. Nothing else in the plan matters if data is being lost silently.

### 1.1 Power resilience
- **Problem:** 2026-04-21 01:32 → 09:08 outage, device on wall charger, no boot record, no diagnostic breadcrumb. Most likely cause: adapter / outlet / household micro-outage — not diagnosable from device data.
- **Target:** zero-loss tolerance through at least a 30 min interruption. The overnight session is ~12 h — a single 30 min gap drops ~4 % of the night's data, which is tolerable; a 7 h gap is not.
- **Options (in order of effort):**
  1. Move to a known-healthy wall circuit (different room if needed), verify under load with a plug-in power logger for 24 h before trusting.
  2. USB battery hat in series (power-bank with passthrough) — 10 000 mAh covers a full night standalone if the wall cuts out.
  3. Small UPS upstream of the adapter — overkill for this device but future-proofs other hardware.
- **Decision for tonight:** do (1) minimally + start a power-log review, defer (2) / (3) until we can source hardware.

### 1.2 Monitor heartbeat-gap alarm
- **Problem:** last night's `cry-monitor.sh` logged `NORESP` per-poll but didn't escalate, and its loop expired before the outage was clearly visible. We needed to know within minutes, not notice in the morning.
- **Design:** extend the existing `cry-monitor.sh` (lives at `/tmp/cry-monitor.sh`, ephemeral — but reconstructed per session) to:
  - Track **consecutive** NORESP count → fire `OUTAGE_CONFIRMED` at 3 in a row (180 s silent).
  - Track `inference_count` delta → fire `INFERENCE_STALLED` if /metrics responds but counter not growing for 90 s (hang detection).
  - Track `uptime_s` reset → already present, keep.
  - Extend loop to 16 h (960 iter × 60 s) so it survives a full overnight + morning cycle.
- **Consumed by:** `Monitor` tool watching `/tmp/cry-monitor.anomalies` (per project memory note: producer-in-bg + Monitor-on-anomaly-file is the canonical pattern).

### 1.3 Extract-session idempotency
- **Problem:** `extract_session.sh` skipped `triggers.jsonl` if already present locally, missing appended entries between extracts. Identified + patched during the 04-20 session.
- **Status:** patch is staged uncommitted; commit tonight so tomorrow's extract is reproducible.

## 2. Tier 2 — annotation workflow

For the dataset to be useful, every WAV needs a label that's stable across time.

### 2.1 Label schema (v1)

Per-WAV labels should be:

| field | values | who |
|---|---|---|
| `yam_label` | `baby_cry`, `crying_sobbing`, `speech`, `other`, `silence`, `multi` | auto (YAMNet ≥ 0.5) |
| `yam_conf` | 0.0 – 1.0 | auto |
| `human_label` | `cry`, `fuss`, `distress`, `not_cry`, `borderline`, `unsure` | human review |
| `environment` | `bedroom`, `living-room`, `car`, `outdoor`, `other` | auto (session metadata) |
| `context` | free-text — "pre-feed", "wake-up", "post-bath", etc. | human, optional |
| `audit_ts` | ISO timestamp | auto at audit time |

`yam_label` and `human_label` disagreements are the interesting set — those become the training-refinement candidates.

### 2.2 Bulk auto-label pass
- Input: `yamnet_files.csv` per session.
- Rule: if max `baby_cry` or `crying_sobbing` ≥ 0.5 → `yam_label = baby_cry`; if `speech` ≥ 0.5 → `speech`; etc.
- Output: `labels.csv` per session, one row per WAV, everything resolved except `human_label`.
- Cost: near-zero, already part of audit pipeline.

### 2.3 Human audit tool (to build)
- Web UI or terminal: show spectrogram PNG + 5 s audio clip + current auto-label, take a single keypress (`c` cry, `f` fuss, `d` distress, `n` not-cry, `b` borderline, `?` skip).
- Stores to `labels.csv` directly, resumable.
- Goal: ~5 s per WAV review ⇒ 10 min/night human time for a 100-WAV session.
- Not blocking for tonight; build before the 3rd session.

### 2.4 Borderline review cadence
- Weekly: re-review the "borderline" and "disagreement" sets. These are where model improvement comes from.

## 3. Tier 3 — retention and rotation

### 3.1 Firmware capacity
- Current cap: `CRY_REC_KEEP_FILES = 5000`, ~30 MB free-space guard, ring-delete oldest.
- At 100 captures/night: cap hit in 50 nights.
- At 500 captures/night (noisier environment): cap hit in 10 nights.

### 3.2 Host-side retention policy
- **Nightly extract** already mirrors everything to host. Good.
- **Weekly harvest:** after audit + commit of labels.csv, move WAVs to a host archive directory (gzipped tar per session, per-month bucket) and purge the on-device SD card via a `/files/rm?path=...` endpoint. Keeps the device at a healthy <1000 files, reduces mount-recovery time on reboot.
- **Monthly backup:** rsync the host archive to cold storage (external SSD / cloud). A single corrupted SD is not allowed to lose a month of data.

### 3.3 Dataset versioning
- `datasets/cry-detect-01/v0.1/` at the repo root (or a dataset-specific repo if size warrants it, LFS or DVC).
- Each release is a snapshot of labeled WAVs + labels.csv + manifest.csv at a specific audit date.
- Model training runs reference a dataset version string for reproducibility.

## 4. Tier 4 — metadata enrichment

Currently per-capture we have: timestamp, trigger_note (`auto-rms-Nx-rmsN`), trigger_rms, and that's it. For training usefulness, enrich with:

### 4.1 Caregiver context (phone-side)
- Simple web form on `http://<device>/label` or a companion PWA:
  - Tap "Feed", "Change", "Wake", "Pat", "Sleep-down" — timestamps a context event.
  - Context events are merged during audit to tag nearby WAVs.
- Not needed tonight, but starts to matter at ~5 sessions in.

### 4.2 Environment metadata
- Per-session: `environment=bedroom|living-room|other`, `placement_distance_m`, `ambient_noise_source=appliance|street|quiet`, `baby_age_months`, `build_sha`.
- Store in the session's `README.md` as frontmatter → pulled into joins during audit.
- Start tonight — fill in manually when creating the session dir.

### 4.3 On-device signal enrichment (future firmware)
- Per capture, append to `triggers.jsonl`: pre-trigger noise-floor history (last 5 min RMS p50/p95 trend), state of `cry_conf` over the previous 30 s (did it rise gradually → likely real, or spike from silence → likely impulsive FP).
- Not urgent.

## 5. Tier 5 — environment variety

### 5.1 Why it matters
One mic in one bedroom = one acoustic channel. A model trained on that will fail on: different rooms, different caregivers, different baby stages, siblings, daytime household noise overlap.

### 5.2 Current state (after this session)
- 129 WAVs from bedroom + living-room, single baby, single night primary + context days.
- Sufficient for a toy classifier, insufficient for a general model.

### 5.3 Planned variety
- **Placement variance:** same room, ≥3 mic positions (near cot, far wall, door gap) across sessions. Expose the model to reverb/intensity shifts.
- **Time-of-day variance:** currently all night-biased. A daytime session in the nursery while baby naps adds the "cry against household noise" case.
- **Seasonal/household variance:** fan on/off, window open/closed, AC on/off. Controlled via session notes.
- **Cross-baby (longer term):** if scope expands, second household data is the biggest single-lever improvement.

## 6. Tier 6 — model training roadmap

### 6.1 Baseline decision (when?)
- When we have **≥1000 labeled cries from ≥10 sessions across ≥3 environment configs**, do the first training run.
- Estimated timeline: 4–6 weeks of nightly collection at current rate.

### 6.2 First model architecture
- Start with the simplest thing that could work: **fine-tune YAMNet's last layer on our 4 watched classes**. Freeze the trunk, replace the 521-class head with a 4-class head (`cry`, `fuss`, `distress`, `other`), train on our labeled data.
- YAMNet embeddings (1024-d, emitted per 0.48 s frame) are a known-good feature basis for infant vocalization.
- Cost: minutes to train on CPU. Deployability: same tflite pipeline as today.

### 6.3 Held-out splits
- **Test-holdout sessions:** never audit-tune on these. Pick every 5th session as test-only.
- **Per-baby split:** if we ever get multi-baby data, split by baby not by clip — the harder real-world generalization test.

### 6.4 On-device deployment
- If the fine-tuned head is INT8-PTQ'd with real-data calibration, the saturation issue from §8 of `night-session-20260420.md` will not recur.
- Keep the YAMNet oracle path alive even after deploying a custom model — it stays the audit ground truth.

## 7. Decision log — what we're *not* doing tonight and why

| deferred | reason |
|---|---|
| Re-PTQ the tflite with real calibration | The on-device `max_cry_1s` doesn't drive any decision in a data-collection deployment. Defer until we actually want alerting. |
| HNR cull in `event_recorder.c` | FPs are labeled negative training data, not noise. Culling discards signal. Recompute HNR post-hoc during audit, use as a *review-order* hint, not a capture gate. |
| Log-rotation firmware change | 105 MB/day infer-*.jsonl is still fine on a 64 GB SD. Revisit at ~5000 files or ~1 GB logs. |
| New alert-actuation pathway (push/SMS) | Distinct product track. The data collection works without it. |
| Trigger-threshold retune | The current 5x–7x sustain fires reliably in the bedroom; lowering it to catch quieter events is tempting but raises FP rate in other environments. Don't touch until we have cross-environment data. |

## 8. Tonight's deployment plan — concrete actions

- [x] **Written** — this plan. Future sessions reference it.
- [ ] **Hardware** — move device to bedroom, plug into known-good wall outlet, note outlet/room choice in session README.
- [ ] **Host** — `cry-monitor.sh v2` with consecutive-NORESP escalation + inference-stall detection, running in background with Monitor attached to `/tmp/cry-monitor.anomalies`.
- [ ] **Repo** — commit the `extract_session.sh` triggers-refresh patch (deferred from 04-20).
- [ ] **Session scaffold** — `mkdir logs/night-20260421`, write `README.md` with environment metadata pre-filled, ready for morning extract.

Everything else is deferred per §7.

## 9. Session README template (for every future session)

```markdown
# night-YYYYMMDD

**Start:** YYYY-MM-DD HH:MM +TZ
**End:** YYYY-MM-DD HH:MM +TZ
**Environment:** bedroom | living-room | other
**Mic placement:** ~Xm from cot, <description>
**Caregiver:** <name>
**Baby age (months):** X
**Ambient noise sources:** fan=on|off, AC=on|off, window=open|closed, other=<...>
**Firmware build_sha:** <...> (single build across session or note transitions)
**Device location move during session?:** no | yes (timestamp + reason)

## Dataset split
- [ ] train
- [ ] val
- [x] test-holdout  <!-- pick ~20% of sessions, commit the decision before audit -->

## Incident notes
<free text>

## Post-audit summary
Filled in after audit_pipeline.sh + human review.
- Total WAVs: N
- Labeled cry (human): N
- Labeled fuss: N
- Labeled not-cry: N
- Disagreements (yam vs human): N
```

## 10. Success criteria for the data program

- **3 months in:** 50+ sessions, 2000+ labeled cries, model v0.1 trained and ≥80 % agreement with YAMNet on a held-out set.
- **6 months in:** model v0.2 deployed on-device, driving an alert path, recall ≥90 %, FP rate ≤1 per night.
- **Any point:** zero silent data-loss incidents (failures are OK, but we must *know* when they happen).
