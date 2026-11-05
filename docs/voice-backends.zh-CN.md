[English](voice-backends.md) | [中文](voice-backends.zh-CN.md) | [日本語](voice-backends.ja.md)

# 其他语音后端

每种用法都在前一种的基础上增加，先从用法 1 开始。

| | 能做什么 | 需要什么 |
|---|---|---|
| **1. 只刷固件**（先走这条） | 用你自己的密钥，通过 OpenAI Realtime 或 Google Gemini Live 和机器人语音对话 | 机器人、USB-C 线、桌面版 Chrome 或 Edge、一把 OpenAI 或 Gemini API 密钥 |
| **2. 固件加网关** | 电脑处理语音与「Hey Grok」唤醒词，可选 Gemini Live 或 Grok 登录态语音；Mac 摄像头人脸跟随与 MCP | 已有固件、同一 Wi-Fi 的电脑，以及所选后端的配置；见 [gateway/](../gateway/README.zh-CN.md) |
| **3. 再加 Grok Bot** | 将任务交给桌面智能体并接收完成通知 | 同机登录 Grok Bot；Grok 语音走原生通道，Gemini 可选 CLI 转发 |

各部分怎么通信（端口、协议、消息流程、每部分需要什么）：[架构与通信说明](architecture.zh-CN.md)。

```text
机器人 → Gateway → Gemini Live (default) / Grok login voice
Claude / MCP → Gateway
Grok voice → localhost:18771 login helper → native Grok Bot
Gemini ask_grokbot → optional localhost:18770 → gbot CLI → Grok Bot
```

### 用法 1：只刷固件

1. **刷机。** 用桌面版 Chrome 或 Edge 打开刷机页 <https://josssphzhou.github.io/groki-bot/>，USB-C 连上机器人，点 Connect 选串口，选最新版本和 `CoreS3` 板子，点 Flash。想把机器人恢复成新机状态就勾上「Erase flash before write」。页面连不上时，先让板子进入下载模式（见[下载模式](troubleshooting.zh-CN.md#下载模式)）。
2. **首次配置。** 打开蓝牙设置页 <https://josssphzhou.github.io/groki-bot/settings.html>（Web Bluetooth，仅桌面版 Chrome / Edge），点「BLE 连接」。
   - 「连接」标签页：填 Wi-Fi 名称和密码。机器人只支持 2.4 GHz Wi-Fi。
   - 「对话」标签页：选对话服务商（「OpenAI Realtime」或「Google Gemini Live」），填对应的 API 密钥。
   - 点「保存并重启」。
3. **开始对话。** 重启后机器人连上 Wi-Fi 开始聆听，直接对它说话即可。

默认服务商是 OpenAI。**不填密钥时，机器人能连上 Wi-Fi，但不会回答。** 密钥不编进固件，保存在设备的 NVS 里。

### 用法 2：固件加网关

想让机器人的语音经过电脑处理时走这条，例如使用带唤醒词的 Gemini Live 或 Grok 登录态语音，或者让 Claude 连接机器人。

1. 先完成用法 1 的第 1、2 步（可以不填 API 密钥）。
2. 在和机器人同一个 Wi-Fi 的电脑上，按 [gateway/README.zh-CN.md](../gateway/README.zh-CN.md) 安装并启动网关：`git clone` 本仓库，`cd groki-bot/gateway`，`uv sync --all-extras`，在 `.env` 选择默认 Gemini（填 API 密钥）或 Grok 登录态后端（见下节），再用 Claude Code 启动（`claude mcp add groki -- uv run --all-extras --directory "$(pwd)" stackchan-mcp`）或作为后台服务运行。
3. 在蓝牙设置页的「对话」标签页：
   - 对话服务商：选「XiaoZhi 服务器」。
   - 「XiaoZhi 服务器 URL」：填 `ws://<电脑的局域网 IP>:8765`，例如 `ws://192.168.1.20:8765`。
   - 「XiaoZhi 令牌」：网关设置了 `STACKCHAN_TOKEN` 时填同一个值；网关没设令牌就留空。
   - 点「保存并重启」。
4. 在电脑上打开 <http://127.0.0.1:8766/debug/status>，看到 `"device": {"connected": true}` 后说「Hey Grok」。想改用「Hey Groki」做唤醒词，在 `gateway/.env` 里设 `STACKCHAN_WAKE_PHRASE=hey groki`，再重启网关。

用法 2 配合本固件能做什么、不能做什么：网关负责语音对话；在带摄像头的 Mac 上，网关还会让机器人转头跟着你的脸（见[人脸追踪](../gateway/README.zh-CN.md#人脸追踪)）。Claude 通过网关的 MCP 工具控制头部、灯和摄像头**到不了**本固件，因为本固件的 XiaoZhi 客户端在握手消息里声明 `features.mcp=false`（[components/conversation/xiaozhi_client.cpp](../components/conversation/xiaozhi_client.cpp)），也不处理服务器发来的 MCP 请求。人脸追踪改用 XiaoZhi 的 `head` 消息转头，固件能处理这条消息。两个网关语音后端共用的头部、表情、灯光工具（默认关闭，设 `STACKCHAN_GEMINI_DEVICE_TOOLS=1` 打开）也走这条路：`move_head` 以 `head` 消息发送（需要 v0.2.1 之后的固件），表情以表情消息发送，灯光颜色以 `led` 消息发送。摄像头类工具在本固件上不能用。

### 用法 3：再加 Grok Bot

Grok 登录态语音直接使用原生桌面通道，按下节配置即可，不需要 `gbot` 命令行。如果继续使用 Gemini，仍可通过 `ask_grokbot` 和可选 CLI 转发服务交给桌面智能体处理，见[Gemini 联动配置](../gateway/README.zh-CN.md#可选把任务交给-grok-bot)。

### 让 Claude Code 直接改表情

和网关无关，固件自带一套受令牌保护的 HTTP 接口（`/mcp/*`）。[tools/stackchan-channel/](../tools/stackchan-channel/README.md) 把它包装成 Claude Code 可用的 MCP 服务，提供 `say`、`set_expression`、`set_balloon`、`get_state` 四个工具。令牌在蓝牙设置页「维护」标签页的「Claude Code Channel (MCP)」一节设置。注意：该目录的说明是日文；`say` 走设备上的日语语音引擎，只能读平假名。

[使用 Grok Bot 登录态语音](quick-start.zh-CN.md#使用-grok-bot-登录态语音)
