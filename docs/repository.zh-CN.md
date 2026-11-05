[English](repository.md) | [中文](repository.zh-CN.md) | [日本語](repository.ja.md)

# Groki Bot

Groki Bot 桌面机器人（M5Stack CoreS3 加 Stack-chan 底座）的全部内容都在这一个仓库里：机器人**固件**、运行在电脑上的**网关**，以及让机器人把任务交给 **Grok Bot** 应用里智能体的联动机制。 Groki 现已支持 Grok bot 后端原生语音，连接桌面端应用即可直接和机器人对话。在已登录 Grok Bot 的 Mac 上，语音直接走现有登录状态：无需额外配置 xAI 语音 API 密钥，也无需重刷固件。

## 本仓库说明

| 目录 | 内容 |
|---|---|
| `main/`、`components/`、`assets/` | 机器人固件（ESP-IDF，C++20） |
| [gateway/](../gateway/README.zh-CN.md) | 运行在电脑上的网关（Python）：Gemini / Grok 登录态语音、唤醒词、MCP 与原生桌面通道 |
| [docs/architecture.zh-CN.md](architecture.zh-CN.md) | 固件、网关和 Grok Bot 之间怎么通信 |
| `docs/` | 刷机页、许可页等文档，发布到 GitHub Pages |
| [tools/vision-tracker/](../tools/vision-tracker/README.md) | Mac 端人脸追踪程序（Swift，Apple Vision），人脸追踪时由网关启动 |
| `tools/` | 设置页、Avatar DSL 编译器、[tools/stackchan-channel/](../tools/stackchan-channel/README.md)（通过固件 HTTP 接口给 Claude Code 用的 MCP 服务）、[tools/face-demo/](../tools/face-demo/README.md)（表情演示脚本）等电脑端工具 |

表情采用 Grok Bot 胶囊设计（[docs/design/grokbot-faces/](design/grokbot-faces/)）：灰色胶囊身体、胶囊形眼洞、没有嘴（说话时身体压扁），包含 9 个 Grok Bot 状态（待机、聆听、思考、工作、等待、受阻、完成、失败、睡眠）以及本机心情表情。`main/pill_face.hpp` 每帧计算几何与时序，`assets/grok_face.avdsl` 负责绘制。

## 授权

本仓库自有源码（`components/board`、`components/scs_servo`、`components/avatar`、`components/avatar_vm`、`components/groki_motion`、`components/jtts`、`components/conversation`、`components/config_service`、`components/wifi_config_service`、`components/telegram`、`main`、`tools`）在 **Boost Software License 1.0**（[LICENSE](../LICENSE)）下分发。

**网关（MIT）**：[gateway/](../gateway) 派生自 [kisaragi-mochi/stackchan-mcp](https://github.com/kisaragi-mochi/stackchan-mcp)，继续使用 MIT 许可证，上游版权行保留在 [gateway/LICENSE](../gateway/LICENSE)。它的 Python 依赖列在 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md#gateway-gateway)。`gbot` 命令行（[grok-bot-cli](https://github.com/ScriptedAlchemy/grok-bot-cli)，MIT）是外部工具，不包含在本仓库。

Submodule（`components/M5GFX` / `components/M5Unified` / `components/tl_expected/expected`）与 managed_components（`espressif/esp_audio_codec` / `espressif/esp_websocket_client` / `espressif/mdns` / `espressif/esp_jpeg` / `espressif/esp32-camera` 等）遵循各自上游许可证。

HMM 语音合成使用的 **hts_engine API**（Modified BSD / 名古屋工业大学·东京工业大学）与同捆 **HMM 语音 "Mei"**（CC BY 3.0 / 名古屋工业大学·MMDAgent Project Team）等第三方归属，同样汇总在 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。HTML 版：<https://josssphzhou.github.io/groki-bot/licenses.html>。
