#!/bin/bash
# Build a minimal Zephyr Realm smoke-test guest.

set -e

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
PROJ_DIR="$(dirname "$SCRIPT_DIR")"
ZEPHYR_BASE="$PROJ_DIR/dev_workspace/zephyr/zephyr"
APP_DIR="$PROJ_DIR/src/vecu_zephyr_minimal"
BOARD_ROOT_DIR="$PROJ_DIR/src/vecu_zephyr"
BUILD_DIR="$APP_DIR/build"
SDK_DIR="$PROJ_DIR/dev_workspace/zephyr-sdk-0.16.8"

echo "=== Building Minimal Zephyr Realm Guest ==="
echo "    app       : $APP_DIR"
echo "    board     : lkvm_realm"
echo "    board root: $BOARD_ROOT_DIR"
echo "    output    : $BUILD_DIR"

if [ -d "$BUILD_DIR" ]; then
    echo "    clean     : removing stale build directory"
    rm -rf "$BUILD_DIR"
fi

export ZEPHYR_SDK_INSTALL_DIR="$SDK_DIR"
export ZEPHYR_BASE="$ZEPHYR_BASE"

cd "$PROJ_DIR/dev_workspace/zephyr"
source zephyr/zephyr-env.sh 2>/dev/null || true

west build \
    --board lkvm_realm \
    --build-dir "$BUILD_DIR" \
    "$APP_DIR" \
    -- \
    -DBOARD_ROOT="$BOARD_ROOT_DIR"

echo ""
echo "=== Build complete ==="
echo "    zephyr.bin: $BUILD_DIR/zephyr/zephyr.bin"
echo "    zephyr.elf: $BUILD_DIR/zephyr/zephyr.elf"
