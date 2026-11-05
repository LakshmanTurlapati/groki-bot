English · [中文](xiaozhi_led_control.zh-CN.md)

# XiaoZhi LED control message

XiaoZhi WebSocket text control frames have an `led` type that switches the base LEDs to one solid colour, used as a status light.

## Message format

```json
{"type":"led","mode":"speech"}
{"type":"led","mode":"listen","r":40,"g":40,"b":40}
{"type":"led","mode":"off"}
```

- `mode` is `off`, `listen`, or `speech`. `speech` pulses white on the device and follows the speaker envelope when the firmware has one.
- `r`, `g` and `b` are optional with `mode`. For `listen` they are the solid colour. A legacy frame with only `r`/`g`/`b` all zero still means off. Any other legacy colour is ignored, so a stale blue write cannot replace the speech pulse or light an idle robot.
- The message changes runtime state only and is not written to NVS. Idle, sleep, disconnect, and a closed chat leave the strip off.
- The hardware output is still done in one place, `main/led_task.cpp`.
