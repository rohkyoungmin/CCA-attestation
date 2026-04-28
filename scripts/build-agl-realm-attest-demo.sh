#!/bin/bash
# Build the host-side artifacts required by the paper-facing AGL + Realm demo.

set -e

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"

echo "[*] Building patched arm64 kvmtool"
"$SCRIPT_DIR/build-kvmtool-arm64.sh"

echo ""
echo "[*] Building AGL userspace attestation verifier"
"$SCRIPT_DIR/build-agl-attest-verifier.sh"

echo ""
echo "[*] Building Realm shim + Zephyr mini-shell bundle"
MINI_SHELL_MODE=1 "$SCRIPT_DIR/build-realm-zephyr-shim-bundle.sh"

echo ""
echo "[*] Paper demo artifacts are ready:"
echo "    lkvm    : dev_workspace/kvmtool-cca/lkvm-static"
echo "    verifier: src/agl_attest_verifier/agl_attest_verifier"
echo "    realm   : src/realm_shim/build/realm_zephyr_shim.bin"
