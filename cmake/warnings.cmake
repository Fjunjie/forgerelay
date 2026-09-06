# ForgeRelay 编译警告矩阵（需求 §12.1）。
# 仅作用于自研目标；第三方目标（GTest 等）不调用本函数。
# 缓存变量 FR_WERROR（默认 ON）控制自研代码警告是否视为错误。
#
# fr_apply_warnings(<target> <c|cxx|both>)

function(fr_apply_warnings TARGET LANG)
    set(FR_WARNING_FLAGS
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        -Wformat=2 -Wimplicit-fallthrough
        -Wcast-qual -Wdouble-promotion -Wundef)
    if(FR_WERROR)
        list(APPEND FR_WARNING_FLAGS -Werror)
    endif()

    if(LANG STREQUAL "c" OR LANG STREQUAL "both")
        target_compile_options(${TARGET} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:${FR_WARNING_FLAGS}
            -Wstrict-prototypes -Wmissing-prototypes>)
    endif()
    if(LANG STREQUAL "cxx" OR LANG STREQUAL "both")
        target_compile_options(${TARGET} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:${FR_WARNING_FLAGS}>)
    endif()
endfunction()
