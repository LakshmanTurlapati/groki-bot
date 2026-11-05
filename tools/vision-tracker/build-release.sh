#!/bin/sh
# Build the Apple Vision face tracker the gateway launches.
# This must run on the Mac that talks to the robot (macOS 13+, Swift 5.9+).
# The binary is not stored in git. Firmware does not need this script.
set -eu
cd "$(dirname "$0")"
if [ "$(uname -s)" != "Darwin" ]; then
    echo "groki-vision-tracker builds only on macOS (Apple Vision)." >&2
    echo "On the Mac: cd tools/vision-tracker && ./build-release.sh" >&2
    exit 1
fi
if ! command -v swift >/dev/null 2>&1; then
    echo "swift not found. Install Xcode or the Command Line Tools: xcode-select --install" >&2
    exit 1
fi
swift build -c release
echo "Built $(pwd)/.build/release/groki-vision-tracker"
echo "The gateway starts that path on its own. Override with STACKCHAN_FACE_TRACKER_BIN."
