# Stage 2 plan — cry-detect-01 next increment

*Written 2026-04-17, device deployed in real-world test.*

Stage 1 shipped an end-to-end pretrained pipeline (YAMNet INT8 mel-patch → detector → LED → SD CSV + WAV → SSE dashboard + live PCM stream + mDNS). Stable under observation. Known stop-gaps:

1. Model calibrated on synthetic noise → `cry_conf` biased ~0.6 in quiet room.
2. Only 1 of 2 mics used; no stereo / beamforming / AEC.
3. No real-world FP / TP data yet.
4. `/log/tail` HTTP handler stack bug (cosmetic).

Goal of Stage 2: **prove what the hardware is actually doing, then exploit it.** Measurement before optimisation.

---

## Milestone 2.0 — hardware & signal validation (day-one)

Nothing else happens until we know these.

### 2.0.1 Which microphones are alive?

**Question:** is MIC1 capturing? MIC2? Both? Something else entirely?

**Experiment:**
1. Keep mono capture (1 ch) as-is. Open `http://cry-detect-01.local/` → tap 🔊 Listen.
2. Tap with a finger exactly over MIC1 hole → observe `input_rms` spike in dashboard.
3. Repeat with MIC2. Compare.
4. **Expected**: one mic gives a sharp RMS rise, the other doesn't (because BSP is routing only one into the mono slot).
5. Record 5-second WAV via the event recorder's pre-roll (trigger manually via new `/test/record` endpoint — see 2.0.3).

**Success criterion:** we can say "the mic that's wired to ES7210 slot 0 is MICn" with evidence.

### 2.0.2 Switch to 4-channel capture (ES7210 native)

ES7210 is a 4-channel TDM ADC. `bsp_audio_codec_microphone_init()` with `channel=2` or `channel=4` pulls stereo or all four slots. The BSP support layer accepts this. Required changes:

- `audio_capture.c`: `fs.channel = 2` (stereo, MIC1 + MIC2; skip the 2 aux slots).
- Downmix policy: arithmetic mean of the two int16 samples after DC-removal. Monotonic, phase-stable, simplest possible "beamforming".
- Keep the mel pipeline mono — YAMNet expects `[96, 64, 1]`.

**Experiment:** once 2-ch capture is live, test with tap-on-MIC1 and tap-on-MIC2 individually. Both should produce RMS movement.

**Success criterion:** both mics measurably contribute to `input_rms`; device still hits 1.35 fps inference.

**Cost:** ~1 day including a stability test.

### 2.0.3 Test-triggered event recorder

Add `POST /test/record?seconds=N` endpoint that forces a `event_recorder_trigger()`. Exposed for bench testing only (no auth yet; Wi-Fi is assumed trusted).

Lets us record a WAV on demand, download it from `/recordings/<name>.wav`, open in Audacity, look at the spectrogram. Sanity on frequency response, DC offset, clipping, mic polarity.

**Cost:** ~2 hours.

### 2.0.4 Audio-level sanity page

Extend the web UI with a live scope: plot `input_rms` over the last 60 s from SSE. Visual confirmation of what the mic is hearing. Big-picture this one — the goal is **"do I trust the sensor?"**, not pretty.

**Cost:** ~2 hours.

### 2.0.5 Dual-mic strategy — evaluation experiment

We have two PDM MEMS mics + per-channel PGA on the ES7210 4-ch ADC. Currently using 1 of 2 (mono capture = MIC1 only by BSP default). Options, ranked by likely value for a bedroom baby monitor:

