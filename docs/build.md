[English](build.md) | [中文](build.zh-CN.md) | [日本語](build.ja.md)

[Path 1: firmware only](voice-backends.md#path-1-firmware-only)

# Build from source

You only need this if you want to change the firmware. To just use the robot, flash a release with the web flasher (path 1 above).

Docker is the recommended way to build; you do not need ESP-IDF on your machine. Start Docker Desktop (or the Docker daemon) first.

### Docker (recommended)

The image is `espressif/idf:v5.5.4`. The first pull is about 14 GB. The official image has no Node; the Makefile installs Node.js 18+ inside the container. Output goes to `build-cores3/`.

```sh
git clone https://github.com/JosssphZhou/groki-bot.git
cd groki-bot
git submodule update --init --recursive
tools/apply-m5-patches.sh                    # M5Unified and M5GFX fixes
make build-docker BOARD=cores3
```

`BOARD=` can be `stopwatch`. In this release `cores3` and `stopwatch` are verified to build. `atoms3r` and `atoms3` have not been rebuilt yet: their earlier build failure (face animation assets overflowing the 1 MB storage partition) no longer applies because those assets were removed; see [known issues, item 4](known_issues.md). The Docker path must be given the same `BOARD` so the matching sdkconfig defaults are loaded; output goes to `build-<board>/`.

Flashing does not run inside Docker. To write the firmware you just built, run on your machine (replace the port with yours, see [Download mode](troubleshooting.md#download-mode)):

```sh
make flash BOARD=cores3 PORT=/dev/cu.usbmodem1101     # macOS example
make monitor BOARD=cores3 PORT=/dev/cu.usbmodem1101
```

`make flash` needs ESP-IDF installed on your machine (it loads the IDF environment and then calls `idf.py flash`). Without a local IDF, use the web flasher for a published release: <https://josssphzhou.github.io/groki-bot/>. That page reads GitHub Releases; it cannot pick files from `build-cores3/`.

### Local ESP-IDF

Environment: ESP-IDF 5.5 (verified on 5.5.4). Install it following the Espressif documentation, run `./install.sh esp32s3` in the IDF source directory, then `source export.sh`. The Makefile default is `IDF_PATH=$(HOME)/esp-idf/5.5.4`. Node.js 18+ must be on the build machine's `PATH` (CMake looks for `node` at configure time to compile `.avdsl` to bytecode; the compiler is in `tools/avatar_dsl/`). If the system `python3` is 3.14, IDF 5.5 looks for a matching virtual environment and `make` fails immediately when it is missing.

```sh
git clone https://github.com/JosssphZhou/groki-bot.git
cd groki-bot
git submodule update --init --recursive
tools/apply-m5-patches.sh                    # M5Unified and M5GFX fixes
make set-target BOARD=cores3                 # first time only (each BOARD has its own build directory)
make build     BOARD=cores3
make flash     BOARD=cores3 PORT=/dev/cu.usbmodem1101    # Linux: /dev/ttyACM0
make monitor   BOARD=cores3 PORT=/dev/cu.usbmodem1101
```

Local `make build BOARD=<slug>` output goes to `build-<board>/`.

If CMake reports `Could not find NODE_EXECUTABLE`, the current environment (including the Docker image) has no `node` on `PATH`. No source file is missing.

`tools/apply-m5-patches.sh` applies the repository's M5Unified fixes and the M5GFX I2C read-timeout cleanup fix. It also makes M5Unified use the pinned local M5GFX submodule, avoiding a second downloaded library that could override the patched implementation. Run it after updating submodules; it is safe to rerun and reports conflicts instead of silently skipping a dirty submodule.

OpenAI and Gemini API keys are not compiled into the firmware; they are written to NVS at runtime through the BLE or Wi-Fi settings page. A compile-time default can be supplied in `sdkconfig.defaults.local`, which is gitignored.

## CoreS3 camera

The default CoreS3 build enables the GC0308 camera. Normal photos and the color-bar test use 320×240 RGB565; RAW diagnostics use 640×480 Bayer and then restore the normal format. The camera's SCCB register traffic uses M5Unified's existing internal I2C bus, including its transaction mutex, so touch, power and audio cannot reconfigure a competing camera bus driver.

Experimental on-device WakeNet is disabled in this build to leave internal DMA memory for the camera and microphone. The gateway's computer-side wake word does not depend on this setting. A saved local-ASR mode falls back to microphone lip-sync without changing NVS; select Conversation in settings to use gateway voice. To build on-device ASR instead, put `CONFIG_STACKCHAN_CAMERA_ENABLED=n` and `CONFIG_STACKCHAN_ASR_ENABLED=y` in `sdkconfig.defaults.local`. Kconfig prevents enabling both. Keep `CONFIG_STACKCHAN_QR_TEST_AT_BOOT=n`: the standalone QR test has its own camera driver and cannot run alongside the resident photo service.

Existing `build-cores3/sdkconfig` values override changed defaults. For an existing build directory, use `idf.py -B build-cores3 -DSDKCONFIG=build-cores3/sdkconfig menuconfig` to enable **Stack-chan board → Enable GC0308 DVP camera driver**, or back up the generated sdkconfig and create a fresh build with `make set-target BOARD=cores3` followed by `make build BOARD=cores3`. Check that `sdkconfig.defaults.local` does not turn the camera off.

After flashing, open the device's Wi-Fi settings page and use its camera capture controls. `/api/status` should report `has_camera:true` on camera-equipped boards. This reports hardware capability; a temporary initialization/capture failure returns HTTP 503 and a later request retries. Unsupported boards return HTTP 404. The photo interface is `/api/camera/capture`; gateway MCP `take_photo` is a separate interface that this firmware does not implement.

To check a device, capture an ordinary photo, a color-bar frame, a RAW frame, and another ordinary photo. Repeat after idle time and while voice is connected. Confirm that images have no torn bands, test bars do not persist, touch resumes, and voice still records/plays after capture. Normal responses contain exactly 153,600 bytes with `X-Frame-Format: rgb565be`; RAW responses contain 307,200 bytes with `X-Frame-Format: bayer8-rggb`. A build and host tests cannot establish sensor image quality or DMA headroom on the physical board.

Use the flashing tool's normal reset, not esptool's `watchdog_reset`, while the CoreS3 camera is active. The camera's pixel clock shares GPIO45, which also selects flash voltage at reset on boards without a fixed-voltage eFuse. Normal firmware restarts assert the camera's reset through the AW9523 expander first; external resets bypass that handler. If a reset produces partition-table magic errors, power-cycle the entire board before assuming the flash contents are corrupt. Disconnect USB and external power, hold the left POWER button for six seconds to shut down the battery-powered board, then reconnect it ([CoreS3 power instructions](https://docs.m5stack.com/en/core/CoreS3)).

## Web flasher and settings pages

Flash a published release from the browser (Chrome / Edge):

- **Flash**: <https://josssphzhou.github.io/groki-bot/>
- **BLE settings**: <https://josssphzhou.github.io/groki-bot/settings.html> (Web Bluetooth, desktop Chrome / Edge only)
- **Wi-Fi settings**: once the device is on Wi-Fi, open `http://stackchan-XXXXXX.local/` (mDNS)
- **iOS / SoftAP**: in AP mode, scan the Wi-Fi QR code on the LCD with the iPhone camera and the captive portal opens the settings page. On CoreS3 / StopWatch, tap the top-right corner of the screen to open the device UI and choose AP mode on the control page. On AtomS3R / AtomS3, a short press on BtnA opens the status overlay and a long press cycles `operation_mode`.

After a `vX.Y.Z` tag is pushed, CI (when GitHub Actions is enabled) builds the four boards and attaches them to a Release; the Pages site then updates. The web flasher source is `docs/index.html` and the BLE settings page source is `tools/settings.html`.
