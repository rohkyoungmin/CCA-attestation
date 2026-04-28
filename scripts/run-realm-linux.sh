#!/bin/bash
# Run a Linux kernel as a Realm VM via lkvm --realm
# Run this script INSIDE the FVP Linux (Normal World)
# Usage: ./run-realm-linux.sh [--foreground] [--debug] [--console <serial|virtio>] [--kernel <Image>] [--disk <rootfs.ext4>] [--no-disk] [--mem <size>]

VM_NAME="realm-linux"
SOCK_FILE="/root/.lkvm/${VM_NAME}.sock"
FOREGROUND=0
DEBUG=0
LOG_FILE="/root/${VM_NAME}.log"
DTB_DUMP="/root/${VM_NAME}.dtb"
SERIAL_TTY="/dev/ttyAMA3"
MEMORY="512M"
KERNEL_IMG=""
DISK_IMG=""
USE_DISK=1
CONSOLE_KIND="serial"
KERNEL_PARAMS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --foreground)
            FOREGROUND=1
            ;;
        --debug)
            DEBUG=1
            ;;
        --console)
            shift
            if [ -z "$1" ]; then
                echo "[ERROR] Missing value for --console"
                exit 1
            fi
            CONSOLE_KIND="$1"
            ;;
        --virtio-console)
            CONSOLE_KIND="virtio"
            ;;
        --kernel)
            shift
            if [ -z "$1" ]; then
                echo "[ERROR] Missing value for --kernel"
                exit 1
            fi
            KERNEL_IMG="$1"
            ;;
        --disk)
            shift
            if [ -z "$1" ]; then
                echo "[ERROR] Missing value for --disk"
                exit 1
            fi
            DISK_IMG="$1"
            ;;
        --no-disk)
            USE_DISK=0
            ;;
        --mem)
            shift
            if [ -z "$1" ]; then
                echo "[ERROR] Missing value for --mem"
                exit 1
            fi
            MEMORY="$1"
            ;;
        -*)
            echo "[ERROR] Unknown option: $1"
            echo "        Usage: ./run-realm-linux.sh [--foreground] [--debug] [--console <serial|virtio>] [--kernel <Image>] [--disk <rootfs.ext4>] [--no-disk] [--mem <size>]"
            exit 1
            ;;
        *)
            if [ -z "$KERNEL_IMG" ]; then
                KERNEL_IMG="$1"
            elif [ -z "$DISK_IMG" ]; then
                DISK_IMG="$1"
            else
                echo "[ERROR] Unexpected argument: $1"
                exit 1
            fi
            ;;
    esac
    shift
done

case "$CONSOLE_KIND" in
    serial|virtio)
        ;;
    *)
        echo "[ERROR] Unsupported console kind: $CONSOLE_KIND"
        echo "        Expected: serial or virtio"
        exit 1
        ;;
esac

