#!/bin/bash
set -e

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
source "$SCRIPT_DIR/config.sh"

KVMTOOL_DIR="$THIRD_PARTY_DIR/kvmtool-cca"
LIBFDT_SRC_DIR="$THIRD_PARTY_DIR/libfdt-src"
LIBFDT_DIR="$LIBFDT_SRC_DIR/libfdt"
TOOLCHAIN_BIN_DIR="$THIRD_PARTY_DIR/$CROSS_COMPILE_DIR_LINUX/bin"
CROSS_PREFIX="$TOOLCHAIN_BIN_DIR/aarch64-none-linux-gnu-"
OUTPUT_BIN="$KVMTOOL_DIR/lkvm-static"
PATCH_FILE="$PROJ_DIR/patches/kvmtool-cca-realm-zephyr-attestation.patch"

CC="${CROSS_PREFIX}gcc"
AR="${CROSS_PREFIX}ar"
LD="${CROSS_PREFIX}ld"

if [ ! -x "$CC" ]; then
    log_error "[kvmtool-arm64] Toolchain not found: $CC"
    exit 1
fi

if [ ! -d "$KVMTOOL_DIR" ]; then
    log_error "[kvmtool-arm64] Missing kvmtool source tree: $KVMTOOL_DIR"
    exit 1
fi

if [ ! -d "$LIBFDT_SRC_DIR" ]; then
    log_error "[kvmtool-arm64] Missing libfdt source tree: $LIBFDT_SRC_DIR"
    exit 1
fi

if [ -f "$PATCH_FILE" ]; then
    log "=== Applying kvmtool Realm/Zephyr attestation patch ==="
    log "    patch  : $PATCH_FILE"

    if git -C "$KVMTOOL_DIR" apply --check "$PATCH_FILE"; then
        git -C "$KVMTOOL_DIR" apply "$PATCH_FILE"
        log "    status : applied"
    elif git -C "$KVMTOOL_DIR" apply --reverse --check "$PATCH_FILE"; then
        log "    status : already applied"
    else
        log_error "[kvmtool-arm64] Patch does not apply cleanly: $PATCH_FILE"
        log_error "                Check dev_workspace/kvmtool-cca for local changes."
        exit 1
    fi
fi

log "=== Building arm64 libfdt ==="
log "    source : $LIBFDT_SRC_DIR"
log "    cc     : $CC"

make -C "$LIBFDT_SRC_DIR" clean >/dev/null
make -C "$LIBFDT_SRC_DIR" libfdt \
    CC="$CC" \
    AR="$AR" \
    LD="$LD"

if [ ! -f "$LIBFDT_DIR/libfdt.a" ]; then
    log_error "[kvmtool-arm64] Missing arm64 libfdt archive: $LIBFDT_DIR/libfdt.a"
    exit 1
fi

log "=== Building arm64 lkvm-static ==="
log "    source : $KVMTOOL_DIR"
log "    libfdt : $LIBFDT_DIR"

make -C "$KVMTOOL_DIR" clean >/dev/null
make -C "$KVMTOOL_DIR" -j"$(nproc)" \
    ARCH=arm64 \
    CROSS_COMPILE="$CROSS_PREFIX" \
    LIBFDT_DIR="$LIBFDT_DIR" \
    lkvm-static

if [ ! -f "$OUTPUT_BIN" ]; then
    log_error "[kvmtool-arm64] Build did not produce: $OUTPUT_BIN"
    exit 1
fi

log "=== Build complete ==="
log "    lkvm-static: $OUTPUT_BIN"
file "$OUTPUT_BIN"
