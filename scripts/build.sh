#!/bin/bash

SCRIPT_BUILD_DIR="$(dirname $(readlink -f "$0"))"

export GREEN='\033[0;32m'
export NC='\033[0m' # No Color
export RED='\033[0;31m'

log() {
    printf "${GREEN}$1${NC}\n"
}

log_error() {
    printf "${RED}$1${NC}\n"
}

hafnium_build() {
    log "===> build the hafnium ... "
    $SCRIPT_BUILD_DIR/build-hafnium.sh
}

optee-os_build() {
      log "===> build the optee-os ... "
      $SCRIPT_BUILD_DIR/build-optee-os.sh
}

rmm_build() {
     log "===> build the rmm ... "
      $SCRIPT_BUILD_DIR/build-rmm.sh
}

linux_build() {
    log "===> build the linux ... "
      $SCRIPT_BUILD_DIR/build-linux.sh
}

tf-a_build() {
     log "===> build the tf-a ... "
      $SCRIPT_BUILD_DIR/build-tf-a.sh
}

opencsd_build() {
    log "===> build the opencsd ... "
      $SCRIPT_BUILD_DIR/build-opencsd.sh
}

optee-client_build() {
    log "===> build the optee-client... "
      $SCRIPT_BUILD_DIR/build-optee-client.sh
}

optee-examples_build() {
        log "===> build the optee-examples... "
      $SCRIPT_BUILD_DIR/build-optee-examples.sh
}

ta_build() {
    log "===> build the trusted application... "
    $SCRIPT_BUILD_DIR/build-ta.sh
}

zephyr_build() {
    log "===> build Zephyr RTOS (Realm V-ECU)... "
    $SCRIPT_BUILD_DIR/build-zephyr.sh
}

vecu-zephyr_build() {
    log "===> build Zephyr Realm V-ECU app... "
    $SCRIPT_BUILD_DIR/build-vecu-zephyr.sh
}

agl_build() {
    log "===> build AGL (Automotive Grade Linux, Normal V-ECU)... "
    $SCRIPT_BUILD_DIR/build-agl.sh
}


buildall() {
   log "===> build the hafnium ... "
   $SCRIPT_BUILD_DIR/build-hafnium.sh

   log "===> build the optee-os ... "
   $SCRIPT_BUILD_DIR/build-optee-os.sh

   log "===> build the rmm ... "
   $SCRIPT_BUILD_DIR/build-rmm.sh

   log "===> build the linux ... "
   $SCRIPT_BUILD_DIR/build-linux.sh

   log "===> build the tf-a ... "
   $SCRIPT_BUILD_DIR/build-tf-a.sh

   log "===> build the opencsd ... "
   $SCRIPT_BUILD_DIR/build-opencsd.sh

   log "===> build the optee-client... "
   $SCRIPT_BUILD_DIR/build-optee-client.sh

   log "===> build the optee-examples... "
   $SCRIPT_BUILD_DIR/build-optee-examples.sh

   log "===> build the trusted application... "
   $SCRIPT_BUILD_DIR/build-ta.sh

   log "===> build Zephyr RTOS (Realm V-ECU)... "
   $SCRIPT_BUILD_DIR/build-zephyr.sh

   log "===> build Zephyr Realm V-ECU app... "
   $SCRIPT_BUILD_DIR/build-vecu-zephyr.sh

   log "===> build AGL (Automotive Grade Linux, Normal V-ECU)... "
   $SCRIPT_BUILD_DIR/build-agl.sh
}



if [ $# != 1 ]; then
    log_error "Usage: ./build.sh [all | hafnium | optee-os | rmm | linux | tf-a | opencsd | optee-client | optee-examples | ta | zephyr | vecu-zephyr | agl]"
    exit
fi

if [ $1 == "all" ]; then
    buildall
else 
    if [ "$(type -t $1_build)" == function ]; then 
        $1_build
    else 
        log_error "Usage: ./build.sh [all | hafnium | optee-os | rmm | linux | tf-a | opencsd | optee-client | optee-examples | ta | zephyr | vecu-zephyr | agl]"
    fi
fi
