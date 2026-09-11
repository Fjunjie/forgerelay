# frtransport：M3 传输层（Poller 双后端、I/O 线程、工作池、Server 门面、TLS）。

add_library(frtransport STATIC
    fr_server.cpp
)
add_library(forgerelay::transport ALIAS frtransport)

target_include_directories(frtransport
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>)

target_compile_features(frtransport PUBLIC cxx_std_20)
target_link_libraries(frtransport PUBLIC frservice)

if(WIN32)
    target_link_libraries(frtransport PUBLIC ws2_32)
else()
    # TLS 后端（D-20）：找到 OpenSSL 3.x 则启用（定义 FR_HAVE_OPENSSL）；
    # 未找到时构建仍可用，但 tls.enabled=true 的配置会被启动校验拒绝。
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_sources(frtransport PRIVATE fr_tls.cpp)
        target_compile_definitions(frtransport PUBLIC FR_HAVE_OPENSSL)
        target_link_libraries(frtransport PUBLIC OpenSSL::SSL)
        message(STATUS "ForgeRelay: TLS backend enabled (OpenSSL ${OpenSSL_VERSION})")
    else()
        message(STATUS "ForgeRelay: OpenSSL not found - TLS backend DISABLED")
    endif()
endif()

fr_apply_warnings(frtransport cxx)
fr_apply_sanitizers(frtransport)
