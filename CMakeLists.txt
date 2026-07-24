
# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

file (STRINGS version.txt BUILD_NUMBER)
set(PROJECT_VER "${BUILD_NUMBER}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt")
add_compile_options(-Wno-missing-field-initializers)
add_compile_definitions(BUILD_NUMBER="${BUILD_NUMBER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(plai-meshcore)
