[English](features.md) | [中文](features.zh-CN.md) | [日本語](features.ja.md)

# Features

![Eye-ring compositing pipeline: 18 eye rings blended with four weight sets, squashed vertically, then written to screen coordinates](images/expression-engine.svg)

![AI voice conversation: the microphone connects to one of three services over WebSocket; replies drive audio, mouth shape, expression and servos](images/voice-flow.svg)

- **Expression engine**: aora eye-ring contours (48 points per eye), 15 expressions, blink, ring cycling and four-weight blending. The body layer keeps breathing, happy bounce, squash while speaking, shy floating hearts and blush.
- **AI voice conversation**: WebSocket connection to OpenAI Realtime, Google Gemini Live or a XiaoZhi server. Microphone audio goes up; reply audio drives the mouth. The half-duplex CoreS3 mutes the microphone while speaking; tap the screen or touch the head to interrupt a reply (barge-in).
- **Servo head motion**: SCS0009 yaw and pitch with a trapezoidal velocity profile.
- **Face tracking** (with the gateway on a Mac): the robot camera supplies frames over USB or Wi-Fi, Apple Vision finds your face, and the robot turns its head to follow it. Following pauses during conversation. Setup: [gateway README, Face tracking](../gateway/README.md#face-tracking).
- **Head touch**: Si12T three-zone capacitive touch (front, middle, back); stroking switches to the shy face (Affection).
- **Speech balloon**: reply text on a rounded white panel at the bottom of the screen; long text scrolls as a marquee.
- **Three setup paths**: BLE (NimBLE GATT), Wi-Fi STA (mDNS HTTP), SoftAP with a captive portal (iOS friendly).
- **OTA**: dual partitions with boot verification and rollback. Update over BLE in chunks, by local upload over Wi-Fi, or by letting the device fetch the matching board firmware from GitHub Pages.
