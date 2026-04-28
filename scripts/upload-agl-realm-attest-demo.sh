#!/bin/bash
# Upload the Realm/AGL attestation demo pieces from the host into FVP Linux.

set -e

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
TARGET="root@192.168.122.33"
AGL_KERNEL=""
AGL_DISK=""

usage() {
    echo "Usage: ./scripts/upload-agl-realm-attest-demo.sh [target] [--agl-kernel <Image>] [--agl-disk <agl.ext4>]"
}

require_file() {
    if [ ! -f "$1" ]; then
        echo "[ERROR] Missing required file: $1"
        exit 1
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --agl-kernel)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --agl-kernel"; exit 1; }
            AGL_KERNEL="$1"
            ;;
        --agl-disk)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --agl-disk"; exit 1; }
            AGL_DISK="$1"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "[ERROR] Unknown option: $1"
            usage
            exit 1
            ;;
        *)
            TARGET="$1"
            ;;
    esac
    shift
done

LKVM_BIN="$REPO_ROOT/dev_workspace/kvmtool-cca/lkvm-static"
REALM_BUNDLE="$REPO_ROOT/src/realm_shim/build/realm_zephyr_shim.bin"
AGL_VERIFIER="$REPO_ROOT/src/agl_attest_verifier/agl_attest_verifier"

require_file "$LKVM_BIN"
require_file "$REALM_BUNDLE"
require_file "$AGL_VERIFIER"

if [ -n "$AGL_KERNEL" ]; then
    require_file "$AGL_KERNEL"
fi

if [ -n "$AGL_DISK" ]; then
    require_file "$AGL_DISK"
fi

echo "[*] Uploading Realm/AGL demo to $TARGET"
scp "$LKVM_BIN" "$TARGET:/root/lkvm-static.new"
scp "$REALM_BUNDLE" "$TARGET:/root/realm-zephyr-shim.bin"
scp \
    "$REPO_ROOT/scripts/run-vecu-agl.sh" \
    "$REPO_ROOT/scripts/run-vecu-zephyr.sh" \
    "$REPO_ROOT/scripts/run-realm-shim-zephyr.sh" \
    "$REPO_ROOT/scripts/stop-lkvm-vm.sh" \
    "$REPO_ROOT/scripts/read-realm-token.sh" \
    "$REPO_ROOT/scripts/run-agl-realm-attest-demo.sh" \
    "$REPO_ROOT/scripts/install-agl-attest-verifier-rootfs.sh" \
    "$TARGET:/root/"
scp "$AGL_VERIFIER" "$TARGET:/root/agl_attest_verifier"

if [ -n "$AGL_KERNEL" ]; then
    scp "$AGL_KERNEL" "$TARGET:/root/guest-Image"
else
    echo "[*] Skipping AGL kernel upload. Expected on FVP as: /root/guest-Image"
fi

if [ -n "$AGL_DISK" ]; then
    scp "$AGL_DISK" "$TARGET:/root/agl.ext4"
else
    echo "[*] Skipping AGL disk upload. Expected on FVP as: /root/agl.ext4"
fi

ssh "$TARGET" 'mv /root/lkvm-static.new /root/lkvm-static && chmod +x /root/lkvm-static /root/*.sh /root/agl_attest_verifier'

echo "[*] Upload complete."
echo "    FVP command:"
echo "    /root/run-agl-realm-attest-demo.sh --iterations 50"
