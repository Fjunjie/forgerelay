# frclient：阻塞式客户端库（frctl 与 E2E 测试共用）。

add_library(frclient STATIC
    fr_client.cpp
)
add_library(forgerelay::client ALIAS frclient)

target_include_directories(frclient
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>)

target_compile_features(frclient PUBLIC cxx_std_20)
target_link_libraries(frclient PUBLIC frservice)

if(WIN32)
    target_link_libraries(frclient PUBLIC ws2_32)
else()
    # TLS 后端（D-20）：与 frtransport 保持一致的门控。
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_compile_definitions(frclient PUBLIC FR_HAVE_OPENSSL)
        target_link_libraries(frclient PUBLIC OpenSSL::SSL)
    endif()
endif()

fr_apply_warnings(frclient cxx)
fr_apply_sanitizers(frclient)
