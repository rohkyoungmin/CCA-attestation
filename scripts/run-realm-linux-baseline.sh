#!/bin/bash
# Run the known-good Linux Realm baseline used for comparison against Zephyr.
# Run this script INSIDE the FVP Linux (Normal World).
# Usage: ./run-realm-linux-baseline.sh [extra args...]

find_runner() {
    local script_dir

    script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

    for candidate in \
        "${script_dir}/run-realm-linux.sh" \
        "/root/run-realm-linux.sh" \
        "run-realm-linux.sh"
    do
        if [ -x "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done

    return 1
}

RUNNER="$(find_runner || true)"
if [ -z "$RUNNER" ]; then
    echo "[ERROR] run-realm-linux.sh not found."
    echo "        Copy both run-realm-linux.sh and run-realm-linux-baseline.sh into the FVP guest."
    exit 1
fi

exec "$RUNNER" --foreground --console virtio --no-disk --mem 128M "$@"
