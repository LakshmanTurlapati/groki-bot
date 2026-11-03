**English** | [简体中文](README.zh-CN.md)

# Groki Bot Gateway

A small Python service that runs on your computer and gives the
[Groki Bot](../README.md) desktop robot a voice brain. It lives in the
`gateway/` directory of the Groki Bot repository; how it fits together with
the firmware and Grok Bot is described in
[docs/architecture.md](../docs/architecture.md).

- **Voice conversation** through Google Gemini Live by default, or Grok login voice, with the robot's
  microphone and speaker.
- **"Hi Grok" wake word**, detected on your computer, so the gateway only
  forwards microphone audio to the selected voice backend after you call the robot.
- **An MCP server** so Claude Code, Claude Desktop, or any other MCP client
  can make the robot speak and read its status.
- **Face tracking** on a Mac with a camera: the robot turns its head to
  follow your face (see [Face tracking](#face-tracking)).
- Optional: voice control of your Mac, an `ask_claude` voice tool that
  hands hard questions to the Claude CLI (offered only when the `claude` CLI
  is installed), and a Gemini-only `ask_grokbot` voice tool that
  hands tasks to an agent in the Grok Bot app and reads its answers aloud
  (see [Optional: Grok Bot hand-off](#optional-grok-bot-hand-off)).

```text
Robot ← XiaoZhi WebSocket :8765 → Gateway ← MCP → Claude / MCP client
Gateway voice → Gemini Live (default) / Grok login voice
Grok voice → localhost:18771 login helper → native Grok Bot
Gemini ask_grokbot → optional localhost:18770 → gbot CLI → Grok Bot
```

## Do you need this?

The Groki Bot firmware can already talk on its own: flash it, enter your own
OpenAI or Gemini API key in its settings page, and it works without any
computer. Install this gateway if you want:

- the "Hi Grok" wake word,
- the head to follow your face (Mac with a camera),
- Claude (or another MCP client) to make the robot speak,
- voice commands that reach your computer (Mac control, `ask_claude`).

**What works with the Groki Bot firmware.** Voice conversation, the wake
word, face tracking, and the `speak` and `get_status` MCP tools. The Groki Bot
firmware's XiaoZhi client announces `features.mcp=false`, so MCP tools that
drive hardware (`move_head`, `set_led`, `set_avatar`, `take_photo` and similar)
return an error with that firmware. They work with firmware that implements
the XiaoZhi device MCP tools. Face tracking does not use MCP: it turns the
head with the XiaoZhi `head` message, which the firmware handles. The shared voice
device tools (`STACKCHAN_GEMINI_DEVICE_TOOLS=1`) do not need MCP either: with
this firmware `move_head` is sent as a `head` message (firmware newer than
v0.2.1), `set_avatar` as an emotion and `set_all_leds` as an `led` message.
Camera tools have no such path and do not work with this firmware.

## What you need

| | |
|---|---|
| Robot | M5Stack CoreS3 with the Stack-chan base, flashed with the [Groki Bot firmware](../README.md) |
| Computer | macOS (tested on Apple Silicon). Linux should work for voice; Mac control is macOS only. |
| Network | Robot and computer on the same Wi-Fi / LAN |
| Software | [uv](https://docs.astral.sh/uv/), Git, and the Opus audio library (`brew install opus` on macOS, `sudo apt install libopus0` on Debian/Ubuntu) |
| API key | A Google AI Studio key for Gemini Live: <https://aistudio.google.com/apikey> |

## 1. Flash the robot

Follow [path 1 in the Groki Bot README](../docs/voice-backends.md#path-1-firmware-only)
(web flasher in Chrome or Edge; you can skip the API key). Come back here once the robot boots and
joins your Wi-Fi.

## 2. Install the gateway

```bash
git clone https://github.com/JosssphZhou/groki-bot.git
cd groki-bot/gateway
uv sync --all-extras
cp .env.example .env
```

Open `.env`, choose a voice backend, and set a device token:

- Default Gemini: set `GEMINI_API_KEY`. Grok login voice uses the setup below instead and does not need this key.
- `STACKCHAN_TOKEN`: a random secret, for example from `openssl rand -hex 16`.
  Recommended; leave it empty only on a network you trust.

Note: the PyPI package named `stackchan-mcp` is the upstream project
([kisaragi-mochi/stackchan-mcp](https://github.com/kisaragi-mochi/stackchan-mcp))
and does not include the Gemini voice or wake word. Install from this
repository as shown above.

### Grok Bot login voice (macOS)

Gemini is the default backend. To use Grok voice, install and sign in to the Grok Bot desktop app on the same Mac, and make sure Node.js is installed. This integration was tested with Grok Bot 0.71.2 and uses internal APIs, so application updates, account permissions, and usage limits may affect availability. No separate xAI voice API key or firmware reflashing is needed.

After installing the gateway, configure the following in `gateway/.env` with your own agent name and ID:

```dotenv
STACKCHAN_VOICE_BACKEND=gbot
STACKCHAN_GBOT_VOICE_BOT_ID=YOUR_AGENT_ID
STACKCHAN_TOOL_BOT=assistant
STACKCHAN_TOOL_BOT_ID=YOUR_AGENT_ID
```

In a separate terminal, start the login helper from `gateway/` and keep it running:

```sh
STACKCHAN_GBOT_VOICE_BOT_ID=YOUR_AGENT_ID \
STACKCHAN_TOOL_BOT=assistant \
STACKCHAN_TOOL_BOT_ID=YOUR_AGENT_ID \
node scripts/gbot_voice_session_bridge.mjs
```

The Node helper does not load `.env` automatically; pass the same values as shown. It reads the existing macOS app login and Keychain, keeps credentials in memory, and binds only to `127.0.0.1:18771`. Next, start the Python gateway using one of the methods in step 3. `GEMINI_API_KEY` is not required for this backend. To switch back, set `STACKCHAN_VOICE_BACKEND=gemini` and restart the gateway with your existing Gemini key.

To keep the helper running after closing the terminal or restarting your Mac, install it as a user LaunchAgent from `gateway/` with the same agent ID:

```sh
python3 scripts/install_gbot_voice_launchd.py --agent-id YOUR_AGENT_ID --enable-desktop --enable-tools
```

It starts at login and restarts after exit. Add `--dry-run` to preview the plist, or `--replace` to back up and update an existing installation (interrupts current voice calls). Keep this checkout at its installed path. Restart the Python gateway after installation and verify a spoken reply on the robot. The LaunchAgent manages only the login helper; it does not start the Python gateway or repair upstream login or sandbox failures. `http://127.0.0.1:18771/health` confirms helper liveness only. Logs are in `~/Library/Logs/stackchan-gbot-voice.log`.

If this Mac needs a proxy to reach the voice service, add `--proxy-url http://127.0.0.1:PORT` to the installation command, including when using `--replace`. Use your existing proxy's port and Node.js 24 or later. The installer enables Node's environment proxy support and keeps loopback requests direct. It does not inherit proxy settings from your shell. Proxy URLs must not contain credentials.

The gateway checks for a fresh server acknowledgement on the active voice connection every 30 seconds without playing test audio. Failed initial connections, dropped or unresponsive connections, and stalled replies trigger automatic recovery. Retries back off from 1 second to at most 30 seconds. Recovery discards old audio and does not replay questions or desktop tasks. Expired logins and incompatible upstream APIs still require account attention or an application update.

`http://127.0.0.1:8766/debug/voice-health` reports the actual voice connection: HTTP 200 when connected, or HTTP 503 while connecting, recovering, or unavailable. `action_required` identifies login/account problems; `last_error` contains only a fixed error category. Use your configured gateway port if different.

From `gateway/`, run an isolated recovery check against real Grok audio. It simulates an unavailable login helper and closes only its own test connection, leaving robot calls alone:

```sh
PYTHONPATH=. python scripts/probe_gbot_voice_recovery.py --agent-id YOUR_AGENT_ID --output-dir /tmp/groki-recovery-check
```

With the explicit bot name and matching IDs, Grok directly uses native `send_task`, `search_conversations`, and `end_the_call`; it does not add a local `ask_grokbot` wrapper or use the CLI forwarding service. Shared local hardware and optional Mac tools retain their existing opt-in settings, including the legacy-named `STACKCHAN_GEMINI_DEVICE_TOOLS`. Conversation search is not web search. Unknown native tools are not enabled by the legacy `STACKCHAN_GBOT_VOICE_TOOLS` switch.

If the desktop sandbox is unavailable, audio stays usable while the native channel retries in the background. Task dispatching and conversation search are refused until presence and the event stream are ready. Acceptance means the task was accepted, not that a desktop message was visibly delivered. Typed-message subscriptions can still fail; they reconnect independently and do not replay messages missed during a disconnect. No claims of full feature parity or acoustic barge-in are made. See [validation](GBOT_VOICE_VALIDATION.md) and [latency methods](VOICE_LATENCY_BENCHMARK.md).

### Wake word model (about 33 MB)

```bash
mkdir -p models/kws && cd models/kws
curl -L -O https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20.tar.bz2
tar xf sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20.tar.bz2
rm sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20.tar.bz2
cd ../..
```

If the model or runtime is unavailable, the gateway stays connected but blocks
microphone audio from the voice backend. It reports `UNAVAILABLE` and retries
after 2, 4, 8, 16, then 30 seconds, continuing until recovery. Recovery requires
a fresh wake word; audio received during failure is discarded. Explicit
`STACKCHAN_WAKE_WORD=0` reports `DISABLED` and permits microphone passthrough.
This gate applies to the Gemini and Grok-login backends, not XiaoZhi cloud
passthrough. On-device WakeNet is not needed for gateway voice.

Check the model offline (macOS, uses the `say` command):

```bash
uv run --all-extras python scripts/kws_offline_repro.py --generate-say
# prints "hit: ..." or "miss: ..."
```

### Changing the wake word

The default wake word is "Hi Grok". The detector (sherpa-onnx keyword
spotting) matches a sequence of sounds, so any phrase works without
retraining the model. Set it in `.env` and restart the gateway:

- "Hey Groki" instead: `STACKCHAN_WAKE_PHRASE=hey groki` (built in, with the
  same vowel variants as "Hi Grok").
- Your own phrase: set `STACKCHAN_WAKE_PHRASE` to the phrase and
  `STACKCHAN_WAKE_KEYWORD` to its sounds, then `@` and the phrase. English
  words use CMU dictionary phonemes (ARPAbet, with stress digits); Chinese
  uses pinyin split into initial and final, with tone marks. Every sound must
  appear in the model's `tokens.txt`. Examples:

  ```bash
  STACKCHAN_WAKE_PHRASE=hello robot
  STACKCHAN_WAKE_KEYWORD=HH AH0 L OW1 R OW1 B AA2 T @hello_robot

  STACKCHAN_WAKE_PHRASE=小机器人
  STACKCHAN_WAKE_KEYWORD=x iǎo j ī q ì r én @小机器人
  ```

Check a new wake word before relying on it: `kws_offline_repro.py` reads the
same settings (`--voice` picks another macOS voice, `--wav` uses your own
recording). Two or three syllables with clear consonants work best; if it
triggers too rarely or too often, adjust `STACKCHAN_KWS_THRESHOLD` and
`STACKCHAN_KWS_SCORE`.

## 3. Start it and check

```bash
uv run --all-extras stackchan-mcp --check     # config and port check, exits right away
```

Then start the gateway in one of two ways.

**A. Let Claude Code start it (simplest).** The gateway runs while Claude Code
is open:

```bash
claude mcp add groki -- uv run --all-extras --directory "$(pwd)" stackchan-mcp
```

For Claude Desktop, add this to `claude_desktop_config.json` (use the
absolute path of your clone):

```json
{
  "mcpServers": {
    "groki": {
      "command": "uv",
      "args": ["run", "--all-extras", "--directory", "/absolute/path/to/groki-bot/gateway", "stackchan-mcp"]
    }
  }
}
```

**B. Run it all the time, as a background service.** Use this if you want to
talk to the robot without an MCP client open:

```bash
./scripts/run_stackchan_gateway_launchd.sh
```

The script keeps stdin open so the stdio MCP server does not exit. Point a
launchd agent (macOS) or systemd unit (Linux) at it for start at login.

Only one gateway can use ports 8765 and 8766 at a time, so pick A or B.

When it is running you should see `server listening on 0.0.0.0:8765` in the
log, and <http://127.0.0.1:8766/debug/status> returns JSON.

## 4. Point the robot at the gateway

In the Groki Bot BLE settings page
(<https://josssphzhou.github.io/groki-bot/settings.html>, desktop Chrome
or Edge), tab "对话" (Conversation):

1. Provider: "XiaoZhi 服务器" (XiaoZhi server).
2. "XiaoZhi 服务器 URL": `ws://<your computer's LAN IP>:8765`
   (find the IP with `ipconfig getifaddr en0` on macOS or `hostname -I` on Linux).
3. "XiaoZhi 令牌" (token): the same value as `STACKCHAN_TOKEN`, or empty if
   you left that empty.
4. "保存并重启" (Save and restart).

`/debug/status` should now show `"device": {"connected": true, ...}`. Say
"Hi Grok" and start talking (or "Hey Groki" if you set
`STACKCHAN_WAKE_PHRASE=hey groki`, see [Changing the wake word](#changing-the-wake-word)).

## USB voice (no Wi-Fi path to the gateway)

Use this when the robot is plugged into the computer over USB and cannot open
`ws://<LAN>:8765`. The same Opus frames and `hello` / `listen` / `tts` JSON
then ride the serial port that `STACKCHAN_USB_TRANSPORT` already uses for
`$MCP:` tool calls.

1. Flash a firmware that contains this USB voice link (CoreS3):

   ```bash
   make set-target BOARD=cores3   # first time for this board
   make build BOARD=cores3
   make flash BOARD=cores3 PORT=/dev/cu.usbmodem*
   ```

   Stop the gateway first. With `STACKCHAN_USB_TRANSPORT=1` it holds
   `/dev/cu.usbmodem*` and the flash will fail with "port busy".

2. In `gateway/.env`:

   ```bash
   STACKCHAN_USB_TRANSPORT=1
   STACKCHAN_USB_VOICE=1          # immediate; see the table below
   STACKCHAN_VOICE_BACKEND=gbot   # or gemini
   ```

3. The conversation provider on the robot stays "XiaoZhi 服务器". A saved Wi-Fi
   URL is optional for USB voice; when present, it is used when USB voice mode is off.
   With the cable connected and
   no WebSocket session, the gateway sends `voice/mode` and the robot answers
   with the usual hello, then streams 16 kHz Opus up and plays 24 kHz Opus down.

4. Proactive speech, from this computer only (no wake word):

   ```bash
   curl -s -X POST http://127.0.0.1:8766/say \
     -H 'Content-Type: application/json' \
     -d '{"text": "the build finished"}'
   ```

   `/say` accepts local callers and rejects cross-origin browser requests. It opens or reuses the
   voice session and calls the same `speak_text` path as a live Gemini or
   Grok session. A robot that is already connected over Wi-Fi keeps that
   session; `/say` does not move it to USB.

Limits:

- The serial pipe is full duplex. The conversation is half-duplex, the same
  as the Wi-Fi client: the microphone is off while the speaker plays, so the
  robot does not hear you during its own reply.
- Uplink is 24 kbit/s Opus, 60 ms frames (about 3 KiB/s). Downlink is 24 kHz
  Opus on the same 60 ms grid, usually a few hundred bytes a frame. USB
  Serial/JTAG ignores the 115200 baud setting in the gateway; that number is
  only what pyserial asks for. The controller runs at USB full-speed, which
  has room for these frames. A real 115200 UART would be tight if both
  directions were saturated, and this link is not one.
- `STACKCHAN_USB_VOICE=auto` (the default) waits `STACKCHAN_USB_VOICE_DEFER_S`
  seconds (default 8) before signaling, so a robot that can reach the gateway
  over Wi-Fi keeps the WebSocket. Set `1` when the cable is the only path.
  Set `0` to keep USB as tool calls only. `/say` does not wait out the defer.
  A healthy USB voice session does not periodically switch back to probe Wi-Fi;
  disable USB voice to return to the saved WebSocket connection.
- The gateway renews the USB voice lease every two seconds. If the cable is
  removed or the gateway stops responding, the lease expires after ten seconds
  and the firmware returns to its saved Wi-Fi WebSocket connection. An orderly
  gateway shutdown releases the lease immediately.

## Face tracking

The gateway follows faces seen by the **CoreS3 camera** by default. The robot
captures a 320×240 image; [the Swift detector](../tools/vision-tracker/README.md)
runs Apple Vision on the Mac. Frames stay in memory and are not uploaded to
the voice provider. The Mac webcam is not opened or requested.

### Setup

Use macOS 13 or later, Swift 5.9 or later, a CoreS3 with head servos, and firmware
advertising `camera_tracking_v1`. Older firmware remains usable for voice but
reports `unsupported_firmware` for robot-camera tracking.

Robot-camera tracking uses the Gemini or Grok-login gateway backend, which
provides conversation pause events. The separate `xiaozhi` cloud-passthrough
backend keeps voice behavior unchanged and reports `unsupported_voice_backend`
for robot tracking; its explicit Mac-camera mode remains available.

Build the detector once from the repository root:

```bash
cd tools/vision-tracker
swift build -c release
```

Start the gateway normally. With USB voice active, snapshots share the USB
link, so guest Wi-Fi isolation does not prevent tracking. Enable
`STACKCHAN_USB_TRANSPORT=1` and `STACKCHAN_USB_VOICE=1` for this path.
Otherwise the robot must reach the gateway WebSocket and the Mac must reach
the robot's HTTP settings service.

For Wi-Fi sessions, the gateway discovers the HTTP origin from the active bot connection. For a
routed setup, set `STACKCHAN_CAMERA_URL=http://<robot-address>` (origin only).
If the bot's HTTP settings have a password, set `STACKCHAN_CAMERA_PASSWORD` to
that password. This is separate from the gateway's `STACKCHAN_TOKEN`.
`VISION_URL` remains the photo-upload destination and is not used here.

Capture defaults to a four-frame-per-second ceiling. `STACKCHAN_CAMERA_FPS`
can reduce that rate or raise it to a maximum of eight; the effective rate
depends on acquisition, transfer, and motion checks. USB snapshots use quality-80 JPEG to
reduce transfer time. Each capture, detection, and correction finishes before
another cycle starts. Late results are discarded;
connection or detector failures retry with a delay, without switching cameras.

See [tracking research and measurements](../docs/robot-camera-tracking.md)
for the controller design, hardware results, and a live benchmark command.

### Behavior and diagnostics

Open <http://127.0.0.1:8766/debug/status> and inspect `face_tracking` for the
source, capture health, last frame/face time, pause reason, and last error.
A healthy image containing no face is different from a camera failure.
`capture_transport` identifies USB or HTTP; `frames_processed`, `faces_detected`,
and `last_face` show whether capture and detection are progressing.

Move in front of the robot camera: bounded angular corrections bring your face
toward the image center. A centered face holds the current head angle. The
firmware checks measured servo position and a single-use frame token before
moving, so an old detection cannot override a newer touch or head command.
The controller converts image offset to camera angle, tapers gain near center,
and keeps motor holding torque during active following. The detector keeps a
consistent face target across frames. Losing the face releases tracking;
ordinary idle motion resumes after the existing head hold expires.

Firmware advertising `camera_tracking_v2` also supports motion-aware USB
captures. Measured head positions bracket the retained image, and corrections
refer to its estimated capture heading. Excessive movement during capture is
rejected. Turns preserve velocity when a new target arrives, with bounded
speed and acceleration. An adaptive angle filter suppresses small detection
jitter and accounts for the camera's own rotation. Binary camera packets
avoid base64 overhead while sharing the CRC-protected voice stream.

`STACKCHAN_CAMERA_TRACKING_MOTION=0` selects settled captures;
`STACKCHAN_CAMERA_BINARY=0` selects legacy JSON/base64 camera chunks.
`STACKCHAN_CAMERA_JPEG_QUALITY` defaults to 80 and accepts 60-90. Older
firmware automatically uses its original capture and transfer behavior.

Automatic tracking and idle motion pause throughout the actual wake-word
conversation window and speech. Dormant microphone streaming still permits
tracking. Manual commands remain available. Corrections captured before a
pause or reconnect are discarded, not replayed after speech.

Say “look at me” or “stop looking at me” to toggle head following. Set
`STACKCHAN_HEAD_FOLLOW_DEFAULT=0` to start with following off, or
`STACKCHAN_FACE_TRACKER_AUTOSTART=0` to disable the detector. Robot capture
also stops while head following is off.

### Use a Mac webcam explicitly

Set `STACKCHAN_FACE_TRACKER_SOURCE=mac` to retain the previous webcam path.
Only this mode accepts detections posted to `/track` and requires macOS
camera permission. Set `STACKCHAN_FACE_TRACKER_CAMERA` to part of the camera
name; otherwise the detector prefers a Studio Display, then an iPhone, then
the first available camera.

To grant permission, run the built `groki-vision-tracker` from the terminal
that starts the gateway and allow the camera prompt. If needed, enable that
terminal under System Settings → Privacy & Security → Camera. A manually
started webcam helper requires `STACKCHAN_FACE_TRACKER_SOURCE=mac` on the
gateway and `STACKCHAN_FACE_TRACKER_AUTOSTART=0` to avoid a second helper:

```bash
tools/vision-tracker/.build/release/groki-vision-tracker --endpoint http://127.0.0.1:8766/track --fps 8
```

## Optional: Grok Bot hand-off

The instructions below are the **optional CLI hand-off for Gemini voice**. Grok login voice uses native tools instead; see [Grok Bot login voice](#grok-bot-login-voice-macos). It does not run the port 18770 forwarding service.

With this on, you can say "Hi Grok, look up tomorrow's weather in Tokyo" or
"research X for me": Gemini acknowledges the accepted task, hands the task to an agent in the Grok Bot app, and reads the
agent's replies aloud as they come in. Small talk stays with Gemini. The full
message flow is in [docs/architecture.md](../docs/architecture.md#4-grok-bot-hand-off).

It is off unless you configure it. You need, on the same computer as the
gateway:

- The Grok Bot app, installed and signed in, with an agent (or group) that
  should receive tasks.
- The `gbot` command line tool from
  [grok-bot-cli](https://github.com/ScriptedAlchemy/grok-bot-cli) (needs a
  recent Node.js, see its README): `npm install --global grok-bot-cli`, then
  check with `gbot bots list`.
  `gbot` uses the app's session; the gateway never sees a Grok token.

Steps:

1. Pick the agent name from `gbot bots list` (or create one, for example
   `gbot bots create --name assistant`) and add it to `.env`:
   `STACKCHAN_TOOL_BOT=assistant`. This is what turns the feature on.
2. Start the forwarding service in a second terminal (or as a second
   background service) and leave it running:

   ```bash
   uv run --all-extras stackchan-gbot-proxy
   curl -s http://127.0.0.1:18770/health     # {"ok": true, "service": "gbot-http", ...}
   ```

3. Restart the gateway. Ask the robot to look something up.

Test the forwarding service without the robot:

```bash
curl -s -X POST http://127.0.0.1:18770/send \
  -H 'Content-Type: application/json' \
  -d '{"text": "Reply with one short sentence: hello", "target": "assistant"}'
```

## Configuration reference

All settings live in `.env` (read once at start; restart the gateway, or
reconnect the MCP client, after a change).

| Variable | Default | Meaning |
|---|---|---|
| `GEMINI_API_KEY` | none | Required only for the Gemini backend, not Grok login voice |
| `STACKCHAN_GEMINI_VOICE` | `Kore` | Gemini voice name |
| `STACKCHAN_GEMINI_MODEL` | `gemini-3.8-live` | Gemini Live model (set `gemini-3.1-flash-live-preview` to go back) |
| `STACKCHAN_TOKEN` | empty | Shared secret with the robot; also protects `/capture` and `/debug/inject-text` |
| `HOST`, `WS_PORT`, `CAPTURE_PORT` | `0.0.0.0`, `8765`, `8766` | Listen address and ports |
| `STACKCHAN_WAKE_WORD` | on | `0` disables the wake word gate |
| `STACKCHAN_KWS_SCORE` | `7` | Higher makes the wake word easier to trigger |
| `STACKCHAN_KWS_THRESHOLD` | `0.05` | Lower makes the wake word more sensitive |
| `STACKCHAN_KWS_MODEL_DIR` | `./models/kws` | Wake word model location |
| `STACKCHAN_WAKE_PHRASE`, `STACKCHAN_WAKE_KEYWORD` | `hi grok` | Wake word; see [Changing the wake word](#changing-the-wake-word). `STACKCHAN_WAKE_PHRASE=hey groki` alone switches to "Hey Groki" |
| `STACKCHAN_WAKE_IDLE_S` | `30` | Silence before the listening window closes |
| `STACKCHAN_MAC_CONTROL` | off | `1` lets voice commands control this Mac (see Safety) |
| `STACKCHAN_FACE_TRACKER_SOURCE` | `robot` | CoreS3 camera; `mac` explicitly selects a webcam |
| `STACKCHAN_CAMERA_FPS` | `4` | Robot capture rate ceiling, greater than zero and at most 8 Hz; effective rate includes servo settling |
| `STACKCHAN_CAMERA_USB_FORMAT` | `jpeg` | USB camera encoding; `rgb565be` retains uncompressed diagnostics |
| `STACKCHAN_TRACKING_HFOV`, `STACKCHAN_TRACKING_VFOV` | `60`, `46.8` | Estimated horizontal/vertical lens angles in degrees; configurable after measured calibration |
| `STACKCHAN_TRACKING_GAIN` | `0.9` | Angular correction gain, 0.1–1; reduced automatically near center |
| `STACKCHAN_CAMERA_URL` | connection peer | Optional robot HTTP(S) origin; no path or credentials |
| `STACKCHAN_CAMERA_PASSWORD` | empty | Robot HTTP settings password, separate from the gateway token |
| `STACKCHAN_FACE_TRACKER_AUTOSTART` | on | `0` disables the face detector |
| `STACKCHAN_FACE_TRACKER_BIN` | `tools/vision-tracker/.build/release/groki-vision-tracker` | Face tracker executable |
| `STACKCHAN_FACE_TRACKER_CAMERA` | empty | Mac source only: part of the webcam name |
| `STACKCHAN_HEAD_FOLLOW_DEFAULT` | on | `0` starts with head follow off; "look at me" turns it on |
| `STACKCHAN_GEMINI_DEVICE_TOOLS` | off | `1` gives both voice backends face/LED/head tools. Firmware with device MCP gets them as MCP calls; the Groki Bot firmware gets them as XiaoZhi emotion, `led` and `head` messages (`head` needs firmware newer than v0.2.1) |
| `STACKCHAN_USB_TRANSPORT` | off | `1` enables the USB serial channel (tool calls, and voice when USB voice mode is on); it locks `/dev/cu.usbmodem*` |
| `STACKCHAN_USB_VOICE` | `auto` | `1` carries Opus and tts/listen JSON over USB as soon as the cable is up. `auto` does that only when no WebSocket voice session appears within `STACKCHAN_USB_VOICE_DEFER_S` (default 8). `0` leaves USB as tool calls only |
| `STACKCHAN_USB_VOICE_DEFER_S` | `8` | Seconds `auto` waits before opening USB voice, so a working Wi-Fi session can connect first |
| `STACKCHAN_ASK_CLAUDE` | on when the `claude` CLI is found | `0` removes the `ask_claude` voice tool even when the CLI is installed |
| `STACKCHAN_CLAUDE_BIN` | `claude` on PATH | Claude CLI used by `ask_claude` |
| `STACKCHAN_CLAUDE_MODEL` | `claude-sonnet-5` | Model passed to the Claude CLI by `ask_claude` and Mac tasks |
| `STACKCHAN_VOICE_BACKEND` | `gemini` | `gbot` uses Grok login voice; `xiaozhi` forwards to XiaoZhi cloud |
| `STACKCHAN_GBOT_VOICE_BOT_ID` | none | Grok voice agent ID; helper and gateway must agree |
| `STACKCHAN_GBOT_VOICE_URL` | `http://127.0.0.1:18771` | Loopback HTTP login helper only |
| `STACKCHAN_GBOT_VOICE_LANGUAGE` | `zh` | Input transcription language, e.g. `en` or `auto` |
| `STACKCHAN_PERSONALITY_FILE` | `personality.md` in this directory, if present | Extra persona text appended to Gemini's instructions. The built-in persona is a short one named Groki that replies in the user's language; text in this file overrides it where they differ (name, language, style) |
| `STACKCHAN_TOOL_BOT` | empty (off) | Agent name; enables native Grok desktop access or Gemini `ask_grokbot` |
| `STACKCHAN_TOOL_BOT_ID` | empty | Required for native Grok; must match `STACKCHAN_GBOT_VOICE_BOT_ID`. Optional for Gemini |
| `STACKCHAN_TOOL_BOT_PREFIX` | built-in read-aloud hint | Text put in front of every task; set it empty to send tasks as is (Gemini CLI path only) |
| `STACKCHAN_GBOT_URL` | `http://127.0.0.1:18770` | Forwarding service address (Gemini CLI path only) |
| `STACKCHAN_ASK_TIMEOUT` | `110` | Seconds to wait for the agent's replies (Gemini CLI path only) |
| `STACKCHAN_GBOT_BIN` | `gbot` on PATH | Forwarding service: path to `gbot` (Gemini CLI path only) |
| `STACKCHAN_GBOT_HTTP_PORT` | `18770` | Forwarding service port (it always binds 127.0.0.1) (Gemini CLI path only) |

## Safety

- **Mac control is off by default.** With `STACKCHAN_MAC_CONTROL=1`, anyone
  within earshot of the robot can open apps and URLs, change volume, take
  screenshots, run Shortcuts, and start `claude -p` tasks that may edit files
  in the gateway directory. The voice prompt asks for spoken confirmation
  before destructive actions, but that is a model instruction, not a hard
  permission check.
- **Set `STACKCHAN_TOKEN`.** Without it, any device on your network can
  connect as the robot, and the `/debug/inject-text` endpoint accepts text
  from anyone who can reach port 8766.
- **The Grok Bot forwarding service binds 127.0.0.1 only.** Any program on
  this computer that can reach port 18770 can message your Grok Bot agents as
  you. Do not expose it to the network.
- **Keep keys out of Git.** `.env` is ignored by `.gitignore`. Do not paste
  keys into scripts, test files or issues.

## Troubleshooting

| Symptom | Check |
|---|---|
| Robot never connects | Same network? Firewall allows incoming 8765? URL starts with `ws://`, not `wss://`, and uses the computer's LAN IP. Log line `ESP32 auth rejected` means the token does not match. |
| Connects but never answers | `GEMINI_API_KEY` set? `/debug/status` → `gemini.last_error`. An error containing `reported as leaked` means Google disabled the key: create a new one. |
| Gemini connection times out | The computer must be able to reach Google's Gemini API. In regions where Google is blocked, run the gateway behind a working network proxy. |
| Wake word never triggers | Model downloaded to `models/kws`? Log shows `KWS ready`? Try `STACKCHAN_KWS_THRESHOLD=0.03`, or run `scripts/kws_offline_repro.py`. `STACKCHAN_WAKE_WORD=0` turns the gate off. |
| `sherpa-onnx unavailable` / `Library not loaded: libonnxruntime` | Run `uv sync --all-extras` again; it installs `sherpa-onnx-core`, which ships the runtime. |
| `Could not find Opus library` | Install Opus (`brew install opus` or `apt install libopus0`). |
| Head does not follow faces | Check `/debug/status` → `face_tracking`: `unsupported_firmware` needs tracking-capable firmware; `detector_missing` needs a Swift build; `auth_required` needs the robot HTTP password; capture failures need Mac→bot connectivity. `no_face` means the camera works but sees no face. `head_follow: false`: say "look at me". Mac webcam mode additionally requires camera permission. |
| Flashing fails with "port busy" | Stop the gateway if you enabled `STACKCHAN_USB_TRANSPORT`. |
| USB cable is in, but no voice | Provider must be XiaoZhi. `STACKCHAN_USB_TRANSPORT=1`. `STACKCHAN_USB_VOICE=1` if you do not want to wait for the Wi-Fi defer. `/debug/status` → `device.transport` is `usb`. `/say` from another machine returns `local only`. |
| `.env` change has no effect | Restart the gateway; with setup A, reconnect the MCP server (`/mcp` in Claude Code). |
| Gemini hand-off says the task could not be sent | Is `stackchan-gbot-proxy` running (`curl http://127.0.0.1:18770/health`)? Does `gbot bots list` work in a terminal and list the name in `STACKCHAN_TOOL_BOT`? Is the Grok Bot app signed in? |

## MCP tools

| Tool | With Groki Bot firmware |
|---|---|
| `get_status` | Works: gateway and robot connection state |
| `speak(text)` | Works: the gateway synthesizes speech and plays it on the robot |
| `say(text, voice?)` | Needs a separately installed VOICEVOX engine |
| `set_voice_mode` | Advanced: routes transcribed speech into a terminal session; off by default |
| `get_device_info`, `set_volume`, `set_brightness`, `move_head`, `get_head_angles`, `set_avatar`, `set_mouth`, `set_mouth_sequence`, `set_blink`, `set_led`, `set_leds`, `set_all_leds`, `clear_leds`, `set_background_color`, `get_touch_state`, `take_photo`, `install_avatar_assets`, `check_vm_en`, `gpio_test`, `uart_diag` | Need firmware with XiaoZhi device MCP tools |

## Development

```bash
uv sync --all-extras
NO_PROXY="*" uv run --all-extras pytest -q
```

## Credits and license

This gateway is a fork of
[kisaragi-mochi/stackchan-mcp](https://github.com/kisaragi-mochi/stackchan-mcp),
which grew out of the [stack-chan](https://github.com/mongonta0716/stack-chan)
community. It speaks the [XiaoZhi](https://github.com/78/xiaozhi-esp32)
WebSocket protocol and uses [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx)
for the wake word.

MIT License, see [LICENSE](LICENSE). The rest of the Groki Bot repository
(the firmware) is under its own license; see [../LICENSE](../LICENSE) and
[../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). The `gbot` CLI
([grok-bot-cli](https://github.com/ScriptedAlchemy/grok-bot-cli), MIT) is an
external tool and is not included.
