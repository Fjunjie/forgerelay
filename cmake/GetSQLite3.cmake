# SQLite 获取（DECISIONS D-14）：优先系统包 find_package(SQLite3)
# （CMake 内置 FindSQLite3 模块，对应目标 SQLite::SQLite3）；失败则 FetchContent
# 官方 amalgamation 压缩包自行编译静态库。仓库不提交第三方源码副本。
#
# 结果：设置变量 FR_SQLITE_TARGET（在调用者作用域）。

include(FetchContent)

function(fr_get_sqlite3)
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        message(STATUS "ForgeRelay: using system SQLite3 ${SQLite3_VERSION}")
        set(FR_SQLITE_TARGET SQLite::SQLite3 PARENT_SCOPE)
        return()
    endif()

    message(STATUS "ForgeRelay: fetching SQLite amalgamation 3.46.0 (FetchContent)")
    FetchContent_Declare(
        sqlite_amalgamation
        URL        https://sqlite.org/2024/sqlite-amalgamation-3460000.zip
    )
    FetchContent_MakeAvailable(sqlite_amalgamation)

    add_library(fr_sqlite3 STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
    target_include_directories(fr_sqlite3 PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
    # amalgamation 要求 pthread（线程安全内核模式）；关闭我们不用的扩展以缩小体积。
    if(UNIX)
        target_compile_definitions(fr_sqlite3 PRIVATE
            SQLITE_THREADSAFE=1 SQLITE_ENABLE_COLUMN_METADATA=0)
        find_package(Threads REQUIRED)
        target_link_libraries(fr_sqlite3 PUBLIC Threads::Threads)
        target_compile_options(fr_sqlite3 PRIVATE -w)
    else()
        target_compile_definitions(fr_sqlite3 PRIVATE SQLITE_THREADSAFE=0)
    endif()
    set(FR_SQLITE_TARGET fr_sqlite3 PARENT_SCOPE)
endfunction()
