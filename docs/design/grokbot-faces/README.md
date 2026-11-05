<!--
SPDX-FileCopyrightText: 2026 Joseph Zhou
SPDX-License-Identifier: BSL-1.0
-->

# Grok Bot face design (reference)

The design the firmware face follows. `spec.html` has every measured number
(geometry, timing, triggers, head poses); the other pages are 320 x 240 CSS
mocks of each state. Open `index.html` in a browser to see them animate.

`main/pill_face.hpp` transcribes these `@keyframes` into keyframe tracks and
replays them through the same transform chain, and `assets/grok_face.avdsl`
draws the result. The host tool `face_preview` (components/avatar/test/host)
renders firmware frames at a given animation time so they can be compared
with these mocks side by side.

Head-servo poses in the spec are not implemented yet.
