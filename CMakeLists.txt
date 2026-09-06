cmake_minimum_required(VERSION 3.25)

project(forgerelay
    VERSION 0.1.0
    DESCRIPTION "ForgeRelay lightweight artifact cache and distribution service"
    LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(warnings)
include(sanitizers)

option(FR_BUILD_TESTS "Build ForgeRelay tests" ON)
option(FR_WERROR "Treat warnings as errors in ForgeRelay targets" ON)

# 子目录存在性守卫：允许“仅骨架”或“仅核心库”的最小构建（如离线无 GTest 时
# 配 -DFR_BUILD_TESTS=OFF）。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/core_c/CMakeLists.txt")
    add_subdirectory(src/core_c)
endif()

if(FR_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    include(GetGoogleTest)
    fr_get_gtest()
    enable_testing()
    add_subdirectory(tests)
endif()
