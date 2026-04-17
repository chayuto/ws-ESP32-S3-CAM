# Build an ESP-IDF project for ESP32-S3-CAM

Activate ESP-IDF v5.5.3+ and build out-of-tree. Usage: `/build <project_path>`

`<project_path>` is anything with a top-level `CMakeLists.txt` — a `projects/<name>/` folder
you wrote, or a vendor example under `ref/demo/ESP32-S3-CAM-OVxxxx/examples/ESP-IDF-v5.5.1/<n>/`.

## Steps

1. Activate IDF:
   ```zsh
   . ~/esp/esp-idf/export.sh 2>/dev/null
   ```

2. Pick a build directory **outside the project** so vendor clones stay clean:
   ```zsh
   BUILD=/tmp/ws-esp32s3-build/$(basename <project_path>)
   mkdir -p "$BUILD"
   ```

3. Run the build:
   ```zsh
   idf.py -C <project_path> -B "$BUILD" build 2>&1 | tail -25
   ```

## Known Failure Recovery

| Symptom | Cause | Fix |
|---|---|---|
| `The "path" field in the manifest file "/System/Volumes/Data/home/wxggc/esp/v551/esp-idf/..."` | Vendor `dependencies.lock` has Waveshare's build-machine IDF_PATH baked in | `rm <project>/dependencies.lock` and rebuild |
| `Partitions tables occupies 5.1MB of flash which does not fit in configured flash size 2MB` | `sdkconfig` was deleted; regenerated from minimal `sdkconfig.defaults` lost 16MB flash setting | `cd <vendor example> && git checkout sdkconfig` to restore hand-tuned config |
| `IDF_TARGET not set` or IRAM overflow | Wrong target | `idf.py -C <path> set-target esp32s3` (NOT esp32c6 — that's sibling repos) |
| ESP_VIDEO or camera component missing | `main/idf_component.yml` missing deps | Ensure it lists `espressif/esp_video` and `waveshare/custom_io_expander_ch32v003` |
| `app partition is too small` | Custom partition not activated | `sdkconfig.defaults` needs `CONFIG_PARTITION_TABLE_CUSTOM=y`, `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`, `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` |
| `managed_components/` dir polluting vendor clone | `idf.py` downloads deps into project dir | Inevitable but gitignored in our `.gitignore` |

## Patching Wi-Fi Credentials

Vendor examples use `example_connect` (protocol_examples_common). Keys are `CONFIG_EXAMPLE_WIFI_SSID` / `CONFIG_EXAMPLE_WIFI_PASSWORD`.

**Do NOT `rm sdkconfig` to force regen** — Waveshare tracks a hand-tuned `sdkconfig` in git
with critical settings (16 MB flash, OPI PSRAM) that are not in their minimal `sdkconfig.defaults`.

In-place sed patch (macOS syntax):
```zsh
sed -i '' 's/^CONFIG_EXAMPLE_WIFI_SSID=".*"/CONFIG_EXAMPLE_WIFI_SSID="<ssid>"/;
           s/^CONFIG_EXAMPLE_WIFI_PASSWORD=".*"/CONFIG_EXAMPLE_WIFI_PASSWORD="<pw>"/' \
           <project>/sdkconfig
```

## Report

- On success: binary path + size + free app-partition bytes from the `idf.py` tail.
- On failure: the specific error line, matched to the table above if possible.
