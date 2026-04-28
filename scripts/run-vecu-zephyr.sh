#!/bin/bash
# Run Zephyr RTOS as a Realm VM (V-ECU1) via lkvm-realm --realm
# Run this script INSIDE the FVP Linux (Normal World)
# Usage: ./run-vecu-zephyr.sh [--foreground] [--debug] [--shell-port <port>] [--serial-tty <tty>] [--telnet-port <port>] [--entry-offset <hex>] [zephyr.bin path]

VM_NAME="realm-vecu1"
SOCK_FILE="/root/.lkvm/${VM_NAME}.sock"
FOREGROUND=0
DEBUG=0
LOG_FILE="/root/${VM_NAME}.log"
UART_LOG_FILE="/root/${VM_NAME}.uart.log"
TOKEN_FILE="/root/${VM_NAME}.token.bin"
TOKEN_META_FILE="/root/${VM_NAME}.token.meta"
DTB_DUMP="/root/${VM_NAME}.dtb"
ZEPHYR_BIN="/root/realm-zephyr.bin"
SHELL_PORT="${REALM_ZEPHYR_SHELL_PORT:-5002}"
SERIAL_TTY=""
TELNET_PORT=""
ENTRY_OFFSET=""
TOKEN_WATCH=1

set_shell_port() {
    case "$1" in
        5000|5001|5002|5003|5004)
            SHELL_PORT="$1"
            SERIAL_TTY="/dev/ttyAMA$((SHELL_PORT - 5000))"
            TELNET_PORT="$SHELL_PORT"
            ;;
        *)
            echo "[ERROR] Unsupported --shell-port: $1"
            echo "        Expected one of: 5000, 5001, 5002, 5003, 5004"
            exit 1
            ;;
    esac
}

set_shell_port "$SHELL_PORT"

while [ $# -gt 0 ]; do
    case "$1" in
        --foreground)
            FOREGROUND=1
            ;;
        --debug)
            DEBUG=1
            ;;
        --shell-port)
            shift
            if [ -z "$1" ]; then
                echo "[ERROR] Missing value for --shell-port"
                exit 1
            fi
            set_shell_port "$1"
            ;;
        --token-watch)
            TOKEN_WATCH=1
            ;;
        --no-token-watch)
            TOKEN_WATCH=0
            ;;
        --entry-offset)
            shift
            if [ -z "$1" ]; then
                echo "[ERROR] Missing value for --entry-offset"
                exit 1
            fi
            ENTRY_OFFSET="$1"
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
        -*)
            echo "[ERROR] Unknown option: $1"
            echo "        Usage: ./run-vecu-zephyr.sh [--foreground] [--debug] [--shell-port <port>] [--serial-tty <tty>] [--telnet-port <port>] [--entry-offset <hex>] [zephyr.bin path]"
            exit 1
            ;;
        *)
            ZEPHYR_BIN="$1"
            ;;
    esac
    shift
done

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

# Cleanup stale socket from previous run
if [ -S "$SOCK_FILE" ]; then
    echo "[*] Removing stale socket: $SOCK_FILE"
    rm -f "$SOCK_FILE"
fi

if ps -ef | grep -F "$VM_NAME" | grep -F "lkvm" | grep -v grep >/dev/null 2>&1; then
    if [ -x /root/stop-lkvm-vm.sh ]; then
        echo "[*] Existing VM detected, attempting automatic stop: $VM_NAME"
        /root/stop-lkvm-vm.sh "$VM_NAME" || true
        sleep 1
    fi
fi

if ps -ef | grep -F "$VM_NAME" | grep -F "lkvm" | grep -v grep >/dev/null 2>&1; then
    echo "[ERROR] A Realm VM named $VM_NAME is still running."
    echo "        Stop it first, for example:"
    echo "        /root/stop-lkvm-vm.sh $VM_NAME"
    echo "        ps -ef | grep -F \"--name $VM_NAME\""
    exit 1
fi

if [ ! -f "$ZEPHYR_BIN" ]; then
    echo "[ERROR] Zephyr binary not found: $ZEPHYR_BIN"
    echo "        Upload it with: scp zephyr.bin root@192.168.122.33:$ZEPHYR_BIN"
    exit 1
