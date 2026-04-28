#!/bin/bash

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
source "$SCRIPT_DIR/config.sh"

APP_DIR="$SRC_DIR/realm_shim"
OUT_DIR="$APP_DIR/build"
ELF="$OUT_DIR/realm_shim.elf"
BIN="$OUT_DIR/realm_shim.bin"
MAP="$OUT_DIR/realm_shim.map"

CC="$THIRD_PARTY_DIR/$CROSS_COMPILE_DIR/bin/aarch64-none-elf-gcc"
OBJCOPY="$THIRD_PARTY_DIR/$CROSS_COMPILE_DIR/bin/aarch64-none-elf-objcopy"
OBJDUMP="$THIRD_PARTY_DIR/$CROSS_COMPILE_DIR/bin/aarch64-none-elf-objdump"

mkdir -p "$OUT_DIR"

if [ ! -x "$CC" ]; then
    log_error "[realm-shim] Toolchain not found: $CC"
    exit 1
fi

log "=== Building Realm Shim ==="
log "    source : $APP_DIR"
log "    output : $OUT_DIR"
if [ -n "$REALM_SHIM_PAYLOAD_ENTRY" ]; then
    log "    payload entry override : $REALM_SHIM_PAYLOAD_ENTRY"
fi

"$CC" -nostdlib -nostartfiles -ffreestanding \
    -mcpu=cortex-a53 -march=armv8-a \
    -I"$APP_DIR" \
    ${REALM_SHIM_PAYLOAD_ENTRY:+-DREALM_SHIM_PAYLOAD_ENTRY=$REALM_SHIM_PAYLOAD_ENTRY} \
    -T "$APP_DIR/linker.ld" \
    -Wl,-Map="$MAP" \
    -o "$ELF" \
    "$APP_DIR/start.S"

if [ $? -ne 0 ]; then
    log_error "[realm-shim] ELF build failed."
    exit 1
fi

"$OBJCOPY" -O binary "$ELF" "$BIN"

if [ $? -ne 0 ]; then
    log_error "[realm-shim] Binary extraction failed."
    exit 1
fi

log "=== Build complete ==="
log "    realm_shim.bin: $BIN"
log "    realm_shim.elf: $ELF"
log "    realm_shim.map: $MAP"
"$OBJDUMP" -D "$ELF" --start-address=0x80000000 --stop-address=0x80000180 | sed -n '1,120p'
