# SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
# SPDX-License-Identifier: BSL-1.0
#
# Host-side tests for avatar animation helpers (ESP-IDF independent).

cmake_minimum_required(VERSION 3.16)
project(avatar_host_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(AVATAR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(REPO_ROOT ${AVATAR_ROOT}/../..)
set(VM_ROOT ${REPO_ROOT}/components/avatar_vm)

enable_testing()

add_executable(avatar_test_expression_controller test_expression_controller.cpp)
target_include_directories(avatar_test_expression_controller PRIVATE
    ${AVATAR_ROOT}/include
    ${VM_ROOT}/include
)
target_compile_options(avatar_test_expression_controller PRIVATE -Wall -Wextra)
add_test(NAME expression_controller COMMAND avatar_test_expression_controller)

# Multi-producer overlay publish tests (SharedState lives in main/).
add_executable(avatar_test_overlay_publish test_overlay_publish.cpp)
target_include_directories(avatar_test_overlay_publish PRIVATE
    ${AVATAR_ROOT}/include
    ${VM_ROOT}/include
    ${REPO_ROOT}/main
)
target_link_libraries(avatar_test_overlay_publish PRIVATE pthread)
target_compile_options(avatar_test_overlay_publish PRIVATE -Wall -Wextra)
add_test(NAME overlay_publish COMMAND avatar_test_overlay_publish)

# Pill face compositor (main/pill_face.hpp): keyframes, transitions, budget.
add_executable(avatar_test_pill_face test_pill_face.cpp)
target_include_directories(avatar_test_pill_face PRIVATE
    ${AVATAR_ROOT}/include
    ${VM_ROOT}/include
    ${REPO_ROOT}/main
)
target_compile_options(avatar_test_pill_face PRIVATE -Wall -Wextra)
add_test(NAME pill_face COMMAND avatar_test_pill_face)

# The real face pipeline on the host: compositor -> VM -> compiled
# assets/grok_face.avdsl -> software canvas. `face_preview render` writes PPM
# frames for comparing against the design mocks.
add_library(avatar_vm_host STATIC
    ${VM_ROOT}/decoder.cpp
    ${VM_ROOT}/vm.cpp
)
target_include_directories(avatar_vm_host PUBLIC
    ${VM_ROOT}/include
    ${REPO_ROOT}/components/tl_expected/expected/include
    ${REPO_ROOT}/wasm/shim
)
add_executable(face_preview face_preview.cpp)
target_include_directories(face_preview PRIVATE
    ${AVATAR_ROOT}/include
    ${REPO_ROOT}/main
)
target_link_libraries(face_preview PRIVATE avatar_vm_host)
target_compile_options(face_preview PRIVATE -Wall -Wextra)

find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    set(_FACE_DSL ${REPO_ROOT}/assets/grok_face.avdsl)
    set(_FACE_AVBC ${CMAKE_CURRENT_BINARY_DIR}/grok_face.avbc)
    file(GLOB _DSL_JS ${REPO_ROOT}/tools/avatar_dsl/*.js ${REPO_ROOT}/tools/avatar_dsl/*.mjs)
    add_custom_command(
        OUTPUT ${_FACE_AVBC}
        COMMAND ${NODE_EXECUTABLE} ${REPO_ROOT}/tools/avatar_dsl/cli.mjs ${_FACE_DSL} ${_FACE_AVBC}
        DEPENDS ${_FACE_DSL} ${_DSL_JS}
        VERBATIM
    )
    add_custom_target(grok_face_avbc ALL DEPENDS ${_FACE_AVBC})
    add_test(NAME face_bytecode COMMAND face_preview check ${_FACE_AVBC})
else()
    message(STATUS "node not found: skipping the face_bytecode test")
endif()
