# GoogleTest 获取（DEPENDENCIES.md / DECISIONS D-10）：
# 优先系统包 find_package(GTest)；失败则 FetchContent 官方 release。
# 仅测试目标使用；GTest 头以 SYSTEM 引入，警告矩阵不适用于第三方代码。

include(FetchContent)

function(fr_get_gtest)
    find_package(GTest QUIET)
    if(GTEST_FOUND)
        message(STATUS "ForgeRelay: using system GTest")
        return()
    endif()

    message(STATUS "ForgeRelay: fetching GoogleTest v1.16.0 (FetchContent)")
    set(FORCE_COLORED_OUTPUT ON CACHE INTERNAL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.16.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)

    foreach(_gt_target gtest gtest_main)
        if(TARGET ${_gt_target})
            get_target_property(_gt_type ${_gt_target} TYPE)
            if(NOT _gt_type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options(${_gt_target} PRIVATE
                    $<$<CXX_COMPILER_ID:GNU,Clang>:-w>)
            endif()
        endif()
    endforeach()
endfunction()
