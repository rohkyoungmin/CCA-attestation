#!/bin/bash
# Install the AGL attestation verifier into /root/agl.ext4 from FVP Linux.

set -e

AGL_DISK="/root/agl.ext4"
VERIFIER="/root/agl_attest_verifier"
MNT="/mnt/agl-rootfs"
HOST_IP="192.168.33.1"
PORT="7777"

usage() {
    echo "Usage: ./install-agl-attest-verifier-rootfs.sh [--disk /root/agl.ext4] [--verifier /root/agl_attest_verifier] [--host-ip 192.168.33.1] [--port 7777]"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --disk)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --disk"; exit 1; }
            AGL_DISK="$1"
            ;;
        --verifier)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --verifier"; exit 1; }
            VERIFIER="$1"
            ;;
        --host-ip)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --host-ip"; exit 1; }
            HOST_IP="$1"
            ;;
        --port)
            shift
            [ -n "$1" ] || { echo "[ERROR] Missing value for --port"; exit 1; }
            PORT="$1"
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

if [ ! -f "$AGL_DISK" ]; then
    echo "[ERROR] Missing AGL disk: $AGL_DISK"
    exit 1
fi

if [ ! -x "$VERIFIER" ]; then
    echo "[ERROR] Missing verifier binary: $VERIFIER"
    exit 1
fi

mkdir -p "$MNT"

if mount | grep -q " $MNT "; then
    umount "$MNT"
fi

echo "[*] Mounting AGL rootfs: $AGL_DISK"
mount -o loop "$AGL_DISK" "$MNT"

cleanup() {
    sync
    umount "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

install -m 0755 "$VERIFIER" "$MNT/usr/bin/agl_attest_verifier"

mkdir -p "$MNT/etc/systemd/system"
cat >"$MNT/etc/systemd/system/agl-attest-verifier.service" <<EOF_SERVICE
[Unit]
Description=AGL CCA Attestation Verifier
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/agl_attest_verifier --host $HOST_IP --port $PORT
Restart=always
RestartSec=1
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
EOF_SERVICE

mkdir -p "$MNT/etc/systemd/system/multi-user.target.wants"
ln -sf ../agl-attest-verifier.service \
    "$MNT/etc/systemd/system/multi-user.target.wants/agl-attest-verifier.service"

mkdir -p "$MNT/etc/init.d"
cat >"$MNT/etc/init.d/S99agl-attest-verifier" <<EOF_INIT
#!/bin/sh
case "\$1" in
  start|"")
    /usr/bin/agl_attest_verifier --host $HOST_IP --port $PORT &
    ;;
  stop)
    killall agl_attest_verifier 2>/dev/null || true
    ;;
esac
EOF_INIT
chmod +x "$MNT/etc/init.d/S99agl-attest-verifier"

echo "[*] Installed AGL verifier into rootfs"
echo "    binary : /usr/bin/agl_attest_verifier"
echo "    service: agl-attest-verifier.service"
