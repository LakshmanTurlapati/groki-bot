# SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
# SPDX-License-Identifier: BSL-1.0
#
# Host tests for groki_motion nadenade, LED packing, and LCD/IMU face input.

cmake_minimum_required(VERSION 3.16)
project(groki_motion_host_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(MOTION_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(REPO_ROOT ${MOTION_ROOT}/../..)

add_library(groki_motion_host STATIC
    ${MOTION_ROOT}/motion.cpp
    ${MOTION_ROOT}/face_input.cpp
)
target_include_directories(groki_motion_host
    PUBLIC ${MOTION_ROOT}/include
)
target_compile_options(groki_motion_host PRIVATE -Wall -Wextra)

enable_testing()

add_executable(groki_motion_test_nadenade test_nadenade.cpp)
target_link_libraries(groki_motion_test_nadenade PRIVATE groki_motion_host)
target_compile_options(groki_motion_test_nadenade PRIVATE -Wall -Wextra)
add_test(NAME nadenade COMMAND groki_motion_test_nadenade)

add_executable(groki_motion_test_spring test_spring.cpp)
target_link_libraries(groki_motion_test_spring PRIVATE groki_motion_host)
target_compile_options(groki_motion_test_spring PRIVATE -Wall -Wextra)
add_test(NAME spring COMMAND groki_motion_test_spring)

add_executable(groki_motion_test_led_runtime test_led_runtime.cpp)
target_include_directories(groki_motion_test_led_runtime PRIVATE ${REPO_ROOT}/main)
target_compile_options(groki_motion_test_led_runtime PRIVATE -Wall -Wextra)
add_test(NAME led_runtime COMMAND groki_motion_test_led_runtime)

add_executable(groki_motion_test_face_input test_face_input.cpp)
target_link_libraries(groki_motion_test_face_input PRIVATE groki_motion_host)
# expression_for lives in the assembly layer; avatar_vm for the enum.
target_include_directories(groki_motion_test_face_input PRIVATE
    ${REPO_ROOT}/main
    ${REPO_ROOT}/components/avatar_vm/include)
target_compile_options(groki_motion_test_face_input PRIVATE -Wall -Wextra)
add_test(NAME face_input COMMAND groki_motion_test_face_input)
