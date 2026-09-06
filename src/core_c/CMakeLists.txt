# libfrcore：C17 核心库（缓冲区、字节序、帧编解码、路径、fd 封装）。
# ARCH-01：纯 C 接口，不依赖 C++ 与业务层。

add_library(frcore STATIC
    fr_error.c
    fr_byteorder.c
    fr_checked.c
    fr_buffer.c
    fr_crc32.c
    fr_hex.c
    fr_frame.c
    fr_path.c
    fr_fd.c
    fr_version.c
)
add_library(forgerelay::core ALIAS frcore)

target_include_directories(frcore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>)

target_compile_features(frcore PUBLIC c_std_17)

if(UNIX)
    # 64 位文件偏移，保证大制品 stat/lseek 正确。
    target_compile_definitions(frcore PRIVATE _FILE_OFFSET_BITS=64)
endif()

fr_apply_warnings(frcore c)
fr_apply_sanitizers(frcore)
