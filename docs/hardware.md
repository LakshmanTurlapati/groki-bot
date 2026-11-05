[English](hardware.md) | [中文](hardware.zh-CN.md) | [日本語](hardware.ja.md)

# Hardware



### Standard unit: CoreS3 + Stack-chan base

![I2C devices, display and UART1 dual-servo wiring between CoreS3 and the Stack-chan base](images/hardware.svg)

| Item | Spec |
|---|---|
| SoC | ESP32-S3R8 |
| PSRAM | 8 MB, in-package Quad SPI, 80 MHz |
| Flash | 16 MB external SPI (W25Q128 package) |
| Display | 320×240 IPS touch screen |
| Servos | SCS0009 ×2 (yaw / pitch) |
| Servo bus | UART1, TX GPIO 6 / RX GPIO 7, 1 Mbps, 8N1 |
| Servo ID / zero | Yaw ID 1, zero 460; pitch ID 2, zero 620 |
| Step | 1 step ≈ 0.3125° (`deg = (raw - zero) * 5 / 16`) |
| Default soft limits | Yaw ±40°, pitch -10° to +25° |
| Head touch | Si12T, I²C 0x68, three zones (front / middle / back) |
| Battery gauge | INA226, I²C 0x41 |
| IO expander | PY32, I²C 0x6F; pin 0 controls servo VM power (wait 200 ms after enabling before using the bus) |
| PMIC | AXP2101, I²C 0x34 (managed by M5Unified) |
| LCD touch | I²C 0x38 (managed by M5Unified) |

Takao Base runs the same `cores3` firmware: servos use Port A (TX GPIO 2 / RX GPIO 1, half duplex with echo cancellation), with no servo power control and no INA226.

### Other supported boards

| Board | Build slug | Display | Notes |
|---|---|---|---|
| CoreS3 + Stack-chan base | `cores3` | 320×240 IPS + touch | Default. Two servo axes, head touch, INA226 |
| CoreS3 + Takao Base | `cores3` | Same | Half-duplex servos on Port A; no servo power control or battery gauge |
| AtomS3R + Atomic ECHO BASE | `atoms3r` | 128×128 LCD | No servos. 8 MB Octal PSRAM, 8 MB flash, ES8311, BtnA toggles UI / AP |
| AtomS3 (no PSRAM) + ECHO BASE | `atoms3` | 128×128 LCD | Slim profile. No conversation, no BLE audio, no RTP |
| M5 StopWatch (C152) | `stopwatch` | 466×466 round AMOLED + touch | No servos. 8 MB Octal PSRAM, 16 MB flash, gaze follows touch, ES8311 |

The board is detected at boot and reported through `set_board_kind()` to the UI and feature switches. Build with `make build BOARD=<slug>` (default `cores3`). PSRAM mode is fixed at link time per board, so firmware images cannot be swapped between boards.

## Technical specs

| Item | Spec |
|---|---|
| Framework / language | ESP-IDF 5.5 (verified on 5.5.4; 5.4.2 also compiles) / C++20 |
| Target chip | `esp32s3` |
| Face rendering | M5GFX. Full-screen buffer when PSRAM is present; 17 ms budget per frame; full-screen overlays yield the CPU every 33 ms |
| Expressions | 20: Neutral, Happy, Sad, Angry, Doubt, Sleepy, Listening, Thinking, Excited, Curious, Confused, Surprised, Dizzy, Affection, Bored, plus the Grok Bot run states Working, Waiting, Blocked, Done, Failed |
| Pill face compositing | `main/pill_face.hpp` replays the design keyframes (CSS cubic-bezier easing, nested transforms) and writes up to 28 capsule segments per frame; poses blend with four weight sets (`expression`, `expression_from` and two hold layers) over 300 ms; at most 45 draw primitives per frame |
| Avatar DSL | `.avdsl` source compiled to `.avbc` bytecode, hot-swappable over BLE or Wi-Fi. The default face is `assets/grok_face.avdsl` |
| Lip sync | 16 kHz mono, 256-point radix-2 FFT; speech-band log energy plus spectral flux; EWMA noise floor follows the environment |
| Servo motion | Trapezoidal velocity `PathGenerator`, torque on only while moving; limits stored in NVS |
| Volume | 0 to 200%, adjustable live from BLE, Wi-Fi or the on-device UI |
| OTA | Dual slots with boot rollback. CoreS3 / StopWatch 4 MiB per slot (`partitions_16mb.csv`); AtomS3R 0x350000 per slot (`partitions_8mb.csv`); AtomS3 3 MiB per slot (`partitions.csv`) |
| BLE | NimBLE, Just Works pairing; application-layer X25519 + AES-256-GCM; optional password |
| Wi-Fi settings | After connecting, HTTP on port 80 serves `settings_wifi.html`, mDNS `stackchan-XXXXXX.local` |
| SoftAP | SSID `Stackchan-XXXXXX` with WPA2; the LCD shows a Wi-Fi QR code; DNS hijack plus an HTTP 404 catch-all opens the captive portal |
