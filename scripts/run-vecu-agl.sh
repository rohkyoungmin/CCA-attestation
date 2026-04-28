#!/bin/bash
# Run AGL as a Normal World KVM guest (V-ECU2)
# Run this script INSIDE the FVP Linux (Normal World)
# Usage: ./run-vecu-agl.sh [--foreground] [--debug] [--serial-tty <tty>] [--telnet-port <port>] [--net-mode user|tap]

VM_NAME="agl-normal"
SOCK_FILE="/root/.lkvm/${VM_NAME}.sock"
FOREGROUND=0
DEBUG=0
LOG_FILE="/root/${VM_NAME}.log"
UART_LOG_FILE="/root/${VM_NAME}.uart.log"
KERNEL_IMG="/root/guest-Image"
DISK_IMG="/root/agl.ext4"
SERIAL_TTY="/dev/ttyAMA1"
TELNET_PORT="5001"
NET_MODE="${AGL_NET_MODE:-user}"
NET_HOST_IP="${AGL_NET_HOST_IP:-192.168.34.1}"
NET_GUEST_IP="${AGL_NET_GUEST_IP:-192.168.34.15}"
NET_TAPIF="${AGL_NET_TAPIF:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --foreground)
            FOREGROUND=1
            ;;
        --debug)
            DEBUG=1
            ;;
        --serial-tty)
            shift
            if [ -z "$1" ]; then
                echo "[ERROR] Missing value for --serial-tty"
                exit 1
            fi
            SERIAL_TTY="$1"
            ;;
        --telnet-port)
            shift
            if [ -z "$1" ]; then
                echo "[ERROR] Missing value for --telnet-port"
                exit 1
            fi
            TELNET_PORT="$1"
            ;;
        --net-mode)
            shift
            if [ -z "$1" ]; then
                echo "[ERROR] Missing value for --net-mode"
                exit 1
            fi
            NET_MODE="$1"
            ;;
        --net-host-ip)
            shift
            if [ -z "$1" ]; then
                echo "[ERROR] Missing value for --net-host-ip"
                exit 1
            fi
            NET_HOST_IP="$1"
            ;;
        --net-guest-ip)
            shift
            if [ -z "$1" ]; then
                echo "[ERROR] Missing value for --net-guest-ip"
                exit 1
            fi
            NET_GUEST_IP="$1"
            ;;
        --net-tapif)
            shift
            if [ -z "$1" ]; then
                echo "[ERROR] Missing value for --net-tapif"
                exit 1
            fi
            NET_TAPIF="$1"
            ;;
        -h|--help)
            echo "Usage: ./run-vecu-agl.sh [--foreground] [--debug] [--serial-tty <tty>] [--telnet-port <port>] [--net-mode user|tap] [--net-host-ip <ip>] [--net-guest-ip <ip>] [--net-tapif <tap>]"
            exit 0
            ;;
        -*)
            echo "[ERROR] Unknown option: $1"
            echo "        Usage: ./run-vecu-agl.sh [--foreground] [--debug] [--serial-tty <tty>] [--telnet-port <port>] [--net-mode user|tap] [--net-host-ip <ip>] [--net-guest-ip <ip>] [--net-tapif <tap>]"
            exit 1
            ;;
    esac
    shift
done

find_lkvm_bin() {
    for candidate in /root/lkvm-static lkvm-static lkvm; do
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

if [ -S "$SOCK_FILE" ]; then
    echo "[*] Removing stale socket: $SOCK_FILE"
    rm -f "$SOCK_FILE"
fi

if ps -ef | grep -F "$VM_NAME" | grep -F "lkvm" | grep -v grep >/dev/null 2>&1; then
    echo "[ERROR] A Normal World VM named $VM_NAME is already running."
    echo "        Stop it first, for example:"
    echo "        /root/stop-lkvm-vm.sh $VM_NAME"
    exit 1
fi

if [ ! -f "$KERNEL_IMG" ] || [ ! -f "$DISK_IMG" ]; then
    echo "[ERROR] Missing AGL guest artifacts."
    echo "        kernel: $KERNEL_IMG"
    echo "        disk  : $DISK_IMG"
    exit 1
fi

LKVM_BIN="$(find_lkvm_bin || true)"
if [ -z "$LKVM_BIN" ]; then
    echo "[ERROR] lkvm binary not found."
    echo "        Expected one of: /root/lkvm-static, lkvm-static, lkvm"
    exit 1
fi

echo "[*] Starting AGL VM..."
echo "    kernel : $KERNEL_IMG"
echo "    disk   : $DISK_IMG"
echo "    lkvm   : $LKVM_BIN"
echo "    memory : 512MB"
echo "    VM name: $VM_NAME"
echo "    net    : $NET_MODE"
if [ "$NET_MODE" = "tap" ]; then
    echo "    host IP: $NET_HOST_IP"
    echo "    guest IP: $NET_GUEST_IP"
fi
if [ "$FOREGROUND" -eq 1 ]; then
    echo "    output : current shell"
elif [ "$DEBUG" -eq 1 ]; then
    echo "    log    : $LOG_FILE"
else
    echo "    output : telnet localhost $TELNET_PORT"
    echo "    serial : $SERIAL_TTY"
    echo "    uart   : $UART_LOG_FILE"
fi
echo ""

if [ "$NET_MODE" = "tap" ]; then
    NET_ARG="mode=tap,script=none,host_ip=${NET_HOST_IP},guest_ip=${NET_GUEST_IP}"
    if [ -n "$NET_TAPIF" ]; then
        NET_ARG="${NET_ARG},tapif=${NET_TAPIF}"
    fi
    KERNEL_IP_PARAM="ip=${NET_GUEST_IP}::${NET_HOST_IP}:255.255.255.0::enp0s0:off"
elif [ "$NET_MODE" = "user" ]; then
    NET_ARG="mode=user"
    KERNEL_IP_PARAM="ip=dhcp"
else
    echo "[ERROR] Unsupported --net-mode: $NET_MODE"
    echo "        Expected: user or tap"
    exit 1
fi

CMD=(
    "$LKVM_BIN" run
    --name "$VM_NAME"
    --kernel "$KERNEL_IMG"
    --disk "$DISK_IMG"
    --cpus 1
    --mem 512M
    --console serial
    --network "$NET_ARG"
    --params "console=ttyAMA0,115200 root=/dev/vda rw ${KERNEL_IP_PARAM}"
)

if [ "$DEBUG" -eq 1 ]; then
    CMD+=(--debug)
fi

if [ "$FOREGROUND" -eq 1 ]; then
    echo "[*] Running in foreground"
    "${CMD[@]}"
elif [ "$DEBUG" -eq 1 ]; then
    rm -f "$LOG_FILE"
    "${CMD[@]}" >"$LOG_FILE" 2>&1 &
    echo "[*] Started PID $!"
    echo "[*] Tail logs with: tail -f $LOG_FILE"
else
    rm -f "$UART_LOG_FILE"
    : >"$UART_LOG_FILE"
    (
        "${CMD[@]}" < "$SERIAL_TTY" 2>&1
    ) | tee "$UART_LOG_FILE" > "$SERIAL_TTY" &
    echo "[*] Started PID $!"
    echo "[*] Connect with: telnet localhost $TELNET_PORT"
    echo "[*] UART log    : tail -f $UART_LOG_FILE"
fi
