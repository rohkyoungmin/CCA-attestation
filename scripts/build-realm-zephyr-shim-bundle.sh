#!/bin/bash
set -e

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
source "$SCRIPT_DIR/config.sh"

SHIM_APP_DIR="$SRC_DIR/realm_shim"
PAYLOAD_APP_DIR="$SRC_DIR/vecu_zephyr"

SHIM_BIN="$SHIM_APP_DIR/build/realm_shim.bin"
PAYLOAD_ELF="$PAYLOAD_APP_DIR/build/zephyr/zephyr.elf"
PAYLOAD_BIN="$PAYLOAD_APP_DIR/build/zephyr/zephyr.bin"
BUNDLE_DIR="$SHIM_APP_DIR/build"
BUNDLE_BIN="$BUNDLE_DIR/realm_zephyr_shim.bin"

PAYLOAD_OFFSET=$((0x00200000))
IMAGE_SIZE_OFFSET=$((0x10))

log "=== Building standalone Realm shim bundle ==="
log "    shim     : $SHIM_APP_DIR"
log "    payload  : $PAYLOAD_APP_DIR"
log "    offset   : 0x00200000"

"$SCRIPT_DIR/build-vecu-zephyr.sh"

if [ ! -f "$PAYLOAD_ELF" ]; then
    log_error "[realm-shim-bundle] Missing Zephyr payload ELF: $PAYLOAD_ELF"
    exit 1
fi

PAYLOAD_ENTRY=$(
    source "$SCRIPT_DIR/config.sh"
    "$THIRD_PARTY_DIR/$CROSS_COMPILE_DIR_LINUX/bin/aarch64-none-linux-gnu-nm" -n "$PAYLOAD_ELF" |
        awk '/ realm_payload_entry$/ {print "0x"$1; exit}'
)

if [ -z "$PAYLOAD_ENTRY" ]; then
    PAYLOAD_ENTRY=$(readelf -h "$PAYLOAD_ELF" | awk '/Entry point address:/ {print $4}')
fi

if [ -z "$PAYLOAD_ENTRY" ]; then
    log_error "[realm-shim-bundle] Failed to determine Zephyr payload entry from: $PAYLOAD_ELF"
    exit 1
fi

log "    payload entry : $PAYLOAD_ENTRY"

REALM_SHIM_PAYLOAD_ENTRY="$PAYLOAD_ENTRY" "$SCRIPT_DIR/build-realm-shim.sh"

if [ ! -f "$SHIM_BIN" ]; then
    log_error "[realm-shim-bundle] Missing shim binary: $SHIM_BIN"
    exit 1
fi

if [ ! -f "$PAYLOAD_BIN" ]; then
    log_error "[realm-shim-bundle] Missing Zephyr payload binary: $PAYLOAD_BIN"
    exit 1
fi

mkdir -p "$BUNDLE_DIR"
cp "$SHIM_BIN" "$BUNDLE_BIN"

SHIM_SIZE=$(stat -c%s "$SHIM_BIN")
PAYLOAD_SIZE=$(stat -c%s "$PAYLOAD_BIN")

if [ "$SHIM_SIZE" -gt "$PAYLOAD_OFFSET" ]; then
    log_error "[realm-shim-bundle] Shim is too large ($SHIM_SIZE bytes) for payload offset 0x00200000"
    exit 1
fi

truncate -s "$PAYLOAD_OFFSET" "$BUNDLE_BIN"
cat "$PAYLOAD_BIN" >> "$BUNDLE_BIN"

BUNDLE_SIZE=$(stat -c%s "$BUNDLE_BIN")
python3 - "$BUNDLE_BIN" "$IMAGE_SIZE_OFFSET" "$BUNDLE_SIZE" <<'PY'
import struct
import sys

path = sys.argv[1]
offset = int(sys.argv[2], 0)
size = int(sys.argv[3], 0)

with open(path, "r+b") as f:
    f.seek(offset)
    f.write(struct.pack("<Q", size))
PY

log "=== Bundle complete ==="
log "    shim size    : $SHIM_SIZE bytes"
log "    payload size : $PAYLOAD_SIZE bytes"
log "    image size   : $BUNDLE_SIZE bytes"
log "    bundle bin   : $BUNDLE_BIN"
