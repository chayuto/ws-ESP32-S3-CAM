# Hardware utilization audit — cry-detect-01

*Written 2026-04-17 during Stage 1 deployment. Cross-reference against `.claude/commands/hardware-specs.md` (source of truth for what's on the board) and `docs/research/hw-verification-stage1.md` (source of truth for what's wired + how the BSP exposes it).*

Goal: for every piece of hardware on this board, record **what Stage 1 uses vs doesn't**, with rationale. Makes Stage 2 planning concrete — you can't optimise what you don't know you have.

---

## Status legend

- ✅ **Active** — Stage 1 actively uses it in the pipeline.
- 🟡 **Partial** — used, but not to its full capability.
- ⬜ **Unused but easy** — trivial to adopt; deferred for reason noted.
- 🔒 **Rejected for this project** — deliberate scope decision with reason.
- ⚠️ **Unavailable** — physically absent on our SKU (e.g. LCD).

---

## 1. Compute / memory

| Resource | Spec | Status | Usage |
|---|---|---|---|
| **ESP32-S3 dual-core LX7** | 240 MHz, FPU, PIE vector SIMD | ✅ | Core 1: audio_capture, mel, YAMNet inference. Core 0: Wi-Fi, HTTP, mDNS, SNTP, housekeeping. Target split. |
| **SRAM** | ~316 KiB dynamic | ✅ | ~185 KB used; 131 KB headroom. |
| **PSRAM** | 8 MB OPI @ 80 MHz, DMA-capable | ✅ | YAMNet tensor arena (1.54 MB), mel filterbank, audio ring, Wi-Fi/LWIP buffers. |
| **Flash** | 16 MB QIO @ 80 MHz | ✅ | 5 MB app / 5 MB SPIFFS (yamnet) / 1 MB FAT (fallback logs). 4+ MB unused. |
| **Hardware crypto** (AES/SHA/RSA/ECC/RNG/DS) | on-die accelerators | ⬜ | No HTTPS / mTLS yet → idle. Would kick in automatically if we add TLS. |
| **JPEG HW encoder** | not present on S3 (P4-only) | ⚠️ | N/A. |
| **USB-OTG host** | available | ⬜ | Stage 1 uses USB only for power/flashing. OTG-host mode not exploited. |

---

## 2. Audio input — ES7210 4-channel ADC

| Resource | Spec | Status | Usage |
|---|---|---|---|
| **ES7210 ADC** (I²C 0x40) | 4 channels TDM, I²S slave | 🟡 | We ask `esp_codec_dev` for 1 channel → only **MIC1 slot** is read. MIC2 + 2 aux slots wasted. |
| **MIC1 PDM MEMS mic** | physical mic, wired to ES7210 ch0 | ✅ | Single signal source for the entire pipeline. |
| **MIC2 PDM MEMS mic** | physical mic, wired to ES7210 ch1 | ⬜ | **Key underutilisation.** Stage 2.0.5 will evaluate mix / beamform / dual-gain / detect-vs-listen split strategies. |
| **Aux ch2** (playback echo reference slot) | routed for AEC | ⬜ | No speaker playback yet → no use. Activates automatically if Stage 2 soothing audio lands alongside AFE. |
| **Aux ch3** | spare | ⚠️ | Unused per BSP config. |
| **ES7210 per-channel PGA** | independent gain per ch | ⬜ | BSP exposes one gain (`esp_codec_dev_set_in_gain`). For dual-gain dynamic-range extender we'd need direct I²C register writes. Deferred. |
| **ES7210 HPF / ALC** | register-configurable | ⬜ | Not exposed by `esp_codec_dev`; defaults only. Could improve ambient-rumble rejection (HPF ~100 Hz) with direct reg writes. |
| **I²S peripheral I2S_NUM_1** | master, 32-bit slots, Philips | ✅ | Full-duplex wired but only RX used in Stage 1. |

**Biggest Stage-1 gap:** only 1 of 2 mics. The board was explicitly sold as a **dual-mic array** for beamforming; we're leaving that table.

---

## 3. Audio output — ES8311 DAC + speaker

| Resource | Spec | Status | Usage |
|---|---|---|---|
| **ES8311 codec** (I²C 0x18/7-bit) | DAC output, 100 dB SNR | 🔒 | Stage 1 scope: *detect only*. No output path. |
| **Onboard speaker driver** (internal to ES8311) | no external amp enable pin needed | 🔒 | Same — no sound-producing use case yet. |
| **I²S DOUT (GPIO 14)** | routed to ES8311 | 🔒 | Idle. |

Not used because the Stage-1 product is a *passive listener*. Stage-2 candidates that would activate it:
- Auto-play soothing white noise / lullaby on detection (via SD → ES8311).
- Two-way talk-back (parent speaks from phone → stream back to crib).
- Audio confirmation tone when the user button is pressed.

Hardware is fully functional and proven (the vendor `03_audio_play` example uses it). Unblocked — just scope.

---

## 4. Vision — GC2145 camera

| Resource | Spec | Status | Usage |
|---|---|---|---|
| **GC2145 sensor** (SCCB 0x3C) | 2 MP UXGA, DVP | 🔒 | **Deliberately parked 2026-04-17.** |
| **LCD_CAM peripheral** (DVP RX on pins 17,18,21,38–48) | 8-bit parallel | 🔒 | Idle. |

**Rationale:** user scope is a "discreet bedroom monitor" — baby not facing the camera in the dark. Face-detect gating adds 400 KB PSRAM + latency for zero expected benefit. See `docs/research/stage1-bringup-labnotebook.md` / `cry-detect-01-plan.md` change-log entry v1.

Hardware is fully functional (verified in the vendor `01_simple_video_server` example). Unblocked — purely a scope decision.

---