find_lkvm_bin() {
    for candidate in /root/lkvm-realm /root/lkvm-static lkvm-realm lkvm-static lkvm; do
        if command -v "$candidate" >/dev/null 2>&1; then
            command -v "$candidate"
            return 0
        fi
        if [ -x "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

find_kernel_img() {
    for candidate in "$KERNEL_IMG" /root/realm-linux-Image /root/guest-Image; do
        if [ -n "$candidate" ] && [ -f "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

find_disk_img() {
    for candidate in "$DISK_IMG" /root/agl.ext4 /root/rootfs.ext4; do
        if [ -n "$candidate" ] && [ -f "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

if [ -S "$SOCK_FILE" ]; then
    echo "[*] Removing stale socket: $SOCK_FILE"
    rm -f "$SOCK_FILE"
fi

if ps -ef | grep -F "$VM_NAME" | grep -F "lkvm" | grep -v grep >/dev/null 2>&1; then
    echo "[ERROR] A Realm VM named $VM_NAME is already running."
    echo "        Stop it first, for example:"
    echo "        /root/stop-lkvm-vm.sh $VM_NAME"
    exit 1
fi

LKVM_BIN="$(find_lkvm_bin || true)"
if [ -z "$LKVM_BIN" ]; then
    echo "[ERROR] Realm-capable lkvm binary not found."
    echo "        Expected one of: /root/lkvm-realm, /root/lkvm-static, lkvm-realm, lkvm-static, lkvm"
    exit 1
fi

KERNEL_IMG="$(find_kernel_img || true)"
if [ -z "$KERNEL_IMG" ]; then
    echo "[ERROR] Linux guest Image not found."
    echo "        Tried: /root/realm-linux-Image, /root/guest-Image"
    exit 1
fi

if [ "$USE_DISK" -eq 1 ]; then
    DISK_IMG="$(find_disk_img || true)"
    if [ -z "$DISK_IMG" ]; then
        echo "[ERROR] Linux guest rootfs image not found."
        echo "        Tried: /root/agl.ext4, /root/rootfs.ext4"
        echo "        Or run without a disk for an early-boot probe: --no-disk"
        exit 1
    fi
fi

if [ "$CONSOLE_KIND" = "virtio" ]; then
    KERNEL_PARAMS="console=hvc0 ignore_loglevel"
else
    KERNEL_PARAMS="earlycon=uart8250,mmio,0x1000000 console=ttyS0,115200 ignore_loglevel"
fi

if [ "$USE_DISK" -eq 1 ]; then
    KERNEL_PARAMS="$KERNEL_PARAMS root=/dev/vda rw rootwait"
fi

echo "[*] Starting Linux Realm VM..."
echo "    kernel : $KERNEL_IMG"
if [ "$USE_DISK" -eq 1 ]; then
    echo "    disk   : $DISK_IMG"
else
    echo "    disk   : <none>"
fi
echo "    lkvm   : $LKVM_BIN"
echo "    memory : $MEMORY"
echo "    VM name: $VM_NAME"
echo "    console: $CONSOLE_KIND"
if [ "$FOREGROUND" -eq 1 ]; then
    echo "    output : current shell"
elif [ "$DEBUG" -eq 1 ]; then
    echo "    output : telnet localhost 5003"
    echo "    log    : $LOG_FILE"
else
    echo "    output : telnet localhost 5003"
fi
echo ""

CMD=(
    "$LKVM_BIN" run
    --realm
    --disable-mte
    --no-pvtime
    --irqchip gicv3
    --kernel "$KERNEL_IMG"
    --cpus 1
    --mem "$MEMORY"
    --console "$CONSOLE_KIND"
    --name "$VM_NAME"
    --params "$KERNEL_PARAMS"
)

if [ "$USE_DISK" -eq 1 ]; then
    CMD+=(--disk "$DISK_IMG")
fi

if [ "$DEBUG" -eq 1 ]; then
    CMD+=(--debug --dump-dtb "$DTB_DUMP")
fi

if [ "$FOREGROUND" -eq 1 ]; then
    echo "[*] Running in foreground"
    "${CMD[@]}"
elif [ ! -e "$SERIAL_TTY" ]; then
    echo "[WARN] Serial TTY not found: $SERIAL_TTY"
    echo "       Falling back to log-only mode."
    rm -f "$LOG_FILE"
    "${CMD[@]}" >"$LOG_FILE" 2>&1 &
    echo "[*] Started PID $!"
    echo "[*] Tail logs with: tail -f $LOG_FILE"
elif [ "$DEBUG" -eq 1 ]; then
    rm -f "$LOG_FILE"
    (
        "${CMD[@]}" <"$SERIAL_TTY" 2>&1 | tee "$LOG_FILE" >"$SERIAL_TTY"
    ) &
    echo "[*] Started PID $!"
    echo "[*] Guest serial : telnet localhost 5003"
    echo "[*] Debug log    : tail -f $LOG_FILE"
else
    "${CMD[@]}" <"$SERIAL_TTY" >"$SERIAL_TTY" 2>&1 &
    echo "[*] Started PID $!"
    echo "[*] Guest serial : telnet localhost 5003"
fi
