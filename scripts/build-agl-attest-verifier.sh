#!/bin/bash
set -e

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
source "$SCRIPT_DIR/config.sh"

SRC="$PROJ_DIR/src/agl_attest_verifier/agl_attest_verifier.c"
OUT="$PROJ_DIR/src/agl_attest_verifier/agl_attest_verifier"
CC="$THIRD_PARTY_DIR/$CROSS_COMPILE_DIR_LINUX/bin/aarch64-none-linux-gnu-gcc"

if [ ! -x "$CC" ]; then
    log_error "[agl-verifier] Toolchain not found: $CC"
    exit 1
fi

log "=== Building AGL attestation verifier ==="
log "    source : $SRC"
log "    output : $OUT"

"$CC" -O2 -static -Wall -Wextra -o "$OUT" "$SRC"

file "$OUT"
sha256sum "$OUT"
