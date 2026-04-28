#!/bin/bash
# Run standalone Realm shim + Zephyr payload bundle.
# Usage: ./run-realm-shim-zephyr.sh [--foreground] [--debug] [--shell-port <port>] [/root/realm-zephyr-shim.bin]

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
BUNDLE_BIN="/root/realm-zephyr-shim.bin"
ARGS=()

while [ $# -gt 0 ]; do
	case "$1" in
		--entry-offset|--serial-tty|--telnet-port|--shell-port)
			ARGS+=("$1")
			shift
			if [ -z "$1" ]; then
				echo "[ERROR] Missing value for ${ARGS[-1]}"
				exit 1
			fi
			ARGS+=("$1")
			shift
			;;
		-*)
			ARGS+=("$1")
			shift
			;;
		*)
			BUNDLE_BIN="$1"
			shift
			;;
	esac
done

exec "$SCRIPT_DIR/run-vecu-zephyr.sh" "${ARGS[@]}" "$BUNDLE_BIN"
