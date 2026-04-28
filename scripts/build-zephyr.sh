#!/bin/bash
# Build the Zephyr Realm V-ECU image used by lkvm --realm.
# This script is kept as the main Zephyr entrypoint and delegates to
# build-vecu-zephyr.sh so that documentation and runtime paths stay aligned.

set -e

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"

"$SCRIPT_DIR/build-vecu-zephyr.sh"
