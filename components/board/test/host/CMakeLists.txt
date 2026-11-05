# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.16)
project(camera_host_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_executable(camera_test
    test_camera.cpp
    ${BOARD_ROOT}/camera_gc0308.cpp
)
target_include_directories(camera_test PRIVATE
    ${BOARD_ROOT}/include
    ${BOARD_ROOT}/../tl_expected/expected/include
    ${CMAKE_CURRENT_SOURCE_DIR}/idf_stub
)
target_compile_options(camera_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME camera COMMAND camera_test)

add_executable(led_strip_test
    test_led_strip.cpp
    ${BOARD_ROOT}/io_expander_py32.cpp
    ${BOARD_ROOT}/led_strip.cpp
)
target_include_directories(led_strip_test PRIVATE
    ${BOARD_ROOT}/include
    ${BOARD_ROOT}/../tl_expected/expected/include
    ${CMAKE_CURRENT_SOURCE_DIR}/idf_stub
)
target_compile_options(led_strip_test PRIVATE -Wall -Wextra)
add_test(NAME led_strip COMMAND led_strip_test)

add_executable(camera_sccb_test test_sccb.cpp ${BOARD_ROOT}/camera_sccb.cpp)
target_include_directories(camera_sccb_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/idf_stub)
target_compile_options(camera_sccb_test PRIVATE -Wall -Wextra)
add_test(NAME camera_sccb COMMAND camera_sccb_test)

# Compile the actual low-level M5GFX routines against simulated registers.
# This checks the cleanup contract used by the camera's shared-bus adapter.
set(M5GFX_ROOT ${BOARD_ROOT}/../M5GFX)
set(M5GFX_I2C_SOURCE ${M5GFX_ROOT}/src/lgfx/v1/platforms/esp32/common.inl)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${M5GFX_I2C_SOURCE})
file(READ ${M5GFX_I2C_SOURCE} M5GFX_I2C_TEXT)
function(extract_i2c_function start_marker end_marker output_name)
    string(FIND "${M5GFX_I2C_TEXT}" "${start_marker}" start)
    string(FIND "${M5GFX_I2C_TEXT}" "${end_marker}" end)
    if(start LESS 0 OR end LESS_EQUAL start)
        message(FATAL_ERROR "M5GFX I2C source changed; update the host function boundaries")
    endif()
    math(EXPR length "${end} - ${start}")
    string(SUBSTRING "${M5GFX_I2C_TEXT}" ${start} ${length} body)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${output_name} "${body}")
endfunction()
extract_i2c_function("    static constexpr uint32_t i2c_stall_limit_us ="
                     "    cpp::result<void, error_t> release(" m5gfx_i2c_wait.inc)
extract_i2c_function("    cpp::result<void, error_t> readBytes("
                     "    cpp::result<void, error_t> transactionWrite(" m5gfx_i2c_read.inc)
extract_i2c_function("    cpp::result<void, error_t> endTransaction("
                     "    bool busy(int i2c_port)" m5gfx_i2c_end.inc)
add_executable(camera_i2c_cleanup_test test_i2c_cleanup.cpp)
set_target_properties(camera_i2c_cleanup_test PROPERTIES CXX_EXTENSIONS ON)
target_compile_definitions(camera_i2c_cleanup_test PRIVATE CONFIG_IDF_TARGET=1 CONFIG_IDF_TARGET_ESP32S3=1)
target_include_directories(camera_i2c_cleanup_test PRIVATE ${M5GFX_ROOT}/src ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(camera_i2c_cleanup_test PRIVATE -Wall -Wextra)
add_test(NAME camera_i2c_cleanup COMMAND camera_i2c_cleanup_test)
