# Restore Waveshare factory firmware

The board ships running Waveshare's multi-app launcher (Settings / Camera / Music Player / Recorder / XiaoZhi AI). Flashing any example overwrites it. This restores it.

Usage: `/restore-factory` (default = no-LCD build) or `/restore-factory <size>` where `<size>` ∈ {`1_83`, `2`, `2_8`, `3_5`}.

## Pre-built Factory Binaries (in the vendor repo clone)

```
ref/demo/ESP32-S3-CAM-OVxxxx/Firmware/
├── ESP32-S3-CAM-XXXX-Factory.bin              ← no LCD / USB-only use
├── ESP32-S3-CAM-XXXX-Factory-LCD_1_83.bin     ← 1.83" LCD variant
├── ESP32-S3-CAM-XXXX-Factory-LCD_2.bin        ← 2"   LCD variant
├── ESP32-S3-CAM-XXXX-Factory-LCD_2_8.bin      ← 2.8" LCD variant
└── ESP32-S3-CAM-XXXX-Factory-LCD_3_5.bin      ← 3.5" LCD variant
```

These are **merged** images — single file containing bootloader + partition table + app.

## Flash Command

```zsh
. ~/esp/esp-idf/export.sh 2>/dev/null
PORT=$(ls /dev/cu.usbmodem* | head -1)
BIN=ref/demo/ESP32-S3-CAM-OVxxxx/Firmware/ESP32-S3-CAM-XXXX-Factory.bin   # or LCD variant

python -m esptool --chip esp32s3 --port "$PORT" -b 460800 \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
    0x0 "$BIN"
```

Offset `0x0` because these are merged images. Do NOT specify bootloader/partition offsets.

## If Flash Fails to Connect

Force download mode: unplug USB → hold **BOOT** → plug USB → release **BOOT** → retry.

## After Restore

The board reboots into the XiaoZhi launcher (USB host appears as mass-storage / serial console). Use the Camera app on the LCD (if fitted) or the USB UVC view to confirm GC2145 is alive.
