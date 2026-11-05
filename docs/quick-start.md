[English](quick-start.md) | [中文](quick-start.zh-CN.md) | [日本語](quick-start.ja.md)

# Quick start: talk through Grok Bot (macOS)

What you need: a Groki Bot (CoreS3 with a Stack-chan base), a USB-C cable, desktop Chrome or Edge, a Mac on the same Wi-Fi signed in to the Grok Bot desktop app (tested with version 0.71.2; uses internal APIs, so app updates may break it), Node.js, and [uv](https://docs.astral.sh/uv/).

1. Flash the firmware and set up Wi-Fi: steps 1 and 2 of [path 1](voice-backends.md#path-1-firmware-only). You can skip the API key.
2. Install the gateway on the Mac: `git clone` this repository, `cd groki-bot/gateway`, `uv sync --all-extras`, `cp .env.example .env`.
3. Set the four values in `gateway/.env` under [Configure Grok Bot native voice](#configure-grok-bot-native-voice).
4. In a second terminal, start the login helper using the full command in [Configure Grok Bot native voice](#configure-grok-bot-native-voice) and keep it running.
5. Start the gateway as in [path 2](voice-backends.md#path-2-firmware--gateway), point the robot at `ws://<your computer's LAN IP>:8765` in the BLE settings page, and say "Hey Grok".

Gemini stays the default backend; set `STACKCHAN_VOICE_BACKEND=gemini` to switch back. Other voice options (OpenAI Realtime, Gemini Live, Grok Bot hand-off through the `gbot` CLI) are below under [Other voice backends](voice-backends.md#other-voice-backends).

### Configure Grok Bot native voice

Gemini remains the default voice backend and does not switch to Grok automatically; select Grok Bot native voice yourself in `gateway/.env`.

Native voice supports macOS only and uses internal Grok Bot desktop APIs tested with version 0.71.2, so app updates may break it; if it stops working or you want to switch back, set `STACKCHAN_VOICE_BACKEND=gemini` and restart the gateway with your Gemini API key still configured, without reflashing firmware.

Add these four lines to `gateway/.env`, replacing the example name and ID with your own:

```dotenv
STACKCHAN_VOICE_BACKEND=gbot
STACKCHAN_GBOT_VOICE_BOT_ID=YOUR_AGENT_ID
STACKCHAN_TOOL_BOT=assistant
STACKCHAN_TOOL_BOT_ID=YOUR_AGENT_ID
```

- `STACKCHAN_VOICE_BACKEND=gbot` selects the Grok voice backend.
- `STACKCHAN_GBOT_VOICE_BOT_ID` is the agent ID for the voice session.
- `STACKCHAN_TOOL_BOT` is the name you give this agent; the code only checks that it is nonempty and does not look up the agent by name in the app.
- `STACKCHAN_TOOL_BOT_ID` is the agent ID for desktop tasks and must match the voice agent ID character for character, without automatic case or whitespace normalization.

Use your own agent's name and that same agent's ID. If you have the optional `gbot` CLI, look them up with:

```sh
gbot bots list
gbot bots list --json
gbot bots get "<name or ID>"
```

These commands read the local Grok Bot agent directory. If the directory cannot be found, they report an error and suggest specifying it with `--dir` or `GROK_BOT_AGENTS_DIR` (observed with gbot 0.11.3 on 2026-10-18). `--json` requests JSON output; the help does not document its columns or fields. If you cannot obtain the ID, stop here and confirm it in Grok Bot or your agent configuration. The CLI is not required to run native voice once you have the ID.

Sign in to the Grok Bot desktop app on the same Mac. In a second terminal, run this complete command from `gateway/`, with the same name and IDs, and leave the terminal running:

```sh
STACKCHAN_GBOT_VOICE_BOT_ID=YOUR_AGENT_ID \
STACKCHAN_TOOL_BOT=assistant \
STACKCHAN_TOOL_BOT_ID=YOUR_AGENT_ID \
node scripts/gbot_voice_session_bridge.mjs
```

The login helper does not read `.env`; pass these variables explicitly. It listens only on `127.0.0.1:18771`.

Optional: `STACKCHAN_GBOT_VOICE_LANGUAGE` defaults to `zh`; set it to `en` or `auto` in `gateway/.env` if needed. See [gateway setup](../gateway/README.md#grok-bot-login-voice-macos) for advanced settings.

After starting the gateway, check its startup log for `voice_backend=gbot`. In another terminal, check the helper:

```sh
curl http://127.0.0.1:18771/health
```

The helper returns `"ok": true` when running. Together these checks show the backend selection and helper configuration, not that login or voice works. Confirm the full path by saying "Hey Grok" to the robot and hearing a reply.

To return to Gemini, set `STACKCHAN_VOICE_BACKEND=gemini` or remove that line, keep a valid `GEMINI_API_KEY`, and restart the gateway. If another launch command or service environment also sets `gbot`, change that setting too. The independent login helper does not exit automatically; stop it in its terminal when no longer needed.

If Grok voice fails to start, the gateway does not automatically fall back to Gemini: the robot may appear online without voice, and the log includes `Grok voice startup failed`; switch back using the instructions above.

### Grok Bot login voice

The gateway also supports `STACKCHAN_VOICE_BACKEND=gbot`: use the signed-in Grok Bot app on macOS for voice interaction without a separate xAI voice API key. Gemini remains the default and can be selected again with `STACKCHAN_VOICE_BACKEND=gemini`. Switching gateway backends does not require reflashing firmware.

Grok voice directly uses native `send_task`, `search_conversations`, and `end_the_call`, alongside shared local hardware and optional Mac tools. It does not require the `gbot` CLI; the CLI forwarding service remains an optional path for Gemini. See [gateway setup](../gateway/README.md#grok-bot-login-voice-macos).

Real task acceptance, delivery of completion results to the voice model, and model-generated output audio have been observed in testing. The spoken words have been confirmed to play correctly on the physical speaker (2026-10-18). Desktop typed-message subscriptions can still fail; they reconnect separately and do not replay messages missed during a disconnect. CoreS3 firmware may stop microphone capture while speaking, so pure acoustic interruption is not established. See [validation limits](../gateway/GBOT_VOICE_VALIDATION.md).