## 5. I/O expander — CH32V003

| Resource | Spec | Status | Usage |
|---|---|---|---|
| **Red LED** (P6, active-LOW) | physical indicator | ✅ | 6-state machine: boot / connecting / syncing / idle / alert / error / streaming-breathing. Critical fallback observability. |
| **Battery voltage ADC** | 10-bit ADC via I²C | ⬜ | **Easy to add for Stage 2 battery-power use case.** One-line read + metrics field. Irrelevant while USB-powered. |
| **P0 touch reset** | N/A — no LCD fitted | ⚠️ | — |
| **P1 LCD backlight PWM** | N/A — no LCD fitted | ⚠️ | — |
| **P2 LCD reset** | N/A — no LCD fitted | ⚠️ | — |
| **P3-P5 other** | reserved / ancillary | ⚠️ | — |

---

## 6. Storage

| Resource | Spec | Status | Usage |
|---|---|---|---|
| **SDMMC 1-bit** (CLK=16, CMD=43, D0=44) | ~2–5 MB/s real | ✅ | `sd_logger` rotating CSV + `event_recorder` WAVs. Works on this hardware without the C6's SPI-sharing drama. |
| **Internal FAT partition** (`logs_fat` 1 MB) | SD-absent fallback | ✅ | Auto-mounts to `/logs` if no SD. Exercised this session. |
| **SPIFFS `yamnet` partition** (5 MB) | holds `yamnet.tflite` | ✅ | 4.05 MB used; ~700 KB headroom for a future model swap. |
| **NVS** (24 KB) | Wi-Fi creds + flags | ✅ | Wi-Fi credentials + key/value persistence (future: Telegram token, thresholds). |

---

## 7. User input

| Resource | Spec | Status | Usage |
|---|---|---|---|
| **BOOT button** (GPIO 0) | strapping pin, active LOW | ⬜ | Only used for bootloader entry. Could bind to "dump metrics to serial" in running app — low value. |
| **User button** (GPIO 15) | external pull-up, active LOW | ⬜ | **Ready to use.** `espressif/button ^4.1.4` already pulled in as BSP transitive dep. Ideal Stage-2 uses: alert acknowledge / silence, toggle listen-stream from device side, "bookmark" log event ("just fed baby"). |

---

## 8. Connectivity

| Resource | Spec | Status | Usage |
|---|---|---|---|
| **Wi-Fi 4** (802.11 b/g/n, +21 dBm) | STA mode | ✅ | `example_connect` → DHCP → SNTP → mDNS → HTTP. |
| **BLE 5** | LE + extended advertising | ⬜ | **Idle.** Stage 2.5 candidate: parent-proximity auto-mute. Flash cost ~50 KB + RAM ~30 KB if enabled. Currently `CONFIG_BT_ENABLED=y` by default — actually occupying ~80 KB. Could either disable to save RAM or put it to use. |
| **mDNS** | `cry-detect-01.local` | ✅ | Active. |

---

## 9. USB

| Resource | Spec | Status | Usage |
|---|---|---|---|
| **Native USB-Serial-JTAG** (GPIO 19/20) | CDC console + flasher | ✅ | Console + flashing. Bitten by macOS DTR trap (documented). |
| **USB-OTG host** (same PHY, role-switchable) | host capability | ⬜ | Would require role-switch; not needed. |

---

## 10. Power / form-factor

| Resource | Spec | Status | Usage |
|---|---|---|---|
| **USB-C 5 V input** | primary power | ✅ | Deployment running from USB. |
| **GH1.25 Li battery connector** | single-cell 3.7 V, ≤ 2000 mAh | ⬜ | **Deferred.** Bedroom placement is mains-powered; no portability requirement yet. Enables if we want truly wireless placement. Requires: Li-ion + 5 V boost handled by the onboard regulator path (check schematic before trusting). |
| **No PMIC** | simple linear regulators | — | Accept as-is. |

---

## 11. Summary — the "we have, we don't use" list

High-leverage, low-effort wins (Stage 2 fodder):

1. **MIC2 / 2-ch capture** — 2.0.5 in Stage 2 plan. The board's defining feature, left on the table in Stage 1.
2. **ES8311 speaker** — enables "auto-soothe" UX, two-way talk-back. Big parent-facing product uplift.
3. **User button (GPIO 15)** — ~30 min to add an "acknowledge" handler; adds meaningful on-device control.
4. **CH32V003 battery ADC** — 1-line read, `/metrics` field; free observability.
5. **BLE 5** — parent-proximity auto-mute. Best-in-class "smart bedroom monitor" feature if we go there.
6. **Hardware crypto / TLS** — free if we move the web UI to HTTPS (we will not, for local-LAN use).

Rejected-for-project (don't spend time on):

- GC2145 camera — not useful for night / non-facing baby.
- LCD + touch — SKU doesn't have a screen; would need FPC add-on.
- 802.15.4 — physically absent on ESP32-S3.

---

## Evidence / verification actions (to do at the bench)

Audit items that say "Active" and "Partial" should be checkable without code changes:

1. `/metrics` shows `input_rms` responds to real audio → MIC1 proven (saw rms jump 59 → 444 at t=887 s today; see monitor log).
2. `noise_floor_p95` changes over time → mel pipeline is consuming real audio.
3. SD log contains dated rows post-NTP → SD write-path proven.
4. LED cycles correctly through boot → idle via physical inspection → IO-expander path proven.
5. Web UI + SSE + mDNS + NTP — all confirmed via `curl` this session.

To validate in Stage 2.0:

- Which physical mic is MIC1? (tap test)
- Is the second mic actually wired? (ES7210 ch1 vs ch0 read)
- What's `input_rms` noise floor vs clipping ceiling? (convert to dB SPL approximately using the gain setting)
