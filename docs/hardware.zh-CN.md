[English](hardware.md) | [中文](hardware.zh-CN.md) | [日本語](hardware.ja.md)

# 硬件规格



### 标准机：CoreS3 + Stack-chan 底座

![CoreS3 与 Stack-chan 底座的 I²C 设备、屏幕与 UART1 双舵机连接](images/hardware.zh-CN.svg)

| 项目 | 规格 |
|---|---|
| SoC | ESP32-S3R8 |
| PSRAM | 8 MB，封装内 Quad SPI，时钟 80 MHz |
| Flash | 16 MB 外置 SPI（W25Q128 封装） |
| 显示 | 320×240 IPS 触摸屏 |
| 舵机 | SCS0009 ×2（偏航 / 俯仰） |
| 舵机总线 | UART1，TX GPIO 6 / RX GPIO 7，1 Mbps，8N1 |
| 舵机 ID / 零位 | 偏航 ID 1、零位 460；俯仰 ID 2、零位 620 |
| 步进 | 1 step ≈ 0.3125°（`deg = (raw - zero) * 5 / 16`） |
| 软限位默认 | 偏航 ±40°，俯仰 -10° 到 +25° |
| 头顶触摸 | Si12T，I²C 0x68，前 / 中 / 后三区 |
| 电池计 | INA226，I²C 0x41 |
| IO 扩展 | PY32，I²C 0x6F；Pin 0 控制舵机 VM 电源（开启后等待 200 ms 再访问总线） |
| PMIC | AXP2101，I²C 0x34（由 M5Unified 管理） |
| LCD 触摸 | I²C 0x38（由 M5Unified 管理） |

Takao Base 使用同一份 `cores3` 固件：舵机走 Port A（TX GPIO 2 / RX GPIO 1，半双工、回声消除），无舵机电源控制、无 INA226。

### 其他支持板卡

| 板卡 | 构建代号 | 显示 | 要点 |
|---|---|---|---|
| CoreS3 + Stack-chan 底座 | `cores3` | 320×240 IPS + 触摸 | 默认。舵机两轴、头顶触摸、INA226 |
| CoreS3 + Takao Base | `cores3` | 同上 | Port A 半双工舵机；无舵机电源 / 电池计 |
| AtomS3R + Atomic ECHO BASE | `atoms3r` | 128×128 LCD | 无舵机。8 MB Octal PSRAM，8 MB Flash，ES8311，BtnA 切换 UI / AP |
| AtomS3（无 PSRAM）+ ECHO BASE | `atoms3` | 128×128 LCD | 轻量配置。无对话 / 无 BLE 音频 / 无 RTP |
| M5 StopWatch (C152) | `stopwatch` | 466×466 圆形 AMOLED + 触摸 | 无舵机。8 MB Octal PSRAM，16 MB Flash，触摸视线跟随，ES8311 |

板卡在启动时检测，经 `set_board_kind()` 反映到 UI 与功能开关。构建：`make build BOARD=<代号>`（默认 `cores3`）。不同板卡的 PSRAM 模式在链接期锁定，固件不能混刷。

## 技术规格

| 项目 | 规格 |
|---|---|
| 框架 / 语言 | ESP-IDF 5.5（按 5.5.4 验证，5.4.2 也可编译）/ C++20 |
| 目标芯片 | `esp32s3` |
| 表情渲染 | M5GFX。有 PSRAM 时全屏缓冲；单帧绘制预算 17 ms，全屏覆盖层按 33 ms 周期让出 CPU |
| 表情 | 20 种：Neutral、Happy、Sad、Angry、Doubt、Sleepy、Listening、Thinking、Excited、Curious、Confused、Surprised、Dizzy、Affection、Bored，以及 Grok Bot 运行状态 Working、Waiting、Blocked、Done、Failed |
| 胶囊表情合成 | `main/pill_face.hpp` 回放设计稿的关键帧（CSS cubic-bezier 缓动、嵌套变换），每帧最多输出 28 个胶囊线段；按 `expression` / `expression_from` / 两层 hold 共四组权重在 300 ms 内混合；每帧最多 45 个绘制图元 |
| Avatar DSL | `.avdsl` 源编译为 `.avbc` 字节码，经 BLE / Wi-Fi 热更换。出厂默认脸为 `assets/grok_face.avdsl` |
| 口型同步 | 16 kHz 单声道，256 点 radix-2 FFT；语音带 log 能量加 spectral flux；EWMA 噪声地板跟随环境 |
| 舵机运动 | 梯形速度曲线 `PathGenerator`，仅在驱动时使能扭矩；限位写入 NVS |
| 音量 | 0 到 200%，BLE / Wi-Fi / 机身 UI 实时调节 |
| OTA | 双槽加启动回滚。CoreS3 / StopWatch 每槽 4 MiB（`partitions_16mb.csv`）；AtomS3R 每槽 0x350000（`partitions_8mb.csv`）；AtomS3 每槽 3 MiB（`partitions.csv`） |
| BLE | NimBLE，Just Works 配对；应用层 X25519 + AES-256-GCM；可选口令 |
| Wi-Fi 设置 | 连接后 HTTP 80 端口提供 `settings_wifi.html`，mDNS `stackchan-XXXXXX.local` |
| SoftAP | SSID `Stackchan-XXXXXX` 加 WPA2；LCD 显示 Wi-Fi 二维码；DNS 劫持加 HTTP 404 兜底打开 captive portal |
