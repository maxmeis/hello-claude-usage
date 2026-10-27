cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/.deps/sensecap-sdk/components")
if(NOT CLAUDE_RAW_DISPLAY_BUILD)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/.deps/esp-protocols/components/mdns")
endif()
set(COMPONENTS main)
if(CLAUDE_RAW_DISPLAY_BUILD)
    set(PROJECT_VER "0.4.2-vendor-display-check")
else()
    set(PROJECT_VER "0.13.0-mdns")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sensecap_oauth_probe)
if(CLAUDE_RAW_DISPLAY_BUILD AND NOT CONFIG_CLAUDE_DISPLAY_DIAGNOSTIC)
    message(FATAL_ERROR "Raw display build requires CONFIG_CLAUDE_DISPLAY_DIAGNOSTIC=y")
endif()
# Preserve the GX module's vendor COLCTRL/COLMOD and input-pad mapping.
# The ESP's 16-bit framebuffer does not establish the panel's RGB bus format.
# Upstream board logging predates IDF 5.1's uint32_t format checks.
# Retain diagnostics without disabling warnings in our application.
foreach(vendor_component bsp bus i2c_devices iot_button)
    idf_component_get_property(vendor_lib ${vendor_component} COMPONENT_LIB)
    target_compile_options(${vendor_lib} PRIVATE "-Wno-error=format")
endforeach()
# Retain the pinned SDK's GT1151 touch-driver warning without failing the build.
idf_component_get_property(i2c_lib i2c_devices COMPONENT_LIB)
target_compile_options(${i2c_lib} PRIVATE "-Wno-error=maybe-uninitialized")
