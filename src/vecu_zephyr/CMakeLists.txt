cmake_minimum_required(VERSION 3.20.0)

# Board search path: local boards/ directory
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(vecu_zephyr_realm)

target_sources(app PRIVATE
    src/main.c
    src/rsi.c
    src/rsi_asm.S
)

target_include_directories(app PRIVATE src)
