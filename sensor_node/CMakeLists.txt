# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Custom DTS bindings: add CSSE4011-PROJECT as DTS root only when building
# standalone (outside the firmware west workspace). Inside the workspace,
# firmware/dts/bindings/ is already on the search path — adding it again
# causes a duplicate-compatible conflict.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../dts/bindings")
    list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
endif()

list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../drivers)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensor_node)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

zephyr_nanopb_sources(app ../nanopb_driver/src/sensor_config.proto ../nanopb_driver/src/sensor_info.proto)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
target_include_directories(app PRIVATE include ${CMAKE_CURRENT_SOURCE_DIR}/../nanopb_driver/include)