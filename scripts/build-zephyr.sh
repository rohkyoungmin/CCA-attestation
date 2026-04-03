#!/bin/bash
# Build Zephyr RTOS as a Realm VM image (V-ECU 1)
# Source: dev_workspace/zephyr/  (initialized by: scripts/env.sh zephyr)
# Output: dev_workspace/zephyr/build/zephyr/zephyr.bin

SCRIPT_DIR="$(dirname $(readlink -f "$0"))"
source $SCRIPT_DIR/config.sh

ZEPHYR_SRC="$THIRD_PARTY_DIR/$ZEPHYR_DIR"
ZEPHYR_SDK="$THIRD_PARTY_DIR/$ZEPHYR_SDK_DIR"

# Sanity checks
if [ ! -d "$ZEPHYR_SRC" ]; then
    log_error "[zephyr] Source not found at $ZEPHYR_SRC"
    log_error "         Run: scripts/env.sh zephyr"
    exit 1
fi

if [ ! -d "$ZEPHYR_SDK" ]; then
    log_error "[zephyr] SDK not found at $ZEPHYR_SDK"
    log_error "         Run: scripts/env.sh zephyr"
    exit 1
fi

# Register Zephyr SDK (idempotent)
log "===> Registering Zephyr SDK..."
$ZEPHYR_SDK/setup.sh -t aarch64-zephyr-elf -c

# Set Zephyr environment
export ZEPHYR_BASE="$ZEPHYR_SRC/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$ZEPHYR_SDK"

cd $ZEPHYR_SRC

# Build echo_server sample as the V-ECU networking workload
# Board: qemu_cortex_a53 is the closest upstream ARM64 target for FVP AEMv8
# Note: For actual Realm VM execution, CONFIG_ARM_CCA_REALM=y will be added
#       once the realm_boot_init() patch is applied to Zephyr source.
log "===> Building Zephyr for qemu_cortex_a53 (ARM64)..."
west build -b qemu_cortex_a53 zephyr/samples/net/sockets/echo_server \
    --build-dir "$ZEPHYR_SRC/build" \
    -- \
    -DCONFIG_NETWORKING=y \
    -DCONFIG_NET_TCP=y \
    -DCONFIG_NET_SOCKETS=y \
    -DCONFIG_NET_LOG=y \
    -DCONFIG_PRINTK=y \
    -DCONFIG_STDOUT_CONSOLE=y

if [ $? -ne 0 ]; then
    log_error "[zephyr] Build failed."
    exit 1
fi

log "===> Zephyr build complete."
log "     Image: $ZEPHYR_OUT"
ls -lh "$ZEPHYR_OUT" 2>/dev/null || log_error "     Warning: zephyr.bin not found at expected path."
