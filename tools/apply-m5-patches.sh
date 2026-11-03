#!/usr/bin/env bash
# Apply minimal local patches to the M5GFX / M5Unified submodules.
#
# Run after `git submodule update --init --recursive`. The submodule will
# show as dirty in `git status` afterwards (expected). To re-sync upstream:
# `git submodule update -f` resets, then re-run this script.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

apply_if_needed() {
    local sub="$1"
    local patch="$2"

    if git -C "$root/components/$sub" apply --reverse --check "$patch" 2>/dev/null; then
        echo "[$sub] patch already applied"
        return 0
    fi

    if ! git -C "$root/components/$sub" apply --check "$patch"; then
        echo "[$sub] patch does not apply; resolve the conflicting changes before continuing." >&2
        return 1
    fi

    echo "[$sub] applying $patch"
    git -C "$root/components/$sub" apply "$patch"
}

apply_if_needed M5Unified "$root/patches/m5unified.patch"
apply_if_needed M5Unified "$root/patches/m5unified-local-m5gfx.patch"
apply_if_needed M5GFX "$root/patches/m5gfx.patch"

echo "Done. components/M5Unified and components/M5GFX show as dirty in 'git status' — this is expected."
