cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../../components/gpsp_font" "../../components/gpsp_display"
    "../../managed_components/espressif__usb")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(usb_bt_check)
# ESP-IDF 6.1 Bluedroid trips GCC string truncation checks.
idf_component_get_property(bt_lib bt COMPONENT_LIB)
target_compile_options(${bt_lib} PRIVATE -Wno-error=stringop-truncation)
# Skip the optional packet type and duplicate link policy setup after
# each link comes up; one adapter refused the packet type change.
target_compile_definitions(${bt_lib} PRIVATE BTM_BYPASS_EXTRA_ACL_SETUP=1)