fi

LKVM_BIN="$(find_lkvm_bin || true)"
if [ -z "$LKVM_BIN" ]; then
    echo "[ERROR] Realm-capable lkvm binary not found."
    echo "        Expected one of: /root/lkvm-realm, /root/lkvm-static, lkvm-realm, lkvm-static, lkvm"
    echo "        Copy the built kvmtool-cca binary into the FVP guest, for example:"
    echo "        scp dev_workspace/kvmtool-cca/lkvm-static root@192.168.122.33:/root/lkvm-static"
    exit 1
fi

echo "[*] Starting Zephyr Realm VM..."
echo "    binary : $ZEPHYR_BIN"
echo "    lkvm   : $LKVM_BIN"
echo "    memory : 64MB"
echo "    VM name: $VM_NAME"
if [ "$FOREGROUND" -eq 1 ]; then
    echo "    output : current shell"
elif [ "$DEBUG" -eq 1 ]; then
    echo "    output : telnet localhost $TELNET_PORT"
    echo "    serial : $SERIAL_TTY"
    echo "    log    : $LOG_FILE"
    echo "    uart   : $UART_LOG_FILE"
    if [ "$TOKEN_WATCH" -eq 1 ]; then
        echo "    token  : $TOKEN_FILE"
        echo "    meta   : $TOKEN_META_FILE"
    fi
else
    echo "    output : telnet localhost $TELNET_PORT"
    echo "    serial : $SERIAL_TTY"
    echo "    uart   : $UART_LOG_FILE"
    if [ "$TOKEN_WATCH" -eq 1 ]; then
        echo "    token  : $TOKEN_FILE"
        echo "    meta   : $TOKEN_META_FILE"
    fi
fi
if [ -n "$ENTRY_OFFSET" ]; then
    echo "    entry  : base + $ENTRY_OFFSET"
fi
echo ""

CMD=(
    "$LKVM_BIN" run
    --realm \
    --disable-mte \
    --no-pvtime \
    --irqchip gicv3 \
    -k "$ZEPHYR_BIN" \
    -m 64 \
    -c 1 \
    --console serial \
    --name "$VM_NAME"
)

if [ "$DEBUG" -eq 1 ]; then
    CMD+=(--debug --dump-dtb "$DTB_DUMP")
fi

run_lkvm_debug_env() {
    if [ "$TOKEN_WATCH" -eq 1 ]; then
        LKVM_REALM_STATUS_WATCH=1 \
        LKVM_REALM_STATUS_GPA=0x83e10000 \
        LKVM_REALM_TOKEN_WATCH=1 \
        LKVM_REALM_TOKEN_GPA=0x83ffe000 \
        LKVM_REALM_TOKEN_CTRL_GPA=0x83fff000 \
        LKVM_REALM_TOKEN_OUT="$TOKEN_FILE" \
        LKVM_REALM_TOKEN_META="$TOKEN_META_FILE" \
        "$@"
    else
        LKVM_REALM_STATUS_WATCH=1 \
        LKVM_REALM_STATUS_GPA=0x83e10000 \
        "$@"
    fi
}

run_lkvm_token_env() {
    if [ "$TOKEN_WATCH" -eq 1 ]; then
        LKVM_REALM_TOKEN_WATCH=1 \
        LKVM_REALM_TOKEN_GPA=0x83ffe000 \
        LKVM_REALM_TOKEN_CTRL_GPA=0x83fff000 \
        LKVM_REALM_TOKEN_OUT="$TOKEN_FILE" \
        LKVM_REALM_TOKEN_META="$TOKEN_META_FILE" \
        "$@"
    else
        "$@"
    fi
}

if [ "$FOREGROUND" -eq 1 ]; then
    echo "[*] Running in foreground"
    if [ -n "$ENTRY_OFFSET" ]; then
        if [ "$DEBUG" -eq 1 ]; then
            LKVM_REALM_ENTRY_OFFSET="$ENTRY_OFFSET" \
            run_lkvm_debug_env "${CMD[@]}"
        else
            LKVM_REALM_ENTRY_OFFSET="$ENTRY_OFFSET" run_lkvm_token_env "${CMD[@]}"
        fi
    else
        if [ "$DEBUG" -eq 1 ]; then
            run_lkvm_debug_env "${CMD[@]}"
        else
            run_lkvm_token_env "${CMD[@]}"
        fi
    fi
