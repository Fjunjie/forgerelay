# ForgeRelay sanitizer 注入（TEST 动态检查，需求 §13）。
# 通过缓存变量 FR_SANITIZE 启用，如：-DFR_SANITIZE="address,undefined"。
# -fno-sanitize-recover 让运行时违规直接终止，由 ctest 捕获为失败。

set(FR_SANITIZE_FLAGS "")
if(FR_SANITIZE)
    set(FR_SANITIZE_FLAGS "-fsanitize=${FR_SANITIZE}" "-fno-sanitize-recover=all" "-fno-omit-frame-pointer")
endif()

# fr_apply_sanitizers(<target> [LINK_ONLY])：编译与链接选项同时注入。
# 静态库目标也需要链接选项，最终可执行文件才能拿到 sanitizer 运行时。
function(fr_apply_sanitizers TARGET)
    if(NOT FR_SANITIZE_FLAGS)
        return()
    endif()
    target_compile_options(${TARGET} PRIVATE ${FR_SANITIZE_FLAGS})
    target_link_options(${TARGET} PRIVATE ${FR_SANITIZE_FLAGS})
endfunction()