| Strategy | Mechanism | Value | Complexity |
|---|---|---|---|
| **D. Detect-vs-listen split** | MIC1 → mel pipeline (model sees "honest" feed at `CONFIG_CRY_DETECT_MIC_GAIN_DB`). MIC2 → `/audio.pcm` stream at higher gain + optional HPF → parent hears a cleaner, louder listen-through. | UX: parent hears better than model needs. Decouples detection gain from listen gain. | **Medium.** Needs 2-ch capture + route channels to different consumers inside `audio_capture`. ~1 day. |
| **B. Beamforming (AFE BSS)** | ESP-SR AFE consumes 2-ch → emits 1-ch direction-isolated audio → fed to YAMNet. | **Accuracy**: rejects off-axis noise (TV) if crib is at a different angle. Best expected FP win. | Depends on Milestone 2.2. ~2-3 days. |
| **C. Dynamic-range extender (dual gain)** | MIC1 @ low PGA (no clipping on loud events). MIC2 @ high PGA (catches quiet events). Pick loudest unclipped sample per sample. | Wider dynamic range in a single mono stream. Academic for now — no clipping observed yet at 36 dB PGA on the single mic. | Medium. ES7210 per-channel PGA registers not exposed by `esp_codec_dev` — would require direct I²C write. ~1 day. |
| **A. Stereo mix-down** | Average the two mics → single mono stream. | +3 dB SNR over single mic (theoretical). | Lowest. Just `channel=2` + downmix in capture task. ~½ day. |
| **E. DOA (cross-correlation)** | Compute cross-correlation between the two mic streams → direction estimate. | Novelty; not needed for detection. | High; defer unless product story requires "cry came from the crib vs from the doorway". |

**Recommendation:** D and B are the valuable ones. D is a UX win the parent can feel on first listen; B is the FP/TP quality lever. Start with D (no AFE dependency), revisit after AFE lands.

**Success criterion for D:** listening to `/audio.pcm` after change sounds measurably louder and clearer than before without affecting `cry_conf` distribution (i.e. detection gain held fixed).

---

## Milestone 2.1 — real-audio INT8 calibration

Straight follow-on from §4.6 of the lab notebook. The synthetic calibration sets are measurably biasing class 20 output up around 0.6 in a quiet room.

### Experiment
1. Collect 200–500 real 16 kHz mono WAV clips from the *actual deployment room* using the test-record endpoint above, mixed with 50 ESC-50 `crying_baby` clips for positives.
2. Re-run `tools/convert_yamnet.py --audio-dir <collected> --calib-count 500`.
3. Compare `cry_conf` distributions (quiet room, ambient TV, baby-cry clip played through phone speaker) between old and new `.tflite` via a host `inference.py` harness.
4. Flash the new `.tflite` via OTA or SPIFFS-only reflash.
5. Re-publish to HF if the improvement is material.

**Success criterion:**
- Quiet-room `cry_conf` p99 < 0.2 (was ~0.65).
- ESC-50 cry-clip `cry_conf` > 0.8 reliably.
- Old threshold (0.85) can drop to ~0.6 without FP increase.

**Cost:** ~1 day (mostly data collection + training-adjacent host work).

---

## Milestone 2.2 — AFE preprocessing (Espressif `esp-sr`)

The C6 sibling project couldn't afford AFE (no PSRAM, no SIMD). **S3 can.** Cost is ~22 % CPU + 1.1 MB PSRAM (Espressif quoted) — both available.

### What AFE brings

- **BSS (Blind Source Separation / beamforming):** 2-mic array → steer toward a direction, suppress the other. Useful if crib is at one angle of incidence and the TV at another.
- **NS (Neural Noise Suppression):** pre-cleans the audio before YAMNet sees it. Expected FP drop.
- **AEC (Acoustic Echo Cancellation):** needed when we eventually play audio back (soothing sounds via ES8311) — cancels our own speaker from the mic input.
- **VADNet:** neural VAD, trained on 15 000 hours. Replaces my `input_rms` threshold as the "audio present" gate → lower CPU when silent.

### Integration

- Add `espressif/esp-sr` back to `idf_component.yml`.
- Create `afe_pipeline.c/h` module that consumes a 2-ch tap from `audio_capture` and emits cleaned mono PCM into a new StreamBuffer that `mel_features` reads from.
- Chain: ES7210 (2 ch) → `audio_capture` → AFE (2 ch in, 1 ch clean out) → `mel_features` → YAMNet.
- Keep a toggle in Kconfig so we can A/B compare AFE-on vs AFE-off on the same audio.

**Success criterion:** on a fixed test-audio playback (TV + phone-cry), AFE-on reduces FP rate ≥ 50 % and maintains TP rate on crying clips.

