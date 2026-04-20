# Read serial output from the board

`idf.py monitor` does **not** work in non-TTY contexts (Claude Code, piped shells, subshells) — it exits immediately and looks like success. Use pyserial directly, **and respect the macOS DTR trap below**.

Usage: `/monitor [seconds]` (default 15)

---

## ⚠ The macOS DTR trap — read this before "why is serial silent?"

On macOS, opening `/dev/cu.usbmodem*` **raises DTR and RTS by default**. For the ESP32-S3's native USB-Serial-JTAG, DTR=asserted drives GPIO0 low and RTS=asserted drives EN low — i.e. **every time pyserial opens the port, the chip is forced back into download-mode / held in reset.** The device never boots the app, so serial stays silent, so you keep re-opening the port, so it stays stuck. Spent ~20 min of cry-detect-01 bring-up debugging this on 2026-04-17.

Symptoms:
- First serial read shows `rst:0x15 (USB_UART_CHIP_RESET), boot:0x0 (DOWNLOAD(USB/UART0))` → `waiting for download`.
- Subsequent reads return **0 bytes**.
- `esptool ... chip_id` **succeeds** (proves USB is fine; device is in download mode).
- Red LED (CH32V003 P6) stays off or dim; power-LED (green) stays solid.

### Fix 1 — `stty -hupcl` + dd (works reliably)

```zsh
stty -f /dev/cu.usbmodem3101 -hupcl 2>/dev/null
dd if=/dev/cu.usbmodem3101 of=/tmp/boot.log bs=1 count=50000 &
DD_PID=$!; sleep 15; kill $DD_PID 2>/dev/null; wait 2>/dev/null
head -200 /tmp/boot.log
```

`stty -hupcl` tells the driver not to drop DTR on close. `dd` opens the device at a lower level than pyserial and doesn't assert modem control lines. This is what actually grabbed the backtrace that unblocked us.

### Fix 2 — pyserial but set `dtr`/`rts` **before** open

```zsh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python -u -c "
import serial, time, sys
ser = serial.Serial()
ser.port = '/dev/cu.usbmodem3101'
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False   # must be set BEFORE open
ser.rts = False   # must be set BEFORE open
ser.open()
# Optional: pulse RTS only to reset without touching DTR
ser.rts = True; time.sleep(0.2); ser.rts = False; time.sleep(0.4)
start = time.time()
while time.time()-start < 20:
    n = ser.in_waiting
    if n: sys.stdout.buffer.write(ser.read(n)); sys.stdout.buffer.flush()
    else: time.sleep(0.05)
ser.close()
"
```

Still not 100 % on older macOS; prefer Fix 1 when serial silence is mysterious.

---

## Healthy read (once the DTR trap is avoided)

```zsh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python -u -c "
import serial, time, sys
ser = serial.Serial()
ser.port = '/dev/cu.usbmodem3101'; ser.baudrate = 115200; ser.timeout = 0.5
ser.dtr = False; ser.rts = False
ser.open()
ser.rts = True; time.sleep(0.2); ser.rts = False; time.sleep(0.4)  # reset only
start = time.time()
while time.time()-start < 15:
    d = ser.read(ser.in_waiting or 1)
    if d: sys.stdout.buffer.write(d); sys.stdout.buffer.flush()
ser.close()
"
```

- **Use IDF venv python** (`~/.espressif/python_env/...`) — system `python3` has no pyserial.
- **ANSI colour escapes** appear as `[0;32mI (…)` etc. — log-level colouring, not corruption.

---

## Recovery sequence when serial is silent AND Wi-Fi is unreachable

The device is stuck. Do not loop attempts; follow this order:

1. **Check LED first.** Red solid/OFF/blink pattern tells you which phase crashed (see `cry-detect-01` LED table). Don't touch serial yet.
2. **Network probe** (no DTR impact): `ping 192.168.1.100` + `curl http://cry-detect-01.local/metrics`. If either works, app is running — issue is pyserial-side, use Fix 1 above.
3. **If both fail:** chip is stuck pre-Wi-Fi. Try Fix 1 (`stty -hupcl` + `dd`) first to capture any crash log that arrived before we started reopening the port.
4. **If still silent**, force-download-mode flash:
   - Unplug USB. Hold BOOT. Plug USB. Release BOOT.
   - `esptool --chip esp32s3 --port /dev/cu.usbmodem3101 --before default_reset --after no_reset erase_flash`
   - `esptool ... --after no_reset write_flash ...` to flash fresh binaries.
   - **Physically unplug and replug without holding BOOT** — don't let esptool or pyserial drive the reset. Clean power-cycle is the only guaranteed way out of the DTR trap.
   - Then read with Fix 1.

Rule: **if pyserial and the chip disagree about reset state, PHYSICAL power cycle wins**. Don't expect DTR/RTS toggles to recover it.

---

## Using `addr2line` on a backtrace

Once a crash log is captured, resolve `Backtrace: 0xAAAA:0xBBBB 0xCCCC:0xDDDD …` to function names:

```zsh
$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line \
    -pfiaC -e /tmp/ws-cry-detect-01-build/cry-detect-01.elf  <addr1> <addr2> …
```

Only the first of each `A:B` pair is the code pointer (the second is the stack-frame).

---

## Healthy boot (GC2145 + cry-detect-01)

```
esp_psram: Adding pool of 8192K of PSRAM memory …           ← PSRAM active
spi_flash: flash io: qio                                     ← 16 MB QIO flash
main_task: Started on CPU0
main_task: Calling app_main()
sdlog: SD not present; using internal fallback FAT           ← expected if no card
sdlog: logging to /logs/cry-0000.log
yamnet: loaded 4052672 bytes from /yamnet/yamnet.tflite
yamnet: arena used: 1157736 / 1572864 bytes
yamnet: output size=521, has_classifier=1
web: http server up on :80 (up to 2 SSE clients)
infer task: start (core=1)
example_connect: Connecting to <SSID>...
wifi:connected with <SSID>, aid = …, channel N, BW20
example_netif_handlers: example_netif_sta ip: 192.168.x.y
net: time synced: 2026-04-17 12:01:20
net: mDNS: cry-detect-01.local / _http._tcp
main: boot complete
hk: up=10s  st=3  wifi=1 rssi=-47  ntp=1  sd=0  heap=…  psram=…  rms=…
```

---

## If It Still Hangs Without Output

- The previous flash may have corrupted the app partition — `/restore-factory`.
- Serial might be held by another process — `lsof /dev/cu.usbmodem3101`.
- Check the **red LED** on CH32V003 P6 for the boot-state pattern before assuming serial is the source of truth.
