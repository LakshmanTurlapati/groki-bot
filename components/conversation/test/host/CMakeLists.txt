# SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
# SPDX-License-Identifier: BSL-1.0
#
# Host tests for the XiaoZhi control-message dispatch (control_dispatch.hpp):
# the gateway's `head` message and the pass-through of every other type.
#
# cJSON comes from ESP-IDF when IDF_PATH is set (the same copy the firmware
# links); otherwise CMake downloads the matching upstream release.

cmake_minimum_required(VERSION 3.18)
project(conversation_host_test C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CONVERSATION_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

if(DEFINED ENV{IDF_PATH} AND EXISTS "$ENV{IDF_PATH}/components/json/cJSON/cJSON.c")
    set(CJSON_DIR "$ENV{IDF_PATH}/components/json/cJSON")
else()
    include(FetchContent)
    # SOURCE_SUBDIR points at a directory without a CMakeLists.txt, so
    # MakeAvailable only downloads; cJSON.c is compiled below.
    FetchContent_Declare(cjson
        URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR no-cmake-build
    )
    FetchContent_MakeAvailable(cjson)
    set(CJSON_DIR ${cjson_SOURCE_DIR})
endif()

add_library(cjson_host STATIC ${CJSON_DIR}/cJSON.c)
target_include_directories(cjson_host PUBLIC ${CJSON_DIR})

enable_testing()

add_executable(conversation_test_control_dispatch test_control_dispatch.cpp)
target_include_directories(conversation_test_control_dispatch PRIVATE ${CONVERSATION_ROOT}/include)
target_link_libraries(conversation_test_control_dispatch PRIVATE cjson_host)
target_compile_options(conversation_test_control_dispatch PRIVATE -Wall -Wextra)
add_test(NAME control_dispatch COMMAND conversation_test_control_dispatch)

add_executable(conversation_test_usb_voice_framer
    test_usb_voice_framer.cpp
    ${CONVERSATION_ROOT}/usb_voice_framer.cpp
    ${CONVERSATION_ROOT}/usb_voice_port.cpp
)
target_include_directories(conversation_test_usb_voice_framer PRIVATE ${CONVERSATION_ROOT}/include)
target_compile_options(conversation_test_usb_voice_framer PRIVATE -Wall -Wextra)
add_test(NAME usb_voice_framer COMMAND conversation_test_usb_voice_framer)
