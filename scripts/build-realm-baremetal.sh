#!/bin/bash

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
source "$SCRIPT_DIR/config.sh"

APP_DIR="$SRC_DIR/realm_baremetal"
OUT_DIR="$APP_DIR/build"
ELF="$OUT_DIR/realm_baremetal.elf"
BIN="$OUT_DIR/realm_baremetal.bin"
MAP="$OUT_DIR/realm_baremetal.map"

CC="$THIRD_PARTY_DIR/$CROSS_COMPILE_DIR/bin/aarch64-none-elf-gcc"
OBJCOPY="$THIRD_PARTY_DIR/$CROSS_COMPILE_DIR/bin/aarch64-none-elf-objcopy"
OBJDUMP="$THIRD_PARTY_DIR/$CROSS_COMPILE_DIR/bin/aarch64-none-elf-objdump"

mkdir -p "$OUT_DIR"

if [ ! -x "$CC" ]; then
    log_error "[realm-baremetal] Toolchain not found: $CC"
    exit 1
fi

log "=== Building Realm Bare-Metal Marker PoC ==="
log "    source : $APP_DIR"
log "    output : $OUT_DIR"

"$CC" -nostdlib -nostartfiles -ffreestanding \
    -mcpu=cortex-a53 -march=armv8-a \
    -T "$APP_DIR/linker.ld" \
    -Wl,-Map="$MAP" \
    -o "$ELF" \
    "$APP_DIR/start.S"

if [ $? -ne 0 ]; then
    log_error "[realm-baremetal] ELF build failed."
    exit 1
fi

"$OBJCOPY" -O binary "$ELF" "$BIN"

if [ $? -ne 0 ]; then
    log_error "[realm-baremetal] Binary extraction failed."
    exit 1
fi

log "=== Build complete ==="
log "    realm_baremetal.bin: $BIN"
log "    realm_baremetal.elf: $ELF"
log "    realm_baremetal.map: $MAP"
"$OBJDUMP" -D "$ELF" --start-address=0x80000000 --stop-address=0x80000100 | sed -n '1,80p'