**Cost:** ~2–3 days including benchmarks.

---

## Milestone 2.3 — observability hardening

1. Fix `/log/tail` handler: move the 8 KB buffer off the handler stack to PSRAM heap.
2. Add `sd_logger` row for every threshold change + AFE on/off + stream listener join/leave — so the SD trace is self-describing for later analysis.
3. Add a `/recordings/` list to the web UI (HTML index); currently only JSON via `/events/list`.
4. Rotate logs daily at midnight UTC (currently only by size).
5. Expose `noise_floor_p50/p95` over SSE for the dashboard chart.

**Cost:** ~half-day.

---

## Milestone 2.4 — push notifications (no app, parent-friendly)

Bedroom monitor is useless if the parent is in another room and the alert only lights an LED + web page.

Approach: **Telegram bot** (free, trivial HTTP POST, works without a server).

- User creates a bot via `@BotFather`, gets a token. Stored in NVS via `/nvs/set?key=tg_token&val=…` endpoint (write-only, gitignored).
- On `detector_state → CRYING`, fire a single `POST https://api.telegram.org/bot<token>/sendMessage` with body `{chat_id, text: "🍼 Baby cry detected at 23:04:12 (conf 0.87)"}`.
- Rate-limit to 1 per `HOLD_MS` window.
- Mutual TLS / server-side validation deliberately out of scope — this is a home device.

**Success criterion:** parent's phone receives a notification within 10 s of the device firing, without any app install on the phone.

**Cost:** ~half-day.

---

## Milestone 2.5 — BLE parent-proximity auto-mute (stretch)

*Defer to Stage 3 unless 2.4 is fast.*

ESP32-S3 has BLE 5. Scan continuously for a whitelisted phone's MAC. If present → auto-mute Telegram notifications (parent is in the room). If absent → resume.

Uses hardware that's otherwise unused for the crying use case. Genuinely nice UX.

**Cost:** ~1 day. Deprioritised because it requires phone-side configuration and we haven't proven core detection yet.

---

## Decision tree — which milestone first

```
did 2.0 (hardware validation) → YES
         │
         ▼
is inference_count healthy? (>1 Hz sustained over 1 h)
         │
         ├── NO → hardware/firmware regression; stop and triage
         │
         └── YES ▼
             is false-positive rate in real room acceptable? (parent judgement)
                   │
                   ├── NO → 2.1 real-audio calibration (easy) before 2.2 AFE (heavy)
                   │
                   └── YES → skip straight to 2.4 Telegram push (done device)
                             then 2.2 AFE as quality improvement
```

**Recommendation for first increment:** 2.0 (hardware validation) → 2.1 (real-audio calibration) → 2.4 (Telegram push). That's a parent-usable bedroom monitor by end of next sprint, with AFE held as a later quality lever.

---

## Open research questions to chase in parallel

1. **What does the ES7210 datasheet actually say about BSP's channel-mask default?** Reading the raw register config via I²C would tell us definitively which physical mic is on slot 0. Cost: 30 min with a bench multimeter and the datasheet PDF in `ref/`.
2. **Is the "stream overrun" warning real or is it a non-issue?** Observed in early logs. Cost: 1 h to instrument `xStreamBufferBytesAvailable` over time.
3. **Does `CONFIG_TFLITE_MICRO_USE_ESP_NN=y` actually kick in, or are we using reference kernels?** The 506 ms latency is 2× our estimate. Toggling it off and re-measuring tells us. Cost: 1 h + one flash cycle.
4. **Noise-floor convergence time in real bedroom.** The 5 min warmup assumption is a guess. Plot `nf_p50/p95` over first hour and see when it stabilises. Cost: 1 observation session.
5. **WAV file byte rate during an alert.** 30 s @ 32 KB/s = 960 KB. 50 retained files = ~50 MB on SD. Fine, but confirm. Cost: measure.

---

## Tracking

When Stage 2 ships, update `docs/research/stage1-bringup-labnotebook.md` with the parking-lot section and create `docs/research/stage2-bringup-labnotebook.md`. Same rigor, same honest fault log.
