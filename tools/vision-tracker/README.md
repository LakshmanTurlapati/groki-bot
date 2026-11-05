# Groki Vision Tracker

Apple Vision face detection for Groki Bot. By default, the gateway captures
the robot's camera and supplies frames to this executable through stdin.
This mode does not access a Mac camera or request camera permission. Frames
stay in memory and are never saved. The gateway controls capture timing,
authentication, conversation pauses, and guarded head movement.

Set `STACKCHAN_FACE_TRACKER_SOURCE=mac` in the gateway to explicitly use the
legacy Mac webcam path. A failed robot camera never selects a Mac camera
automatically. This older path posts face positions to `/track`; the gateway
accepts those positions for motion only in `mac` mode.

Setup, camera permission and how to check it works:
[gateway README, Face tracking](../../gateway/README.md#face-tracking).

## Build

Needs macOS 13 or later and Swift 5.9 or later (Xcode or the Command Line
Tools: `xcode-select --install`).

```bash
cd tools/vision-tracker
./build-release.sh
```

`build-release.sh` is the same `swift build -c release`. It only runs on macOS. The binary is not in git; a checkout on the Mac that runs the gateway has to build it once. A missing binary logs `face tracker unavailable` / `detector_missing` and the head does not follow.

The executable is `tools/vision-tracker/.build/release/groki-vision-tracker`.
The gateway starts it from there on its own.

Tests (Swift Testing, needs a Swift 6 toolchain):

```bash
swift test
```

With only the Command Line Tools installed, some toolchains cannot find the
Swift Testing macros (`plugin for module 'TestingMacros' not found`). Point
the compiler at them:

```bash
swift test -Xswiftc -plugin-path -Xswiftc /Library/Developer/CommandLineTools/usr/lib/swift/host/plugins/testing
```

## Run by hand

The gateway starts the robot frame worker with `--stdin-frames`. Each input
record has an eight-byte header: a little-endian UInt32 encoding (`1` for
RGB565, `2` for JPEG), then a little-endian UInt32 payload length. Both
encodings must describe a 320×240 image. RGB565 payloads contain exactly
153600 row-major pixel bytes, with the high byte first; JPEG payloads are
bounded to 65536 bytes. The legacy `--stdin-rgb565` option accepts consecutive
raw frames without headers. Each complete frame produces one JSON line on
stdout:

```json
{"face":{"x":0.5,"y":0.5,"width":0.2,"height":0.3,"confidence":0.9}}
{"face":null}
```

Coordinates use Apple's normalized Vision rectangle convention: the origin
is at the bottom left. A clean stdin EOF exits successfully; a truncated
frame or detection failure exits with code 1. Diagnostics go to stderr.
The worker detects faces in every frame and associates a target using box
overlap, distance, and size continuity. It initially selects the largest
confident face and retains its spatial target across brief misses. It does
not identify people. The gateway supplies one frame at a time and waits for
its result before capturing another. Device addresses and passwords are
never passed to the worker.

Research, controller tuning, and measured hardware performance:
[robot-camera tracking notes](../../docs/robot-camera-tracking.md).

To run the legacy Mac camera path by hand:

```bash
.build/release/groki-vision-tracker --endpoint http://127.0.0.1:8766/track --fps 8
```

| Argument | Default | Meaning |
|---|---|---|
| `--endpoint` | `http://127.0.0.1:8766/track` | Where detections are posted |
| `--fps` | `8` | At most this many detections per second |
| `--camera` | none | Use the first camera whose name contains this text (case-insensitive) |

Without `--camera` the tracker picks a Studio Display camera first, then an
iPhone (Continuity Camera), then the first camera macOS lists. It prints the
cameras it found and the one it uses to stderr:

```
Vision tracker cameras found: FaceTime HD Camera, Alice's iPhone Camera
Vision tracker camera: Alice's iPhone Camera
```

Exit code 2 means no usable camera: none found, none matching `--camera`, or
camera access not granted.

## Live view (optional)

`live-view/server.py` is a small debugging page. Stop the gateway from
starting the tracker (`STACKCHAN_FACE_TRACKER_AUTOSTART=0` in
`gateway/.env`) and explicitly select `STACKCHAN_FACE_TRACKER_SOURCE=mac`
if the preview should control the head, then:

```bash
cd gateway
uv run python ../tools/vision-tracker/live-view/server.py     # http://127.0.0.1:8787
../tools/vision-tracker/.build/release/groki-vision-tracker --endpoint http://127.0.0.1:8787/track
```

The page shows the browser's own camera preview with the reported face box,
and forwards every detection to the gateway (`STACKCHAN_GATEWAY_TRACK_URL`,
default `http://127.0.0.1:8766/track`), so the head keeps following. Its
sliders are shared between open live-view pages only; they do not change the
gateway's tracking settings.
