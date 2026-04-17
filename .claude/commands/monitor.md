# Read serial output from the board

`idf.py monitor` does **not** work in non-TTY contexts (Claude Code, piped shells, subshells) — it exits immediately and looks like success. Use pyserial directly.

Usage: `/monitor [seconds]` (default 15)

## The Reader

```zsh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python -c "
import serial, time
ser = serial.Serial('/dev/cu.usbmodem3101', 115200, timeout=1)
# DTR/RTS wiggle = reset the board into the fresh boot log
ser.dtr=False; ser.rts=True; time.sleep(0.1); ser.rts=False; time.sleep(0.3)
start=time.time()
while time.time()-start < 15:
    d = ser.read(ser.in_waiting or 1)
    if d: print(d.decode('utf-8', errors='replace'), end='')
ser.close()
"
```

Notes:
- **Use IDF venv python** (`~/.espressif/python_env/...`) — system `python3` has no pyserial.
- **DTR/RTS wiggle** triggers a soft reset so the log starts at boot, not mid-run.
- **ANSI colour escapes** appear as `[0;32mI (…)` etc. — that's the log level colouring, not corruption.

## What a Healthy Boot Looks Like (GC2145 variant)

```
esp_psram: Adding pool of 48K of PSRAM memory …        ← PSRAM active
spi_flash: flash io: qio                                ← 16 MB QIO flash
main_task: Calling app_main()
example_init_video: DVP camera sensor I2C port=0, scl_pin=7, sda_pin=8
E (…) sccb_i2c: … failed to i2c transmit                ← OV2640 probe miss, expected
E (…) ov5640: Camera sensor is not OV5640, PID=0x9090   ← OV5640 probe miss, expected
E (…) gc0308: Get sensor ID failed                      ← GC0308 probe miss, expected
I (…) gc2145: Detected Camera sensor PID=0x2145         ← ✓ GC2145 HIT
example_connect: Connecting to <SSID>...
wifi:connected with <SSID>, aid = …, channel N, BW20
example_netif_handlers: example_netif_sta ip: 192.168.x.y
example: video0: width=320 height=240 format=422P
example: Starting stream server on port: '80'
example: Camera web server starts
main_task: Returned from app_main()
```

The four `E (…)` lines above `gc2145 detected` are the expected SCCB auto-probe sequence — **not** errors.

## If It Hangs Without Output

- The previous flash may have corrupted the app partition — re-flash, or `/restore-factory`.
- Serial might be held by another process — `lsof /dev/cu.usbmodem3101`.
