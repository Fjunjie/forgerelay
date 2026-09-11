# toml++ 获取（DECISIONS D-21）：§12.2 允许 tomlc99 或 toml++；选 toml++（MIT），
# 因其有正式 release tag（可复现获取）且为单头库，集成开销最小。
# 头文件以 SYSTEM 方式引入，警告矩阵不适用（仓库不提交第三方源码副本）。

include(FetchContent)

function(fr_get_toml)
    message(STATUS "ForgeRelay: fetching toml++ v3.4.0 (FetchContent)")
    FetchContent_Declare(
        tomlplusplus
        URL        https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.zip
    )
    FetchContent_MakeAvailable(tomlplusplus)
    set(FR_TOMLPP_SOURCE_DIR "${tomlplusplus_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
