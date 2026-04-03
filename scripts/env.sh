#!/bin/bash

# import variables
SCRIPT_DIR="$(dirname $(readlink -f "$0"))"
source $SCRIPT_DIR/config.sh 



prerequisite_fetch() {
    log "===> Downloading prerequisite packages..."
    sudo apt-get update
    sudo snap install cmake --classic
    sudo apt-get install make git build-essential libssl-dev flex curl \
            bison python3 python3-serial python3-pip device-tree-compiler \
            ninja-build libncurses5 xterm \
            libelf-dev gawk uuid-dev pkg-config \
            rsync python3-pyelftools python3-venv acpica-tools libglib2.0-dev libpixman-1-dev p7zip-full \
            chrpath diffstat lz4 locales -y
    sudo locale-gen en_US.UTF-8
    sudo update-locale LANG=en_US.UTF-8
    # needed for fvp network configuration
    sudo apt-get install libvirt-daemon-system libvirt-clients bridge-utils -y
    sudo pip install gdown
}

# cross compiling
cross_compile_fetch() {
    cd $THIRD_PARTY_DIR
    ## gcc
    if [[ "$(uname -m)" != "aarch64" ]]; then
        CC_DIR=$CROSS_COMPILE_DIR
    else
        CC_DIR=$CROSS_COMPILE_DIR_AARCH64
        CROSS_COMPILE_SRC=$CROSS_COMPILE_SRC_AARCH64
    fi

    if [ ! -d $CC_DIR ]; then
        log "===> Downloading the gcc cross compiler..."
        curl $CROSS_COMPILE_SRC | tar xfJ -
        if [ $? -ne 0 ]; then
            log_error "[cross-compiler] Error downloading the gcc cross compiler."
            exit
        fi
    fi

    ## gcc linux
    if [[ "$(uname -m)" != "aarch64" ]]; then
        CC_DIR=$CROSS_COMPILE_DIR_LINUX
    fi

    if [ ! -d $CC_DIR ]; then
        log "===> Downloading the gcc linux cross compiler..."
        curl $CROSS_COMPILE_SRC_LINUX | tar xfJ -
        if [ $? -ne 0 ]; then
            log_error "[cross-compiler] Error downloading the gcc linux cross compiler."
            exit
        fi
    fi

    ## gcc linux for optee-examples
    if [[ "$(uname -m)" != "aarch64" ]]; then
        CC_DIR=$CROSS_COMPILE_DIR_LINUX_TA
    fi

    if [ ! -d $CC_DIR ]; then
        log "===> Downloading the gcc 12 linux cross compiler..."
        curl $CROSS_COMPILE_SRC_LINUX_TA | tar xfJ -
        if [ $? -ne 0 ]; then
            log_error "[cross-compiler] Error downloading the gcc 12 linux cross compiler."
            exit
        fi
    fi

    ##clang
    if [[ "$(uname -m)" != "aarch64" ]]; then
        CC_DIR=$CROSS_COMPILE_CLANG_DIR
    else
        CC_DIR=$CROSS_COMPILE_CLANG_DIR_AARCH64
        CROSS_COMPILE_CLANG_SRC=$CROSS_COMPILE_CLANG_SRC_AARCH64
    fi

    if [ ! -d $CC_DIR ]; then
        log "===> Downloading the clang+llvm cross compiler..."
        wget -O- $CROSS_COMPILE_CLANG_SRC | tar xfJ -
        if [ $? -ne 0 ]; then
            log_error "[cross-compiler] Error downloading the clang+llvm  cross compiler."
            exit
        fi
    fi
}


# FVP model
fvp_model_fetch() {
    # chdir
    cd $THIRD_PARTY_DIR

    if [ ! -d $FVP_DIR ]; then
        if [[ "$(uname -m)" == "aarch64" ]]; then
            FVP_SRC=$FVP_SRC_AARCH64
            log "===> Downloading Armv-A Based AEM FVP (aarch64-host)..."
            wget -O- $FVP_SRC | tar xvzf -
        else
            log "===> Downloading Armv-A Based AEM FVP (x86-host)..."
            curl $FVP_SRC | tar xzvf -
        fi
        
        if [ $? -ne 0 ]; then
            log_error "[AEM FVP] Error downloading FVP."
            exit
        fi
        cp $FVP_CRYPTO_LIB $FVP_PLUGIN_DIR/Crypto.so
    fi
}

rootfs_fetch() {
    pushd $THIRD_PARTY_DIR

    if [ ! -f $PROJ_DIR/$ROOTFS ]; then
        log "===>  extracting the filesystem image ... "
        pushd $PROJ_DIR
        7z x $PROJ_DIR/$ROOTFS_SRC
        popd
    fi
    popd
}

hf_fetch() {
    pushd $THIRD_PARTY_DIR

    if [ ! -d $THIRD_PARTY_DIR/$HF ]; then
        log "===>  extracting the hafnium ... "
        git clone https://git.trustedfirmware.org/hafnium/hafnium.git
        pushd $THIRD_PARTY_DIR/$HF
        git checkout 4df5520d166a2955566ce8826a5254f7f7ff5fdc
        git submodule update --init --recursive
        git apply $PROJ_CONF_DIR/sc_hf.patch
        popd
    fi
   
    popd

}

