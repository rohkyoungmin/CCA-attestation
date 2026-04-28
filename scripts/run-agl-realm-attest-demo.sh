#!/bin/bash
# Start the current paper-facing demo inside the FVP Linux guest.
# It launches AGL on telnet 5001 and the Zephyr Realm shell on telnet 5002.

set -e

REALM_PORT="5002"
AGL_PORT="5001"
ITERATIONS="50"
REALM_BIN="/root/realm-zephyr-shim.bin"
AGL_VERIFY_PORT="7777"
AGL_NET_MODE="tap"
AGL_NET_HOST_IP="192.168.34.1"
AGL_NET_GUEST_IP="192.168.34.15"
AGL_WAIT_TIMEOUT="300"
REALM_WAIT_TIMEOUT="180"
STOP_EXISTING=1
INSTALL_AGL_VERIFIER=1

usage() {
    echo "Usage: ./run-agl-realm-attest-demo.sh [--realm-port <port>] [--agl-port <port>] [--iterations <N>] [--realm-bin <path>] [--agl-verify-port <port>] [--agl-net-mode user|tap] [--agl-net-host-ip <ip>] [--agl-net-guest-ip <ip>] [--agl-timeout <sec>] [--realm-timeout <sec>] [--no-install-agl-verifier] [--no-stop]"
}

port_to_tty() {
    case "$1" in
        5000|5001|5002|5003|5004)
            echo "/dev/ttyAMA$(($1 - 5000))"
            ;;
        *)
            echo "[ERROR] Unsupported telnet port: $1" >&2
            echo "        Expected one of: 5000, 5001, 5002, 5003, 5004" >&2
            exit 1
            ;;
    esac
}

require_file() {
    if [ ! -f "$1" ]; then
        echo "[ERROR] Missing required file: $1"
        exit 1
    fi
}

require_exec() {
    if [ ! -x "$1" ]; then
        echo "[ERROR] Missing executable script: $1"
        echo "        Upload scripts and run: chmod +x /root/*.sh /root/lkvm-static"
        exit 1
    fi
}

wait_for_log_pattern() {
    log_file="$1"
    timeout="$2"
    pattern="$3"
    label="$4"

    echo "[*] Waiting for $label"
    echo "    log    : $log_file"
    echo "    pattern: $pattern"
    echo "    timeout: ${timeout}s"

    elapsed=0
    while [ "$elapsed" -lt "$timeout" ]; do
        if [ -f "$log_file" ] && grep -E "$pattern" "$log_file" >/dev/null 2>&1; then
            echo "[*] Ready: $label"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done

    echo "[ERROR] Timed out waiting for $label"
    return 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --realm-port)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --realm-port"; exit 1; }
            REALM_PORT="$1"
            ;;
        --agl-port)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --agl-port"; exit 1; }
            AGL_PORT="$1"
            ;;
        --iterations)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --iterations"; exit 1; }
            ITERATIONS="$1"
            ;;
        --realm-bin)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --realm-bin"; exit 1; }
            REALM_BIN="$1"
            ;;
        --agl-verify-port)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --agl-verify-port"; exit 1; }
            AGL_VERIFY_PORT="$1"
            ;;
        --agl-net-mode)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --agl-net-mode"; exit 1; }
            AGL_NET_MODE="$1"
            ;;
        --agl-net-host-ip)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --agl-net-host-ip"; exit 1; }
            AGL_NET_HOST_IP="$1"
            ;;
        --agl-net-guest-ip)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --agl-net-guest-ip"; exit 1; }
            AGL_NET_GUEST_IP="$1"
            ;;
        --agl-timeout)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --agl-timeout"; exit 1; }
            AGL_WAIT_TIMEOUT="$1"
            ;;
        --realm-timeout)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --realm-timeout"; exit 1; }
            REALM_WAIT_TIMEOUT="$1"
            ;;
        --no-install-agl-verifier)
            INSTALL_AGL_VERIFIER=0
            ;;
        --no-stop)
            STOP_EXISTING=0
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
            echo "[ERROR] Unexpected argument: $1"
            usage
            exit 1
            ;;
    esac
    shift
