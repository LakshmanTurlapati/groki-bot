cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stackchan_idf)

# M5Unified must use the patched local graphics driver. A registry copy can
# otherwise win symbol resolution even when the local component also builds.
idf_build_get_property(_build_components BUILD_COMPONENTS)
if(m5stack__m5gfx IN_LIST _build_components)
    message(FATAL_ERROR "Duplicate M5GFX dependency: run tools/apply-m5-patches.sh before building")
endif()

# The CoreS3 sensor shares the internal I2C bus with touch, power and audio.
# Use M5Unified's bus and transaction mutex for SCCB as well. A separate IDF
# I2C driver would reconfigure the same controller behind M5Unified's back.
if(CONFIG_STACKCHAN_CAMERA_ENABLED)
    idf_component_get_property(_camera_lib espressif__esp32-camera COMPONENT_LIB)
    idf_component_get_property(_m5unified_lib M5Unified COMPONENT_LIB)
    get_target_property(_camera_sources ${_camera_lib} SOURCES)
    set(_camera_sources_before "${_camera_sources}")
    list(FILTER _camera_sources EXCLUDE REGEX "(^|/)sccb(-ng)?\\.c$")
    if(_camera_sources STREQUAL _camera_sources_before)
        message(FATAL_ERROR "Camera SCCB source changed; check the M5Unified bus adapter")
    endif()
    set_property(TARGET ${_camera_lib} PROPERTY SOURCES "${_camera_sources}")
    target_sources(${_camera_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/components/board/camera_sccb.cpp")
    target_link_libraries(${_camera_lib} PRIVATE ${_m5unified_lib})
    target_compile_features(${_camera_lib} PRIVATE cxx_std_20)
endif()

# Render-hot submodule: keep -O2 under the global -Os (OTA size budget).
idf_component_get_property(_m5gfx_lib M5GFX COMPONENT_LIB)
if(_m5gfx_lib)
    target_compile_options(${_m5gfx_lib} PRIVATE -O2)
endif()
