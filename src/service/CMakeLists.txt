# frservice：M3/M4 服务层（日志、配置、消息编解码、鉴权、分发）。

include(GetToml)
fr_get_toml()

add_library(frservice STATIC
    fr_log.cpp
    fr_config.cpp
    fr_messages.cpp
    fr_auth.cpp
    fr_dispatch.cpp
)
add_library(forgerelay::service ALIAS frservice)

target_include_directories(frservice
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>)
target_include_directories(frservice
    SYSTEM PRIVATE
        "${FR_TOMLPP_SOURCE_DIR}/include")

target_compile_definitions(frservice PRIVATE
    TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=1 TOML_ENABLE_FORMATTERS=0)

target_compile_features(frservice PUBLIC cxx_std_20)
target_link_libraries(frservice PUBLIC frcore frstorage)

fr_apply_warnings(frservice cxx)
fr_apply_sanitizers(frservice)
