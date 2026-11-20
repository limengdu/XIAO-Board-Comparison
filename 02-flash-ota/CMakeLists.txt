cmake_minimum_required(VERSION 3.22)

# Firmware version; build V2 with -DPROJECT_VER=2.0.0
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "1.0.0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(flash_ota_bench)
