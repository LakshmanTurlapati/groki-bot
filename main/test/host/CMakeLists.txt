# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.16)
project(camera_service_host_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
find_package(Threads REQUIRED)
enable_testing()

add_executable(head_control_test test_head_control.cpp)
target_include_directories(head_control_test PRIVATE ${REPO_ROOT}/main)
target_compile_options(head_control_test PRIVATE -Wall -Wextra)
target_link_libraries(head_control_test PRIVATE Threads::Threads)
add_test(NAME head_control COMMAND head_control_test)

# Compile the production service unchanged. Only the camera hardware, board
# detection, HTTP sink registration, logging and RTOS delay are replaced.
foreach(camera_enabled IN ITEMS 0 1)
    set(target camera_service_test_${camera_enabled})
    add_executable(${target}
        ${REPO_ROOT}/main/camera_service.cpp
        fake_camera.cpp
        test_camera_service.cpp
    )
    target_compile_definitions(${target} PRIVATE CONFIG_STACKCHAN_CAMERA_ENABLED=${camera_enabled})
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
        ${REPO_ROOT}/main
        ${REPO_ROOT}/components/board/include
        ${REPO_ROOT}/components/avatar_vm/include
        ${REPO_ROOT}/components/tl_expected/expected/include
    )
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    target_link_libraries(${target} PRIVATE Threads::Threads)
endforeach()

add_test(NAME camera_disabled COMMAND camera_service_test_0 disabled)
# Each scenario runs in a separate process, matching the service's boot-lifetime
# globals without adding reset hooks to the firmware.
foreach(scenario IN ITEMS unsupported normal boot_recovery init_failure raw raw_restore_recovery
        raw_init_failure colorbar_enable_failure colorbar_disable_failure capture_failure register_failure
        register_recovery serialized_requests render_interlock)
    add_test(NAME camera_${scenario} COMMAND camera_service_test_1 ${scenario})
endforeach()
foreach(scenario IN ITEMS tracking_success tracking_paused tracking_pause_during tracking_failure tracking_timeout
        tracking_feedback_failure tracking_moved tracking_no_worker tracking_no_init tracking_no_servo tracking_finalize_timeout
        tracking_transform tracking_motion tracking_motion_blur tracking_motion_manual tracking_sleeping)
    add_test(NAME camera_${scenario} COMMAND camera_service_test_1 ${scenario})
endforeach()
