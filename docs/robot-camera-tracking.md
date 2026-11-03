# Robot-camera tracking tuning

The CoreS3 camera supplies frames to a local Apple Vision worker on the Mac.
USB sessions use JPEG snapshots; detection results control measured pan/tilt
angles through single-use frame tokens. Camera images remain in memory.
Firmware with `camera_tracking_v2` supports captures during automatic turns
and compact binary image transfer. Older firmware retains settled captures.

## Research and implementation choices

Espressif's [ESP_NEW_JPEG documentation](https://developer.espressif.com/blog/2025/09/esp-new-jpeg-introduction/)
describes an ESP32-S3 SIMD encoder supporting RGB565 and configurable quality.
The implementation uses the pinned component already in this repository,
quality 80, aligned input, and a 64 KiB output bound. Compression retains the
320×240 resolution while reducing USB traffic. Encoding stays inside the
camera/render exclusion to avoid competing framebuffer reads from PSRAM.

The [ViSP pan/tilt visual-servoing tutorial](https://github.com/lagadic/visp/blob/master/doc/tutorial/visual-servo/tutorial-flir-ptu-vs.dox)
uses image error to center a target. Here a small pinhole-camera controller
converts normalized face displacement into angular corrections. Gain tapers
near center; a 1.2% image deadzone suppresses noise. Corrections remain bounded
to 15° yaw and 10° pitch and the device's calibrated mechanical limits.
The 60°/46.8° lens estimates are configurable assumptions, not a measured
CoreS3 lens calibration. Simulations test convergence with 45°–80° horizontal
fields of view.

Apple's [real-time face-tracking sample](https://developer.apple.com/documentation/vision/tracking-the-user-s-face-in-real-time)
describes detection and tracking across frames. On this system, measured face
detection cost was much smaller than acquisition and transport. The worker
therefore redetects each frame and associates boxes by overlap, distance, and
size continuity. It prefers an existing spatial target over a different face
with higher confidence, with bounded reacquisition after misses. This is
spatial association, not face recognition.

The gateway's motion-aware controller filters absolute target bearings,
combining measured camera heading with the face's angular offset. This avoids
mistaking the robot's own turn for movement of the person. Its original
implementation of the [One Euro filter](https://gery.casiez.net/publications/CHI2012-casiez.pdf)
uses the actual sample interval and adapts smoothing to target speed. Center
hysteresis further reduces small reversals. Filters reset on face loss,
session changes, and gaps over 1.2 seconds; no target motion is extrapolated
after observations stop.

In v2, the configured gain applies directly outside hysteresis. The legacy
deadzone subtraction and extra gain taper are omitted because they can make
small persistent errors too weak to move the servo. Plant-model tests with
a 2° movement threshold verify lower residuals than the legacy controller
across 45°–80° lens assumptions; this is not a physical servo calibration.

V2 brackets the retained frame with servo feedback and uses their midpoint
as its estimated capture heading. Frames with more than 3° of bracketed
movement are rejected. Feedback during automatic tracking must agree with
the current motion model within 4.5°. Tokens expire after 1.2 seconds and
retain manual/touch/conversation invalidation. Corrections reference capture
heading rather than adding an old image error to the latest pose.

Firmware tracking uses a bounded motion profile and holds motor torque
between frames. V2 retargets preserve commanded position and velocity,
limiting yaw/pitch to 35°/s and 28°/s and acceleration to 140°/s² and 110°/s².
Ordinary head motion retains spring interpolation. Integration subdivides delayed control ticks into steps of
at most 20 ms; regression tests verify speeds 200/900/1000 with 20–50 ms
ticks converge without overshoot and retain mechanical bounds. It still
requires measured servo feedback for acquisition. Legacy captures require
settling; v2 permits verified automatic tracking motion. Touch, manual
commands, and conversation continue to supersede pending captures.
Tracking capture synchronizes with the renderer instead of waiting a fixed
150 ms. The single-framebuffer driver needs one discarded image before a fresh
tracking image. The CoreS3 uses its documented [240 MHz CPU clock](https://docs.m5stack.com/en/core/CoreS3).

The shared USB lease uses signed elapsed-time comparisons: a concurrent
renewal can be newer than a camera task's sampled clock. Treating that case
as unsigned elapsed time incorrectly discarded valid captures. Regression
checks exercise the ordering, lease expiration, and clock wrap. Host
heartbeats define the active session; transient low-level USB SOF-presence
samples no longer abort snapshots mid-transfer. Espressif documents that its
[USB presence API](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_driver_usb_serial_jtag/include/driver/usb_serial_jtag.h)
checks SOF traffic, independently of an open serial session. On this robot,
transfer diagnostics showed discarded and partial snapshots while the
gateway session stayed connected. The lease expires after
10 seconds without host renewal. Writes also allow a bounded 500 ms host
scheduling stall, and captures cancelled by a
pause or replaced connection do not trigger failure backoff.

V2 camera chunks use CRC-protected binary type-4 packets with a little-endian
request id and offset followed by up to 2048 JPEG bytes. This removes base64
expansion and per-chunk JSON work. The host uses native CRC computation and
reads available serial bytes immediately, avoiding the timeout from waiting
to fill a 512-byte read. Resolution remains 320×240 and JPEG quality stays 80.

## Recorded hardware validation on 2026-11-03

Before the continuous-motion upgrade, the tuned JPEG build completed a
120.06-second reference run:

| Measurement | Result |
| --- | --- |
| Completed frames / face detections | 156 / 140 |
| Complete update rate, including head settling | 1.30 per second |
| Median capture and USB transfer | 412.5 ms |
| Median Vision detection | 14.9 ms |
| Camera errors / USB reconnects | 0 / 0 |
| Observed median face-center offset | 3.67% of width / 2.53% of height |

The uncompressed baseline reported a median 1,058.5 ms capture-and-transfer
time. The installed version reduces that by 61%, about 2.6× faster. The
last JPEG payload was 15,348 bytes versus 153,600 uncompressed bytes.
Voice stayed connected throughout the run. Face-center measurements apply
to detected faces in a live scene; target motion was not controlled, so
these are observations rather than a calibrated accuracy benchmark. The
complete update rate includes servo settling and varies with movement.

Automated validation passed 989 gateway tests (two skipped), 11 Swift tests,
and 37 firmware motion/camera/head/conversation host scenarios with address and
undefined-behavior sanitizers. The CoreS3 firmware build passed and was
flashed with saved settings retained. A short live speech check sent 43 audio
frames and confirmed capture paused during speech and resumed afterward.

Automated checks cover malformed JPEG transfers, worker framing, target
continuity, control convergence, mechanical limits, stale frame rejection,
touch/manual/conversation preemption, camera/render exclusion, and encoding
under exclusion. Run live throughput checks from the repository root:

```sh
python3 gateway/scripts/benchmark_robot_tracking.py --duration 20
```

`face_tracking.frame_metadata` reports payload size, capture time, encoding
time, and measured capture pose. `capture_latency_ms` includes the complete
acquisition and USB transfer; `detection_ms` is the subsequent Vision step.
`STACKCHAN_CAMERA_FPS=4` is a ceiling. Actual updates also wait for the sensor
and head to settle, so it is not a claim of four corrections per second.

## Verification after USB/session review

The combined source passed 1,007 gateway tests (one optional test skipped),
13 Swift tests and a release build, and 48 firmware host tests with address
and undefined-behavior sanitizers plus five speech-synthesis vowel checks.
CoreS3 and camera-disabled StopWatch firmware builds passed with ESP-IDF
5.5.4. CoreS3 retains 167,465 bytes of static D/IRAM for the heap, above the
118,784-byte build floor.

Additional regressions cover stale queued USB writes, heartbeat renewal during
startup, single proxy shutdown, detector failure and explicit disabling over
USB, conversation pauses, centered-axis holds during motion, and incremental
parsing of long partial console lines. These review changes have not been
flashed; the earlier live measurements do not validate this exact revision.
