#!/bin/bash
# Build Zephyr Realm V-ECU for lkvm --realm
set -e

SCRIPT_DIR="$(dirname $(readlink -f "$0"))"
PROJ_DIR="$(dirname $SCRIPT_DIR)"
ZEPHYR_BASE="$PROJ_DIR/dev_workspace/zephyr/zephyr"
APP_DIR="$PROJ_DIR/src/vecu_zephyr"
BUILD_DIR="$APP_DIR/build"
SDK_DIR="$PROJ_DIR/dev_workspace/zephyr-sdk-0.16.8"

BOARD="${BOARD:-lkvm_realm_payload}"
HEADLESS="${HEADLESS:-0}"
SHELL_MODE="${SHELL_MODE:-0}"
MINI_SHELL_MODE="${MINI_SHELL_MODE:-0}"
EXTRA_CONF_ARGS=()
EXTRA_CMAKE_ARGS=()

echo "=== Building Zephyr Realm Payload ==="
echo "    app   : $APP_DIR"
echo "    board : $BOARD"
echo "    output: $BUILD_DIR"
MODE_COUNT=0
[ "$HEADLESS" = "1" ] && MODE_COUNT=$((MODE_COUNT + 1))
[ "$SHELL_MODE" = "1" ] && MODE_COUNT=$((MODE_COUNT + 1))
[ "$MINI_SHELL_MODE" = "1" ] && MODE_COUNT=$((MODE_COUNT + 1))

if [ "$MODE_COUNT" -gt 1 ]; then
    echo "error: HEADLESS=1, SHELL_MODE=1, and MINI_SHELL_MODE=1 are mutually exclusive" >&2
    exit 1
fi
if [ "$HEADLESS" = "1" ]; then
    echo "    mode  : headless (status-page only)"
    EXTRA_CONF_ARGS+=("-DEXTRA_CONF_FILE=$APP_DIR/prj.headless.conf")
elif [ "$SHELL_MODE" = "1" ]; then
    echo "    mode  : shell (Realm-safe polling UART)"
    EXTRA_CONF_ARGS+=("-DEXTRA_CONF_FILE=$APP_DIR/prj.shell.conf")
elif [ "$MINI_SHELL_MODE" = "1" ]; then
    echo "    mode  : mini-shell (app-level UART command loop)"
    EXTRA_CONF_ARGS+=("-DEXTRA_CONF_FILE=$APP_DIR/prj.minishell.conf")
    EXTRA_CMAKE_ARGS+=("-DREALM_MINI_SHELL=ON")
fi

if [ -d "$BUILD_DIR" ]; then
    echo "    clean : removing stale build directory"
    rm -rf "$BUILD_DIR"
fi

# Activate Zephyr SDK
export ZEPHYR_SDK_INSTALL_DIR="$SDK_DIR"
export ZEPHYR_BASE="$ZEPHYR_BASE"
export CCACHE_DISABLE=1

cd "$PROJ_DIR/dev_workspace/zephyr"
source zephyr/zephyr-env.sh 2>/dev/null || true

west build \
    --board "$BOARD" \
    --build-dir "$BUILD_DIR" \
    "$APP_DIR" \
    -- \
    -DBOARD_ROOT="$APP_DIR" \
    "${EXTRA_CMAKE_ARGS[@]}" \
    "${EXTRA_CONF_ARGS[@]}"

echo ""
echo "=== Build complete ==="
echo "    zephyr.bin: $BUILD_DIR/zephyr/zephyr.bin"
echo "    zephyr.elf: $BUILD_DIR/zephyr/zephyr.elf"
