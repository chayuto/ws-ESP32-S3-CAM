# Flash firmware to the connected ESP32-S3-CAM board

Usage: `/flash <project_path>` (same path used with `/build`)

## Steps

1. Activate IDF: `. ~/esp/esp-idf/export.sh 2>/dev/null`

2. Detect port. ESP32-S3 exposes **native USB-Serial/JTAG** — no CP2102, just `usbmodem*`:
   ```zsh
   PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
   # Expect /dev/cu.usbmodem3101 on this host.
   ```

3. Flash (no separate bootloader button needed — DTR/RTS auto-enter download mode):
   ```zsh
   idf.py -C <project_path> -B /tmp/ws-esp32s3-build/$(basename <project_path>) \
          -p "$PORT" flash 2>&1 | tail -12
   ```

4. A ~5 MB vendor example flashes in ~10 s. Expect final lines:
   ```
   Hash of data verified.
   Leaving...
   Hard resetting via RTS pin...
   Done
   ```

## Warnings

- **Do NOT run `idf.py flash monitor` combined in a non-TTY shell** (piped, subshell, Claude Code agent). Monitor exits immediately → flash aborts mid-write → "No bootable app" panic. Always flash first, then monitor separately — see `/monitor`.
- If the port is busy, `esptool` fails with "Could not open serial port". A previous serial reader is still attached — kill it.
- If the chip is in a bad state and auto-download fails, force bootloader mode: unplug USB → hold **BOOT** → plug USB → release **BOOT**. Then retry flash.

## Board Is Currently Running Factory Firmware?

Flashing **overwrites** Waveshare's factory image (multi-app launcher with Settings/Camera/Music/XiaoZhi AI).
The factory `.bin` lives at `ref/demo/ESP32-S3-CAM-OVxxxx/Firmware/ESP32-S3-CAM-XXXX-Factory.bin` (plus per-LCD-size variants).
To restore it, use `/restore-factory`.

## Report

- Success: port used, bytes written, final "Hard resetting" line.
- Failure: the esptool error line, with a suggested next action.
