[English](xiaozhi_led_control.md) · 中文

# XiaoZhi LED 控制消息

XiaoZhi WebSocket 的文本控制帧用 `led` 类型表示语音状态灯。空闲时灯灭；听用户说话时为暗白色；Groki 说话时为白色脉动。

## 消息格式

```json
{"type":"led","mode":"speech"}
{"type":"led","mode":"listen","r":40,"g":40,"b":40}
{"type":"led","mode":"off"}
```

- `mode` 为 `off`、`listen` 或 `speech`。`speech` 在设备上以白色脉动，固件有播放音量时跟着嘴型走。
- 带 `mode` 时 `r`/`g`/`b` 可省略。`listen` 用它们做纯色。只有 `r`/`g`/`b` 且全为 0 的旧消息仍表示关闭。其它旧颜色会被忽略，避免残留的蓝色盖掉说话时的白灯，或在空闲时把灯点亮。
- 消息只改运行状态，不写 NVS。空闲、睡眠、断开和结束对话时灯是灭的。
- 硬件输出仍只在 `main/led_task.cpp`。