rmm_fetch() {
    pushd $THIRD_PARTY_DIR

    if [ ! -d $THIRD_PARTY_DIR/$RMM ]; then
        log "===>  extracting the rmm ... "
        git clone https://git.trustedfirmware.org/TF-RMM/tf-rmm.git
        pushd $THIRD_PARTY_DIR/$RMM
        git checkout 61bdf4e8418f8c34def91a0ea6e4057f535e211a
        git submodule update --init --recursive
        popd
    fi
   
    popd

}

linux_fetch() {
    pushd $SRC_DIR/linux
    cp $PROJ_CONF_DIR/linux.config .config
    popd
}

opencsd_fetch() {
    pushd $THIRD_PARTY_DIR

    if [ ! -d $THIRD_PARTY_DIR/OpenCSD ]; then
        log "===>  extracting the opencsd ... "
        git clone https://github.com/Linaro/OpenCSD.git
    fi
    popd
}

zephyr_fetch() {
    pushd $THIRD_PARTY_DIR

    # Zephyr SDK (cross-toolchain for aarch64-zephyr-elf)
    if [ ! -d $ZEPHYR_SDK_DIR ]; then
        log "===> Downloading Zephyr SDK ${ZEPHYR_SDK_VER}..."
        SDK_TAR="zephyr-sdk-${ZEPHYR_SDK_VER}_linux-x86_64.tar.xz"
        wget -q "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VER}/${SDK_TAR}"
        if [ $? -ne 0 ]; then
            log_error "[zephyr] Error downloading Zephyr SDK."
            popd; exit 1
        fi
        tar xf $SDK_TAR
        rm -f $SDK_TAR
    fi

    # Zephyr source via west
    if [ ! -d $ZEPHYR_DIR ]; then
        log "===> Installing west and initializing Zephyr workspace (${ZEPHYR_VERSION})..."
        pip3 install --user west
        west init $ZEPHYR_DIR --mr $ZEPHYR_VERSION
        if [ $? -ne 0 ]; then
            log_error "[zephyr] Error: west init failed."
            popd; exit 1
        fi
        pushd $ZEPHYR_DIR
        log "===> Running west update (this may take a few minutes)..."
        west update
        if [ $? -ne 0 ]; then
            log_error "[zephyr] Error: west update failed."
            popd; popd; exit 1
        fi
        popd
    fi

    popd
}

agl_fetch() {
    pushd $THIRD_PARTY_DIR

    # Install repo tool if not present
    if ! command -v repo &> /dev/null; then
        log "===> Installing repo tool..."
        mkdir -p ~/.bin
        curl -s https://storage.googleapis.com/git-repo-downloads/repo > ~/.bin/repo
        chmod +x ~/.bin/repo
        export PATH="${HOME}/.bin:${PATH}"
    fi

    if [ ! -d $AGL_DIR ]; then
        log "===> Initializing AGL workspace (branch: ${AGL_BRANCH})..."
        mkdir -p $AGL_DIR
        pushd $AGL_DIR
        repo init -u https://gerrit.automotivelinux.org/gerrit/AGL/AGL-repo \
                  -b $AGL_BRANCH
        if [ $? -ne 0 ]; then
            log_error "[agl] Error: repo init failed."
            popd; popd; exit 1
        fi
        # Skip meta-tensorflow: broken SHA1 in salmon manifest
        mkdir -p .repo/local_manifests
        cat > .repo/local_manifests/skip-tensorflow.xml << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<manifest>
  <remove-project name="meta-tensorflow"/>
</manifest>
EOF
        log "===> Running repo sync (this may take 30-60 minutes)..."
        repo sync -j$(nproc)
        if [ $? -ne 0 ]; then
            log_error "[agl] Error: repo sync failed."
            popd; popd; exit 1
        fi
        popd
    fi

    popd
}

third_parties_fetchall() {
    cd $THIRD_PARTY_DIR
    prerequisite_fetch
    cross_compile_fetch
    fvp_model_fetch
    rootfs_fetch
    hf_fetch
    rmm_fetch
    linux_fetch
    opencsd_fetch
    # Note: zephyr_fetch and agl_fetch are intentionally excluded from 'all'
    # because they are large and time-consuming. Run them separately:
    #   scripts/env.sh zephyr
    #   scripts/env.sh agl
}

if [ ! -d $THIRD_PARTY_DIR ]; then
    log "==> Creating directory $THIRD_PARTY_DIR"
    mkdir -p $THIRD_PARTY_DIR
fi

if [ ! -d $LOG_DIR ]; then
    log "==> Creating directory $LOG_DIR"
    mkdir -p $LOG_DIR
fi

if [ $# != 1 ]; then
    log_error "Usage: ./env_fetch.sh [all | cross_compile | prerequisite | fvp_model"
    exit
fi

if [ $1 == "all" ]; then
    third_parties_fetchall
else 
    if [ "$(type -t $1_fetch)" == function ]; then 
        $1_fetch
    else 
        log_error "Usage: ./env_fetch.sh [all | cross_compile | prerequisite | fvp_model"
    fi
fi
