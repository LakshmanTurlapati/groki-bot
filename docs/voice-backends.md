[English](voice-backends.md) | [中文](voice-backends.zh-CN.md) | [日本語](voice-backends.ja.md)

# Other voice backends

Each path adds to the one before it. Start with path 1.

| | What you get | What you need |
|---|---|---|
| **1. Firmware only** (start here) | Voice conversation with the robot through OpenAI Realtime or Google Gemini Live, using your own API key | The robot, a USB-C cable, desktop Chrome or Edge, an OpenAI or Gemini API key |
| **2. + gateway** | Computer-side voice and "Hi Grok" wake word, with Gemini Live or Grok login voice; optional robot-camera face tracking and MCP | Compatible firmware, a computer connected over USB or Wi-Fi, and the chosen backend configuration; see [gateway/](../gateway/README.md) |
| **3. + Grok Bot** | Hand tasks to a desktop agent and receive completion updates | Grok Bot signed in on that computer; native transport for Grok voice, optional CLI forwarding for Gemini |

How the pieces talk to each other (ports, protocol, message flow, what each part needs): [docs/architecture.md](architecture.md).

```text
Robot → Gateway → Gemini Live (default) / Grok login voice
Claude / MCP → Gateway
Grok voice → localhost:18771 login helper → native Grok Bot
Gemini ask_grokbot → optional localhost:18770 → gbot CLI → Grok Bot
```

### Path 1: firmware only

1. **Flash.** Open the web flasher at <https://josssphzhou.github.io/groki-bot/> in desktop Chrome or Edge, connect the robot over USB-C, click Connect and pick the serial port, choose the latest release and the `CoreS3` board, then click Flash. Tick "Erase flash before write" if you want a clean device. If the page cannot connect, put the board in download mode (see [Download mode](troubleshooting.md#download-mode)).
2. **First-time setup.** Open the BLE settings page at <https://josssphzhou.github.io/groki-bot/settings.html> (Web Bluetooth, desktop Chrome or Edge only) and click "BLE 连接" (Connect over BLE).
   - Tab "连接" (Connection): enter your Wi-Fi SSID and password. The robot only supports 2.4 GHz Wi-Fi.
   - Tab "对话" (Conversation): pick the provider ("OpenAI Realtime" or "Google Gemini Live") and paste the matching API key.
   - Click "保存并重启" (Save and restart).
3. **Talk.** After the reboot the robot joins Wi-Fi and starts listening. Just speak to it.

The default provider is OpenAI. **Without an API key the robot connects to Wi-Fi but never answers.** Keys are not built into the firmware; they are stored in the device's NVS.

### Path 2: firmware + gateway

Use this if you want the robot's voice to run through your computer, for example to use Gemini Live with a wake word, or to connect Claude to the robot.

1. Do steps 1 and 2 of path 1 (you can skip the API key).
2. Install and start the gateway on a computer on the same Wi-Fi, following [gateway/README.md](../gateway/README.md): `git clone` this repository, `cd groki-bot/gateway`, `uv sync --all-extras`, choose Gemini (with an API key) or Grok login voice in `.env` (see below), then start it with Claude Code (`claude mcp add groki -- uv run --all-extras --directory "$(pwd)" stackchan-mcp`) or as a background service.
3. In the BLE settings page, tab "对话" (Conversation):
   - Provider: "XiaoZhi 服务器" (XiaoZhi server).
   - "XiaoZhi 服务器 URL" (XiaoZhi server URL): `ws://<your computer's LAN IP>:8765`, for example `ws://192.168.1.20:8765`.
   - "XiaoZhi 令牌" (XiaoZhi token): if the gateway has `STACKCHAN_TOKEN` set, enter the same value here. If the gateway has no token, leave this empty.
   - Click "保存并重启" (Save and restart).
4. Check <http://127.0.0.1:8766/debug/status> on the computer: `"device": {"connected": true}`. Say "Hi Grok". To use "Hey Groki" as the wake word instead, set `STACKCHAN_WAKE_PHRASE=hey groki` in `gateway/.env` and restart the gateway.

If the robot is plugged in over USB and cannot reach the computer over Wi-Fi, turn on `STACKCHAN_USB_TRANSPORT=1` and `STACKCHAN_USB_VOICE=1`. The gateway then carries the same Opus audio and `tts` / `listen` events over the serial port. `POST http://127.0.0.1:8766/say` speaks without the wake word. Details and flash steps: [gateway README, USB voice](../gateway/README.md#usb-voice-no-wi-fi-path-to-the-gateway).

With path 2, the gateway handles voice and can follow faces using the CoreS3 camera over USB or Wi-Fi, with detection running on the Mac ([Face tracking](../gateway/README.md#face-tracking)). Tracking uses the XiaoZhi `head_tracking` and frame-bound `head_track` messages. General camera and device MCP tools are separate: this firmware announces `features.mcp=false` and does not handle gateway MCP requests. Shared gateway voice head, face and LED tools (`STACKCHAN_GEMINI_DEVICE_TOOLS=1`) use XiaoZhi `head`, emotion, and `led` messages.

### Path 3: firmware + gateway + Grok Bot

Grok login voice uses the native desktop channel described below; it does not need the `gbot` CLI. If you keep Gemini voice, the existing optional `ask_grokbot` and CLI forwarding service remain available. See the [Gemini hand-off setup](../gateway/README.md#optional-grok-bot-hand-off).

For an agent named `assistant`, set `STACKCHAN_TOOL_BOT=assistant` in the gateway's `.env` and follow the gateway setup for the remaining settings.

### Letting Claude Code change the face directly

Separate from the gateway, the firmware has its own token-protected HTTP API (`/mcp/*`). [tools/stackchan-channel/](../tools/stackchan-channel/README.md) wraps it as an MCP server for Claude Code, with the tools `say`, `set_expression`, `set_balloon` and `get_state`. Set the token in the BLE settings page (tab "维护", Maintenance, section "Claude Code Channel (MCP)"). Notes: that README is in Japanese, and `say` uses the on-device Japanese speech engine, so it only reads hiragana.

[Grok Bot login voice](quick-start.md#grok-bot-login-voice)
