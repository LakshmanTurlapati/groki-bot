#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
"""Measure live tracking throughput without storing camera images."""

import argparse
import json
import statistics
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=float, default=20)
    parser.add_argument("--url", default="http://127.0.0.1:8766/debug/status")
    args = parser.parse_args()
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def snapshot():
        with opener.open(args.url, timeout=3) as response:
            return json.load(response)["face_tracking"]

    first = snapshot()
    previous_count = first["frames_processed"]
    observations = []
    started = time.monotonic()
    while time.monotonic() - started < args.duration:
        state = snapshot()
        if state["frames_processed"] < previous_count:
            raise RuntimeError("Tracking restarted during measurement")
        if state["frames_processed"] > previous_count:
            observations.append(state)
            previous_count = state["frames_processed"]
        time.sleep(.2)
    elapsed = time.monotonic() - started
    last = snapshot()

    def median(key):
        values = [s[key] for s in observations if isinstance(s.get(key), (float, int))]
        return round(statistics.median(values), 1) if values else None

    faces = [s["last_face"] for s in observations if s.get("last_face")]
    metadata = [s.get("frame_metadata", {}) for s in observations]
    result = {
        "seconds": round(elapsed, 2),
        "frames": last["frames_processed"] - first["frames_processed"],
        "faces": last["faces_detected"] - first["faces_detected"],
        "effective_fps": round((last["frames_processed"] - first["frames_processed"]) / elapsed, 2),
        "capture_ms_median": median("capture_latency_ms"),
        "detection_ms_median": median("detection_ms"),
        "camera_errors": last["camera_errors"] - first["camera_errors"],
        "motion_frames_observed": sum(m.get("motion_compensated") is True and m.get("capture_motion_deg", 0) > .3
                                      for m in metadata),
        "max_capture_motion_deg": max((m.get("capture_motion_deg", 0) for m in metadata), default=0),
        "median_image_error_x": round(statistics.median(abs(f["x"] - .5) for f in faces), 4) if faces else None,
        "median_image_error_y": round(statistics.median(abs(f["y"] - .5) for f in faces), 4) if faces else None,
        "last": last,
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
