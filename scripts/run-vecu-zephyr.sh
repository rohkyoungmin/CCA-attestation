#!/bin/bash
# Run Zephyr RTOS as a Realm VM (V-ECU1) via lkvm-realm --realm
# Run this script INSIDE the FVP Linux (Normal World)
# Usage: ./run-vecu-zephyr.sh [zephyr.bin path]

ZEPHYR_BIN="${1:-/root/realm-zephyr.bin}"
VM_NAME="realm-vecu1"
SOCK_FILE="/root/.lkvm/${VM_NAME}.sock"

# Cleanup stale socket from previous run
if [ -S "$SOCK_FILE" ]; then
    echo "[*] Removing stale socket: $SOCK_FILE"
    rm -f "$SOCK_FILE"
fi

if [ ! -f "$ZEPHYR_BIN" ]; then
    echo "[ERROR] Zephyr binary not found: $ZEPHYR_BIN"
    echo "        Upload it with: scp zephyr.bin root@192.168.122.33:$ZEPHYR_BIN"
    exit 1
fi

echo "[*] Starting Zephyr Realm VM..."
echo "    binary : $ZEPHYR_BIN"
echo "    memory : 64MB"
echo "    VM name: $VM_NAME"
echo ""

lkvm-realm run \
    --realm \
    -k "$ZEPHYR_BIN" \
    -m 64 \
    -c 1 \
    --console serial \
    --name "$VM_NAME" < /dev/ttyAMA2 > /dev/ttyAMA2 2>&1 &
