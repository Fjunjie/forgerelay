# frtransport：M3 传输层（Poller 双后端、I/O 线程、工作池、Server 门面）。

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
endif()

fr_apply_warnings(frtransport cxx)
fr_apply_sanitizers(frtransport)
