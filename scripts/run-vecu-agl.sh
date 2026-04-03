#!/bin/bash
# Run AGL as Normal World KVM guest (V-ECU2)
# Run this script INSIDE the FVP Linux (Normal World)
# Output goes to uart1 -> telnet localhost 5001

VM_NAME="agl-normal"
SOCK_FILE="/root/.lkvm/${VM_NAME}.sock"

if [ -S "$SOCK_FILE" ]; then
    echo "[*] Removing stale socket: $SOCK_FILE"
    rm -f "$SOCK_FILE"
fi

echo "[*] Starting AGL VM..."
echo "    kernel : /root/guest-Image"
echo "    disk   : /root/agl.ext4"
echo "    output : telnet localhost 5001"
echo ""

lkvm run --name "$VM_NAME" \
    --kernel /root/guest-Image \
    --disk /root/agl.ext4 \
    --cpus 1 --mem 512M \
    --console serial \
    --params "console=ttyAMA0,115200 root=/dev/vda rw" \
    < /dev/ttyAMA1 > /dev/ttyAMA1 2>&1 &

echo "[*] AGL started (PID $!)"
