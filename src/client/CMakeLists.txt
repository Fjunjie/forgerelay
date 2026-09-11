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
endif()

fr_apply_warnings(frclient cxx)
fr_apply_sanitizers(frclient)
