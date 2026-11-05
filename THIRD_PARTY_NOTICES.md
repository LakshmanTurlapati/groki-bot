# Third-party notices

Groki Bot's own source code is distributed under the Boost Software License 1.0 ([LICENSE](LICENSE)), Copyright (c) 2026 Kenta IDA and Copyright (c) 2026 Joseph Zhou. The exception is the gateway in [gateway/](gateway/), which is a fork of an MIT project and stays under the MIT License ([gateway/LICENSE](gateway/LICENSE)); see [Gateway](#gateway-gateway) below.

This file lists the third-party software and data that are included in this repository, compiled into the firmware images, or served from the GitHub Pages site (web flasher and settings pages). An HTML version with the full text of each license is at <https://josssphzhou.github.io/groki-bot/licenses.html> (source: [docs/licenses.html](docs/licenses.html)).

## Firmware upstream

- Based on [ciniml/stackchan-idf](https://github.com/ciniml/stackchan-idf), Copyright (c) 2026 Kenta IDA, under the Boost Software License 1.0. The copyright notice and full license text are kept in [LICENSE](LICENSE).

## Compiled into the firmware

| Component | Location | License | Copyright |
|---|---|---|---|
| hts_engine API 1.10 (modified for embedded use, see [components/hts_engine/README.md](components/hts_engine/README.md)) | `components/hts_engine/` | BSD-3-Clause, full text in [components/hts_engine/COPYING](components/hts_engine/COPYING) | (c) 2001-2015 Nagoya Institute of Technology, Department of Computer Science; (c) 2001-2008 Tokyo Institute of Technology, Interdisciplinary Graduate School of Science and Engineering |
| M5GFX 0.2.36 | submodule `components/M5GFX`, plus [patches/m5gfx.patch](patches/m5gfx.patch) | MIT | (c) 2021 M5Stack |
| efont Chinese bitmap fonts (inside M5GFX, used for on-screen text) | `components/M5GFX/src/lgfx/Fonts/efont/` | BSD-3-Clause | (c) 2000-2001 /efont/ The Electronic Font Open Laboratory |
| Adafruit GFX classic 5x7 font (inside M5GFX, `Font0`) | `components/M5GFX/src/lgfx/Fonts/glcdfont.h` | BSD-2-Clause | (c) 2012 Adafruit Industries |
| M5Unified | submodule `components/M5Unified`, plus [driver fixes](patches/m5unified.patch) and [local M5GFX selection](patches/m5unified-local-m5gfx.patch) | MIT | (c) 2021 M5Stack |
| tl::expected 1.3.1 | submodule `components/tl_expected/expected` | CC0-1.0 | Sy Brand |
| ESP-IDF 5.5 (bootloader, drivers, FreeRTOS, lwIP, mbedTLS, NimBLE and other bundled components) | build framework | Apache-2.0, plus the licenses of its bundled components listed in ESP-IDF's `COPYRIGHT.rst` | (c) Espressif Systems (Shanghai) Co., Ltd. and contributors |

### ESP-IDF managed components

Declared in `main/idf_component.yml` and downloaded by the IDF component manager at build time. Versions are the ones resolved for the v0.2.0 build.

| Component | Version | License | Copyright |
|---|---|---|---|
| espressif/cjson | 1.7.19 | MIT | (c) 2009-2017 Dave Gamble and cJSON contributors |
| espressif/cmake_utilities | 1.1.1 | Apache-2.0 | Espressif Systems (build scripts only) |
| espressif/dl_fft | 0.7.0 | MIT | Espressif Systems |
| espressif/esp-dl | 3.3.11 | MIT | (c) 2021 Espressif Systems (Shanghai) Co., Ltd. |
| espressif/esp-dsp | 1.8.0 | Apache-2.0 | Espressif Systems |
| espressif/esp-now | 2.5.3 | Apache-2.0 | Espressif Systems |
| espressif/esp-sr (including the speech models written to the `model` partition) | 2.5.4 | ESPRESSIF MIT License (use on Espressif products only) | (c) 2018 Espressif Systems (Shanghai) PTE LTD |
| espressif/esp_audio_codec | 2.6.2 | Espressif Modified MIT License (use exclusively with Espressif products) | (c) 2025 Espressif Systems (Shanghai) Co., Ltd. |
| espressif/esp_jpeg | 1.3.1 | Apache-2.0 | Espressif Systems |
| espressif/esp_new_jpeg | 1.0.2 | ESPRESSIF MIT License (use on Espressif products only) | (c) 2024 Espressif Systems (Shanghai) Co., Ltd. |
| espressif/esp_websocket_client | 1.8.0 | Apache-2.0 | Espressif Systems |
| espressif/esp32-camera | 2.1.7 | Apache-2.0 | Espressif Systems |
| espressif/led_strip | 3.0.3 | Apache-2.0 | Espressif Systems |
| espressif/mdns | 1.13.1 | Apache-2.0 | Espressif Systems |
| espressif/quirc | 1.2.0 | ISC | (c) 2010-2012 Daniel Beer |

## Voice data

### HTS voice "Mei": CC BY 3.0

- `assets/voices/mei.htsvoice`, bundled and served from GitHub Pages at `voices/mei.htsvoice`.
- HTS Voice "Mei" (c) 2009-2013 Nagoya Institute of Technology / MMDAgent Project Team.
- License: [Creative Commons Attribution 3.0](https://creativecommons.org/licenses/by/3.0/). Full text: [assets/voices/LICENSE_mei.txt](assets/voices/LICENSE_mei.txt). Upstream: <http://www.mmdagent.jp/>.
- Modified version: `assets/voices/mei16.htsvoice` is Mei downsampled offline to 16 kHz (band-limited mel-cepstrum conversion, Kenta IDA 2026). Distributed under CC BY 3.0 with this notice of modification.

The attribution strings are also stored per voice in [assets/voices.json](assets/voices.json) and shown in the settings page.

### Other CC BY voices (not included)

The firmware loads any `.htsvoice` file. These voices were used for testing and are not in this repository; follow their licenses when you use them:

- htsvoice-tohoku-f01 (female): CC BY 4.0, (c) Tohoku University Inui-Suzuki Laboratory (icn-lab)
- hts_voice_nitech_jp_atr503_m001 (male): CC BY 3.0, (c) Nagoya Institute of Technology

## GitHub Pages site (web flasher and settings pages)

| Component | Location | License | Copyright |
|---|---|---|---|
| esptool-js | `docs/esptool.js`, `tools/remote-flasher/web/esptool-bundle.js` | Apache-2.0 | (c) Espressif Systems |
| pako 2.1.0 (inside esptool-js) | same files | MIT AND Zlib | (c) 2014-2017 Vitaly Puzrin and Andrey Tupitsin |
| Emscripten runtime (inside `avatar_module.js` and `avatar.html`, built by CI with `emscripten/emsdk:3.1.74`) | Pages build output | MIT (Emscripten is dual-licensed MIT and University of Illinois/NCSA) | (c) Emscripten authors |

## Gateway (`gateway/`)

The Python gateway that runs on your computer (voice through Gemini Live, the "Hey Grok" wake word, the MCP server and the Grok Bot forwarding service). It is not part of the firmware image.

### Upstream

- Forked from [kisaragi-mochi/stackchan-mcp](https://github.com/kisaragi-mochi/stackchan-mcp), MIT License. The full license text with all copyright lines is kept in [gateway/LICENSE](gateway/LICENSE): (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.; (c) 2025 Project Contributors; (c) 2026 kisaragi-mochi; (c) 2026 Groki Bot contributors. New files added in this repository (`gateway/stackchan_mcp/gbot_brain.py`, `gbot_client.py`, `gbot_http_proxy.py` and their tests) are (c) 2026 Joseph Zhou under the same MIT License.
- The NOTE paragraph in `gateway/LICENSE` about GPL-3.0 SCServo_lib files refers to the upstream project's own `firmware/` directory. That directory and those files are not part of this repository; the Groki Bot firmware does not use SCServo_lib.
- The gateway speaks the [XiaoZhi](https://github.com/78/xiaozhi-esp32) WebSocket protocol (MIT). No XiaoZhi source code is included.

### Python dependencies (installed by `uv sync`, not included in this repository)

Versions are the ones pinned in [gateway/uv.lock](gateway/uv.lock) at release time.

| Package | Version | License | Used for |
|---|---|---|---|
| websockets | 16.0 | BSD-3-Clause | WebSocket server for the robot |
| aiohttp | 3.13.5 | Apache-2.0 AND MIT | HTTP server on port 8766 |
| mcp | 1.27.0 | MIT | stdio MCP server |
| pydantic | 2.13.3 | MIT | data models |
| python-dotenv | 1.2.2 | BSD-3-Clause | reading `.env` |
| pyserial | 3.5 | BSD-3-Clause | optional USB serial channel |
| edge-tts | 7.2.8 | LGPL-3.0 | optional Edge text-to-speech; used as an unmodified library |
| google-genai | 2.0.1 | Apache-2.0 | Gemini Live client (`gemini-live` extra) |
| sounddevice | 0.5.5 | MIT | local audio tools (`gemini-live` extra) |
| httpx | 0.28.1 | BSD-3-Clause | VOICEVOX client (`tts` extra) |
| opuslib | 3.0.1 | BSD-3-Clause | Opus audio encoding (`tts` extra) |
| numpy | 2.5.0 | BSD-3-Clause and bundled licenses | wake word audio (`wakeword` extra) |
| sherpa-onnx, sherpa-onnx-core | 1.13.2 | Apache-2.0 | wake word engine (`wakeword` extra) |

System library: [libopus](https://opus-codec.org/) (BSD-3-Clause), installed with Homebrew or apt. Wake word model: `sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20` from the [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) releases, downloaded by you as described in the gateway README and not included here. Transitive dependencies are listed in `gateway/uv.lock`.

### External tools (not included)

- `gbot` from the npm package [grok-bot-cli](https://github.com/ScriptedAlchemy/grok-bot-cli) (MIT), used by the Grok Bot forwarding service. You install it yourself; it talks to the Grok Bot app.
- The `claude` CLI, used only when the optional `ask_claude` tool or Mac control is on.

## Host-side tools only (not in the firmware)

- Open JTalk, hts_engine and pyopenjtalk (Modified BSD and others) are used by `tools/jvox/` on the host to generate and check voice data. The generated files follow the license of the source voice.
- `tools/stackchan-channel/` installs `@modelcontextprotocol/sdk` (MIT) with Bun when you use it; the package is not included here.
- `tools/face-demo/` scripts call the device's HTTP API with curl; `task-demo` also runs the `grok` CLI, which you install yourself and which is not included here.

See each component's LICENSE or COPYING file for the complete terms.