else
    if [ ! -e "$SERIAL_TTY" ]; then
        echo "[WARN] Serial TTY not found: $SERIAL_TTY"
        echo "       Falling back to log-only mode."
        rm -f "$LOG_FILE"
        rm -f "$TOKEN_FILE" "$TOKEN_META_FILE"
        if [ -n "$ENTRY_OFFSET" ]; then
            LKVM_REALM_ENTRY_OFFSET="$ENTRY_OFFSET" run_lkvm_token_env "${CMD[@]}" >"$LOG_FILE" 2>&1 &
        else
            run_lkvm_token_env "${CMD[@]}" >"$LOG_FILE" 2>&1 &
        fi
        echo "[*] Started PID $!"
        echo "[*] Tail logs with: tail -f $LOG_FILE"
        if [ "$TOKEN_WATCH" -eq 1 ]; then
            echo "[*] Token dump   : $TOKEN_FILE"
            echo "[*] Token meta   : $TOKEN_META_FILE"
        fi
	elif [ "$DEBUG" -eq 1 ]; then
	    rm -f "$LOG_FILE"
	    rm -f "$UART_LOG_FILE"
	    rm -f "$TOKEN_FILE"
	    rm -f "$TOKEN_META_FILE"
	    : >"$LOG_FILE"
	    : >"$UART_LOG_FILE"
	    if [ -n "$ENTRY_OFFSET" ]; then
	        (
	            LKVM_REALM_ENTRY_OFFSET="$ENTRY_OFFSET" \
	            run_lkvm_debug_env "${CMD[@]}" <"$SERIAL_TTY" 2>"$LOG_FILE"
	        ) | tee "$UART_LOG_FILE" >"$SERIAL_TTY" &
	    else
	        (
	            run_lkvm_debug_env "${CMD[@]}" <"$SERIAL_TTY" 2>"$LOG_FILE"
	        ) | tee "$UART_LOG_FILE" >"$SERIAL_TTY" &
        fi
        echo "[*] Started PID $!"
        echo "[*] Guest serial : telnet localhost $TELNET_PORT"
        echo "[*] UART log     : tail -f $UART_LOG_FILE"
        echo "[*] Debug log    : tail -f $LOG_FILE"
        if [ "$TOKEN_WATCH" -eq 1 ]; then
            echo "[*] Token dump   : $TOKEN_FILE"
            echo "[*] Token meta   : $TOKEN_META_FILE"
        fi
    else
        rm -f "$LOG_FILE"
        rm -f "$UART_LOG_FILE"
        rm -f "$TOKEN_FILE"
        rm -f "$TOKEN_META_FILE"
        : >"$LOG_FILE"
        : >"$UART_LOG_FILE"
        if [ -n "$ENTRY_OFFSET" ]; then
            (
                LKVM_REALM_ENTRY_OFFSET="$ENTRY_OFFSET" run_lkvm_token_env "${CMD[@]}" <"$SERIAL_TTY" 2>"$LOG_FILE"
            ) | tee "$UART_LOG_FILE" >"$SERIAL_TTY" &
        else
            (
                run_lkvm_token_env "${CMD[@]}" <"$SERIAL_TTY" 2>"$LOG_FILE"
            ) | tee "$UART_LOG_FILE" >"$SERIAL_TTY" &
        fi
        echo "[*] Started PID $!"
        echo "[*] Guest serial : telnet localhost $TELNET_PORT"
        echo "[*] UART log     : tail -f $UART_LOG_FILE"
        echo "[*] Host log      : tail -f $LOG_FILE"
        if [ "$TOKEN_WATCH" -eq 1 ]; then
            echo "[*] Token dump   : $TOKEN_FILE"
            echo "[*] Token meta   : $TOKEN_META_FILE"
        fi
    fi
fi
