#!/bin/bash
# Build Zephyr Realm V-ECU for lkvm --realm
set -e

SCRIPT_DIR="$(dirname $(readlink -f "$0"))"
PROJ_DIR="$(dirname $SCRIPT_DIR)"
ZEPHYR_BASE="$PROJ_DIR/dev_workspace/zephyr/zephyr"
APP_DIR="$PROJ_DIR/src/vecu_zephyr"
BUILD_DIR="$APP_DIR/build"
SDK_DIR="$PROJ_DIR/dev_workspace/zephyr-sdk-0.16.8"

echo "=== Building Zephyr Realm V-ECU ==="
echo "    app   : $APP_DIR"
echo "    board : lkvm_realm"
echo "    output: $BUILD_DIR"

# Activate Zephyr SDK
export ZEPHYR_SDK_INSTALL_DIR="$SDK_DIR"
export ZEPHYR_BASE="$ZEPHYR_BASE"

cd "$PROJ_DIR/dev_workspace/zephyr"
source zephyr/zephyr-env.sh 2>/dev/null || true

west build \
    --board lkvm_realm \
    --build-dir "$BUILD_DIR" \
    "$APP_DIR" \
    -- \
    -DBOARD_ROOT="$APP_DIR"

echo ""
echo "=== Build complete ==="
echo "    zephyr.bin: $BUILD_DIR/zephyr/zephyr.bin"
echo "    zephyr.elf: $BUILD_DIR/zephyr/zephyr.elf"
