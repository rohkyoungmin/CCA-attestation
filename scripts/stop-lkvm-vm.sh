#!/bin/bash
# Stop a specific lkvm guest by VM name without touching other guests.
# Run this script INSIDE the FVP Linux (Normal World).
# Usage: ./stop-lkvm-vm.sh <vm-name>

VM_NAME="$1"
SOCK_FILE="/root/.lkvm/${VM_NAME}.sock"

if [ -z "$VM_NAME" ]; then
    echo "[ERROR] Missing VM name."
    echo "        Usage: ./stop-lkvm-vm.sh <vm-name>"
    exit 1
fi

find_vm_pids() {
    ps -ef | awk -v vm="$VM_NAME" '
        /lkvm/ && $0 !~ /awk/ && $0 ~ ("--name " vm "([[:space:]]|$)") {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^[0-9]+$/) {
                    print $i
                    break
                }
            }
        }
    '
}

PIDS="$(find_vm_pids)"

if [ -n "$PIDS" ]; then
    echo "[*] Stopping lkvm VM: $VM_NAME"
    kill -9 $PIDS 2>/dev/null

    for _ in 1 2 3 4 5; do
        sleep 1
        PIDS="$(find_vm_pids)"
        if [ -z "$PIDS" ]; then
            break
        fi
        kill -9 $PIDS 2>/dev/null
    done
else
    echo "[*] No running lkvm process found for VM: $VM_NAME"
fi

if [ -S "$SOCK_FILE" ]; then
    echo "[*] Removing stale socket: $SOCK_FILE"
    rm -f "$SOCK_FILE"
fi

PIDS="$(find_vm_pids)"
if [ -n "$PIDS" ]; then
    echo "[WARN] VM still appears to be running after stop attempt: $VM_NAME"
    echo "       Remaining PID(s): $PIDS"
    exit 1
fi
