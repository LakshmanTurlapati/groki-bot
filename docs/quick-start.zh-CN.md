[English](quick-start.md) | [中文](quick-start.zh-CN.md) | [日本語](quick-start.ja.md)

# 快速开始：用 Grok Bot 对话（仅 macOS）

需要：一台 Groki Bot（CoreS3 加 Stack-chan 底座）、USB-C 线、桌面版 Chrome 或 Edge、同一 Wi-Fi 下已登录 Grok Bot 应用的 Mac（测试版本 0.71.2，用的是内部接口，应用升级后可能失效）、Node.js 和 [uv](https://docs.astral.sh/uv/)。

1. 刷固件并配好 Wi-Fi：照[用法 1](voice-backends.zh-CN.md#用法-1只刷固件)的第 1、2 步做，API 密钥可以不填。
2. 在 Mac 上装网关：`git clone` 本仓库，`cd groki-bot/gateway`，`uv sync --all-extras`，`cp .env.example .env`。
3. 按[配置 Grok Bot 原生语音](#配置-grok-bot-原生语音)一节，在 `gateway/.env` 中设置四项配置。
4. 另开一个终端，按[配置 Grok Bot 原生语音](#配置-grok-bot-原生语音)一节的完整命令启动登录桥，并保持运行。
5. 按[用法 2](voice-backends.zh-CN.md#用法-2固件加网关)启动网关，在蓝牙设置页把机器人指向 `ws://<电脑局域网 IP>:8765`，然后说"Hey Grok"。

默认后端仍是 Gemini，把 `STACKCHAN_VOICE_BACKEND` 设回 `gemini` 即可切回。其他语音方式（OpenAI Realtime、Gemini Live、用 `gbot` 命令行转交任务）在下面的[其他语音后端](voice-backends.zh-CN.md#其他语音后端)一节。

### 配置 Grok Bot 原生语音

代码默认的语音后端仍是 Gemini，不会自动用 Grok，要用 Grok Bot 原生语音，需要你自己在 `gateway/.env` 里切换。

原生语音只支持 macOS，走 Grok Bot 桌面应用的内部接口，测试版本是 0.71.2，应用升级后可能失效；失效或不想用时，把 `STACKCHAN_VOICE_BACKEND` 改成 `gemini` 并重启网关，不用重刷固件，前提是 Gemini 密钥还在。

在 `gateway/.env` 中加入下面四行，把示例名字和 ID 换成你自己的：

```dotenv
STACKCHAN_VOICE_BACKEND=gbot
STACKCHAN_GBOT_VOICE_BOT_ID=YOUR_AGENT_ID
STACKCHAN_TOOL_BOT=助手
STACKCHAN_TOOL_BOT_ID=YOUR_AGENT_ID
```

- `STACKCHAN_VOICE_BACKEND=gbot` 选择 Grok 语音后端。
- `STACKCHAN_GBOT_VOICE_BOT_ID` 是语音会话使用的 agent ID。
- `STACKCHAN_TOOL_BOT` 是你给这个 agent 起的名字，代码只检查它非空，不会去应用里按名字查找。
- `STACKCHAN_TOOL_BOT_ID` 是桌面任务使用的 agent ID，必须与语音使用的 ID 逐字符一致，不会自动归一化大小写和空白。

名字是你自己的 agent 的名字，ID 是同一个 agent 的 ID。装有可选的 `gbot` 命令行时，可以这样查找：

```sh
gbot bots list
gbot bots list --json
gbot bots get "<名字或 ID>"
```

这些命令读取本机的 Grok Bot agent 目录。找不到目录时会报错，并提示用 `--dir` 或环境变量 `GROK_BOT_AGENTS_DIR` 指定目录（2026-10-18 在 gbot 0.11.3 上实测）。`--json` 输出 JSON，帮助里没有写输出列名或字段。拿不到 ID 时，先别继续，去 Grok Bot 应用或你的 agent 配置里确认 ID。拿到 ID 后，运行原生语音本身不需要这个命令行工具。

在同一台 Mac 上登录 Grok Bot 桌面应用。另开一个终端，在 `gateway/` 下运行完整命令，传入相同名字和 ID，并保持这个终端不关：

```sh
STACKCHAN_GBOT_VOICE_BOT_ID=YOUR_AGENT_ID \
STACKCHAN_TOOL_BOT=助手 \
STACKCHAN_TOOL_BOT_ID=YOUR_AGENT_ID \
node scripts/gbot_voice_session_bridge.mjs
```

登录桥不读 `.env`，需要显式传入这三个环境变量，只监听 `127.0.0.1:18771`。

可选：`STACKCHAN_GBOT_VOICE_LANGUAGE` 默认是 `zh`，需要时可在 `gateway/.env` 设为 `en` 或 `auto`。其他高级选项见[网关说明](../gateway/README.zh-CN.md#grok-bot-登录态语音macos)。

启动网关后，在启动日志里确认有 `voice_backend=gbot`，再从另一个终端检查登录桥：

```sh
curl http://127.0.0.1:18771/health
```

登录桥运行时会返回 `"ok": true`。这两项只确认后端选择和登录桥配置，不证明登录有效或语音已通。真正的确认是对机器人说 "Hey Grok"，听到回应。

要切回 Gemini，把 `STACKCHAN_VOICE_BACKEND` 改成 `gemini`（或删掉这一行），保留有效的 `GEMINI_API_KEY`，再重启网关。若网关由其他方式启动、环境里另外注入了 `gbot`，要在那里一起改。独立运行的登录桥不会自动退出，不用时在它的终端里结束。

Grok 语音启动失败时，网关不会自动回退到 Gemini，机器人可能显示在线但没有语音，日志里有 `Grok voice startup failed`，这时按上一段切回 Gemini。

### 使用 Grok Bot 登录态语音

网关新增了 `STACKCHAN_VOICE_BACKEND=gbot`：可在 macOS 上直接复用已登录的 Grok Bot 桌面应用，无需另配 xAI 语音 API 密钥。默认后端仍是 Gemini，设回 `STACKCHAN_VOICE_BACKEND=gemini` 即可恢复。切换网关后端不需要重新刷固件。

Grok 语音直接使用原生 `send_task`、`search_conversations` 与 `end_the_call`，并复用本地硬件与可选的 Mac 工具，不需要 `gbot` 命令行。CLI 转发服务保留作为 Gemini 的可选联动路径。安装步骤见[网关说明](../gateway/README.zh-CN.md#grok-bot-登录态语音macos)。

现场测试已观察到任务受理、完成结果送入语音模型及生成输出音频，实体扬声器播报的具体念词已确认正常（2026-10-18）。桌面文字订阅仍可能报错，会自动独立重连；断开期间遗漏的消息不会补发。CoreS3 固件在发声时可能停止麦克风采集，因此目前尚未验证纯声学打断。详见[验证范围](../gateway/GBOT_VOICE_VALIDATION.md)。
