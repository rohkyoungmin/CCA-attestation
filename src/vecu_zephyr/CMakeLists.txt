cmake_minimum_required(VERSION 3.20.0)

# Board search path: local boards/ directory
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND SOC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(vecu_zephyr_realm)

zephyr_linker_sources(SECTIONS realm_payload_entry.ld)

target_sources(app PRIVATE
    src/payload_entry.S
    src/payload_mmu.c
    src/rsi.c
    src/rsi_asm.S
    src/main.c
)

if(CONFIG_SERIAL)
    target_sources(app PRIVATE src/realm_uart.c)
endif()

if(DEFINED REALM_MINI_SHELL AND REALM_MINI_SHELL)
    target_compile_definitions(app PRIVATE REALM_MINI_SHELL=1)
    target_sources(app PRIVATE src/realm_timer.c)
endif()

target_link_options(app PRIVATE
    "-Wl,-e,realm_payload_entry"
)