done

if [ "$REALM_PORT" = "$AGL_PORT" ]; then
    echo "[ERROR] Realm and AGL cannot share the same telnet port."
    exit 1
fi

REALM_TTY="$(port_to_tty "$REALM_PORT")"
AGL_TTY="$(port_to_tty "$AGL_PORT")"

require_exec /root/lkvm-static
require_exec /root/run-vecu-agl.sh
require_exec /root/run-realm-shim-zephyr.sh
require_exec /root/stop-lkvm-vm.sh
require_file "$REALM_BIN"
require_file /root/guest-Image
require_file /root/agl.ext4

if [ "$STOP_EXISTING" -eq 1 ]; then
    /root/stop-lkvm-vm.sh agl-normal || true
    /root/stop-lkvm-vm.sh realm-vecu1 || true
fi

rm -f /root/.lkvm/agl-normal.sock
rm -f /root/.lkvm/realm-vecu1.sock
rm -f /root/agl-normal.log
rm -f /root/realm-vecu1.log /root/realm-vecu1.uart.log
rm -f /root/realm-vecu1.token.bin /root/realm-vecu1.token.meta

if [ "$INSTALL_AGL_VERIFIER" -eq 1 ]; then
    require_exec /root/install-agl-attest-verifier-rootfs.sh
    require_exec /root/agl_attest_verifier
    /root/install-agl-attest-verifier-rootfs.sh \
        --disk /root/agl.ext4 \
        --verifier /root/agl_attest_verifier \
        --host-ip "$AGL_NET_HOST_IP" \
        --port "$AGL_VERIFY_PORT"
fi

echo "[*] Starting AGL Normal VM"
echo "    console : telnet localhost $AGL_PORT"
echo "    serial  : $AGL_TTY"
echo "    net     : $AGL_NET_MODE"
echo "    host IP : $AGL_NET_HOST_IP"
echo "    guest IP: $AGL_NET_GUEST_IP"
/root/run-vecu-agl.sh \
    --serial-tty "$AGL_TTY" \
    --telnet-port "$AGL_PORT" \
    --net-mode "$AGL_NET_MODE" \
    --net-host-ip "$AGL_NET_HOST_IP" \
    --net-guest-ip "$AGL_NET_GUEST_IP"

echo ""
echo "[*] Starting Zephyr Realm VM"
echo "    shell   : telnet localhost $REALM_PORT"
echo "    serial  : $REALM_TTY"
echo "    AGL verifier port: $AGL_VERIFY_PORT"
LKVM_REALM_AGL_VERIFY=1 \
LKVM_REALM_AGL_VERIFY_PORT="$AGL_VERIFY_PORT" \
    /root/run-realm-shim-zephyr.sh --shell-port "$REALM_PORT" "$REALM_BIN"

wait_for_log_pattern /root/realm-vecu1.uart.log "$REALM_WAIT_TIMEOUT" \
    "Realm mini shell ready|realm:~\\$" \
    "Realm mini-shell"

wait_for_log_pattern /root/realm-vecu1.log "$AGL_WAIT_TIMEOUT" \
    "realm AGL verifier connected" \
    "AGL userspace attestation verifier"

echo ""
echo "[*] Demo is ready."
echo "    Host terminal 1: telnet localhost $AGL_PORT"
echo "    Host terminal 2: telnet localhost $REALM_PORT"
echo "    FVP log       : tail -f /root/realm-vecu1.log"
echo "    AGL log       : tail -f /root/agl-normal.uart.log"
echo ""
echo "[*] In the Realm shell, run:"
echo "    normal attest $ITERATIONS"
echo ""
echo "[*] Expected AGL verifier line:"
echo "    agl_csv,gen=...,token_size=1218,realm_gen_ns=...,parse_ns=...,hash_ns=...,total_ns=...,status=0x0,..."
