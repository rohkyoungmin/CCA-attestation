#!/bin/bash
# Build AGL (Automotive Grade Linux) as a Normal VM image (V-ECU 2)
# Source: dev_workspace/agl-workspace/  (initialized by: scripts/env.sh agl)
# Output: dev_workspace/agl-workspace/build-agl-arm64/tmp/deploy/images/qemuarm64/
#
# Build time: ~1-3 hours (initial), ~10-30 min (incremental)
# Image target: agl-image-minimal (salmon branch — agl-demo-platform was removed)

SCRIPT_DIR="$(dirname $(readlink -f "$0"))"
source $SCRIPT_DIR/config.sh

AGL_SRC="$THIRD_PARTY_DIR/$AGL_DIR"

# Sanity check
if [ ! -d "$AGL_SRC" ]; then
    log_error "[agl] Source not found at $AGL_SRC"
    log_error "      Run: scripts/env.sh agl"
    exit 1
fi

if [ ! -f "$AGL_SRC/meta-agl/scripts/aglsetup.sh" ]; then
    log_error "[agl] meta-agl not found. repo sync may be incomplete."
    log_error "      Run: scripts/env.sh agl"
    exit 1
fi

cd $AGL_SRC

# Initialize AGL build environment
# salmon branch features: agl-devel (dev/debug tools), agl-virt (KVM/QEMU support)
# NOTE: agl-demo feature was removed in salmon; use agl-image-minimal target
log "===> Initializing AGL build environment (machine: $AGL_MACHINE, build: $AGL_BUILD_DIR)..."
source meta-agl/scripts/aglsetup.sh \
    -m $AGL_MACHINE \
    -b $AGL_BUILD_DIR \
    agl-devel

if [ $? -ne 0 ]; then
    log_error "[agl] aglsetup.sh failed."
    exit 1
fi

# Build minimal image (salmon branch: agl-demo-platform renamed to agl-image-minimal)
log "===> Starting bitbake build (agl-image-minimal)..."
log "     This may take 1-3 hours on first build."
bitbake agl-image-minimal

if [ $? -ne 0 ]; then
    log_error "[agl] bitbake build failed."
    exit 1
fi

log "===> AGL build complete."
log "     Image : $AGL_IMG"
log "     Kernel: $AGL_KERNEL"
ls -lh "$AGL_IMG"    2>/dev/null || log_error "     Warning: AGL image not found at expected path."
ls -lh "$AGL_KERNEL" 2>/dev/null || log_error "     Warning: AGL kernel not found at expected path."
