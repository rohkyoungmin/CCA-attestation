#!/bin/bash
# Download and sync the AGL workspace used by this project.
# This is a standalone version of the AGL fetch logic in scripts/env.sh.

set -e

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
source "$SCRIPT_DIR/config.sh"

ensure_repo_tool() {
    if command -v repo >/dev/null 2>&1; then
        return 0
    fi

    log "===> Installing repo tool..."
    mkdir -p "$HOME/.bin"
    curl -s https://storage.googleapis.com/git-repo-downloads/repo > "$HOME/.bin/repo"
    chmod +x "$HOME/.bin/repo"
    export PATH="$HOME/.bin:$PATH"

    if ! command -v repo >/dev/null 2>&1; then
        log_error "[agl] repo tool installation failed."
        exit 1
    fi
}

main() {
    mkdir -p "$THIRD_PARTY_DIR"
    cd "$THIRD_PARTY_DIR"

    ensure_repo_tool

    if [ ! -d "$AGL_DIR/.repo" ]; then
        log "===> Initializing AGL workspace (branch: ${AGL_BRANCH})..."
        mkdir -p "$AGL_DIR"
        pushd "$AGL_DIR" >/dev/null

        repo init -u https://gerrit.automotivelinux.org/gerrit/AGL/AGL-repo \
                  -b "$AGL_BRANCH"

        # Skip meta-tensorflow: broken SHA1 in salmon manifest
        mkdir -p .repo/local_manifests
        cat > .repo/local_manifests/skip-tensorflow.xml <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<manifest>
  <remove-project name="meta-tensorflow"/>
</manifest>
EOF
        popd >/dev/null
    fi

    pushd "$AGL_DIR" >/dev/null
    log "===> Running repo sync for AGL workspace..."
    repo sync -j"$(nproc)"
    popd >/dev/null

    log "===> AGL workspace is ready at: $THIRD_PARTY_DIR/$AGL_DIR"
}

main "$@"
