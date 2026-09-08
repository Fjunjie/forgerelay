# frstorage：C++20 存储层（SQLite 元数据 + 内容寻址块存储 + 会话/GC/恢复）。
# 依赖：libfrcore（C17 核心库）、SQLite、SHA-256 后端（Linux OpenSSL / Windows BCrypt）。

include(GetSQLite3)
fr_get_sqlite3()

set(FR_STORAGE_SOURCES
    fr_db.cpp
    fr_schema.cpp
    fr_digest_common.cpp
    fr_chunk_store.cpp
    fr_storage.cpp
)

if(WIN32)
    list(APPEND FR_STORAGE_SOURCES fr_digest_bcrypt.cpp)
else()
    list(APPEND FR_STORAGE_SOURCES fr_digest_ossl.cpp)
    find_package(OpenSSL REQUIRED)
endif()

add_library(frstorage STATIC ${FR_STORAGE_SOURCES})
add_library(forgerelay::storage ALIAS frstorage)

target_include_directories(frstorage
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>)

target_compile_features(frstorage PUBLIC cxx_std_20)
target_link_libraries(frstorage PUBLIC frcore ${FR_SQLITE_TARGET})

if(WIN32)
    # CNG（BCrypt）：SHA-256 后端（D-13）。
    target_link_libraries(frstorage PUBLIC bcrypt)
else()
    target_link_libraries(frstorage PUBLIC OpenSSL::Crypto)
endif()

fr_apply_warnings(frstorage cxx)
fr_apply_sanitizers(frstorage)
