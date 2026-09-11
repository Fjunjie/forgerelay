// fr_config.cpp - 服务端配置解析与校验实现（CFG-01/CFG-02）。
#include "forgerelay/config.hpp"

#include <toml++/toml.hpp>

#include "forgerelay/log.hpp"
#include "forgerelay/storage/error.hpp"

namespace fr {

bool parse_listen_address(const std::string &text, std::string &host_out, uint16_t &port_out)
{
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }
    host_out = text.substr(0, colon);
    const std::string port = text.substr(colon + 1);
    if (port.empty() || port.size() > 5) {
        return false;
    }
    uint32_t value = 0;
    for (char c : port) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(c - '0');
        if (value > 65535u) {
            return false;
        }
    }
    port_out = static_cast<uint16_t>(value);
    return true;
}

void validate_server_settings(ServerSettings &settings)
{
    std::string host;
    uint16_t port = 0;
    if (!parse_listen_address(settings.listen, host, port)) {
        throw_error(FR_E_ARG, "config: [server].listen must be \"host:port\", got: " +
                                  settings.listen);
    }
    if (settings.max_connections < 16 || settings.max_connections > 1024) {
        throw_error(FR_E_ARG, "config: [server].max_connections must be within [16, 1024]"); // §7.3
    }
    if (settings.worker_threads < 1 || settings.worker_threads > 32) {
        throw_error(FR_E_ARG, "config: [server].worker_threads must be within [1, 32]"); // §7.3
    }
    if (settings.request_timeout_seconds <= 0) {
        throw_error(FR_E_ARG, "config: request_timeout_seconds must be positive");
    }
    if (settings.tls.enabled) {
        if (settings.tls.certificate.empty() || settings.tls.private_key.empty()) {
            throw_error(FR_E_ARG,
                        "config: [tls].certificate/private_key are required when enabled");
        }
    }
    LogLevel level = LogLevel::Info;
    if (!log_level_from(settings.log.level, level)) {
        throw_error(FR_E_ARG,
                    "config: [logging].level must be one of trace/debug/info/warn/error");
    }
    if (settings.log.format != "json") {
        throw_error(FR_E_ARG, "config: [logging].format: only \"json\" is supported"); // LOG-02
    }
    if (settings.storage.root.empty()) {
        throw_error(FR_E_ARG, "config: [storage].root is required");
    }
    if (settings.storage.capacity_bytes == 0) {
        throw_error(FR_E_ARG, "config: [storage].capacity_bytes must be positive");
    }
    if (settings.storage.high_watermark_percent < 1 ||
        settings.storage.high_watermark_percent > 100) {
        throw_error(FR_E_ARG,
                    "config: [storage].high_watermark_percent must be within [1, 100]");
    }
    if (settings.storage.session_hours < 1 || settings.storage.session_hours > 168) {
        throw_error(FR_E_ARG, "config: [storage].upload_session_hours must be within [1, 168]");
    }
    if (settings.storage.chunk_size < 1024ull * 1024 ||
        settings.storage.chunk_size > 8ull * 1024 * 1024) {
        throw_error(FR_E_ARG,
                    "config: [storage].chunk_size must be within [1 MiB, 8 MiB]"); // FR-UP-02
    }
    if (settings.storage.gc_grace_seconds < 0) {
        throw_error(FR_E_ARG, "config: [storage].gc_grace_seconds must be >= 0");
    }
    if (settings.storage.max_sessions_per_owner < 1 ||
        settings.storage.max_sessions_per_owner > 64) {
        throw_error(FR_E_ARG,
                    "config: [storage].max_sessions_per_owner must be within [1, 64]"); // §7.3
    }
    if (settings.log.max_file_bytes == 0 || settings.log.keep_files < 1) {
        throw_error(FR_E_ARG, "config: [logging] rotation settings are invalid"); // LOG-04
    }
}

ServerSettings load_server_config(const std::string &path)
{
    if (path.empty()) {
        throw_error(FR_E_ARG, "config: config path is empty");
    }
    toml::table table;
    try {
        table = toml::parse_file(path);
    } catch (const toml::parse_error &err) {
        throw_error(FR_E_ARG, std::string("config: TOML parse error: ") + err.what());
    }

    ServerSettings settings;

    if (auto server = table["server"].as_table()) {
        settings.listen = server->get("listen")->value_or(settings.listen);
        settings.max_connections = static_cast<int>(server->get("max_connections")->value_or(
            static_cast<int64_t>(settings.max_connections)));
        settings.worker_threads = static_cast<int>(
            server->get("worker_threads")->value_or(static_cast<int64_t>(settings.worker_threads)));
    }
    if (auto tls = table["tls"].as_table()) {
        settings.tls.enabled = tls->get("enabled")->value_or(false);
        settings.tls.certificate = tls->get("certificate")->value_or(std::string());
        settings.tls.private_key = tls->get("private_key")->value_or(std::string());
    }
    if (auto storage = table["storage"].as_table()) {
        settings.storage.root = storage->get("root")->value_or(std::string());
        const int64_t capacity =
            storage->get("capacity_bytes")->value_or(static_cast<int64_t>(
                settings.storage.capacity_bytes > INT64_MAX
                    ? INT64_MAX
                    : static_cast<int64_t>(settings.storage.capacity_bytes)));
        settings.storage.capacity_bytes = static_cast<uint64_t>(capacity);
        settings.storage.high_watermark_percent = static_cast<int>(storage->get(
                                                                        "high_watermark_percent")
                                                                        ->value_or(static_cast<int64_t>(
                                                                            settings.storage.high_watermark_percent)));
        settings.storage.session_hours =
            static_cast<int>(storage->get("upload_session_hours")->value_or(static_cast<int64_t>(
                settings.storage.session_hours)));
        settings.storage.chunk_size =
            static_cast<uint64_t>(storage->get("chunk_size")->value_or(
                static_cast<int64_t>(settings.storage.chunk_size)));
        settings.storage.gc_grace_seconds =
            static_cast<int>(storage->get("gc_grace_seconds")->value_or(static_cast<int64_t>(
                settings.storage.gc_grace_seconds)));
        settings.storage.max_sessions_per_owner =
            static_cast<int>(storage->get("max_sessions_per_owner")->value_or(static_cast<int64_t>(
                settings.storage.max_sessions_per_owner)));
    }
    if (auto database = table["database"].as_table()) {
        settings.database_path = database->get("path")->value_or(std::string());
    }
    if (auto logging = table["logging"].as_table()) {
        settings.log.level = logging->get("level")->value_or(settings.log.level);
        settings.log.format = logging->get("format")->value_or(settings.log.format);
        settings.log.file = logging->get("file")->value_or(std::string());
        const int64_t max_bytes = logging->get("max_file_bytes")->value_or(
            static_cast<int64_t>(settings.log.max_file_bytes > INT64_MAX
                                     ? INT64_MAX
                                     : static_cast<int64_t>(settings.log.max_file_bytes)));
        settings.log.max_file_bytes = static_cast<uint64_t>(max_bytes);
        settings.log.keep_files = static_cast<int>(
            logging->get("keep_files")->value_or(static_cast<int64_t>(settings.log.keep_files)));
    }

    validate_server_settings(settings);
    return settings;
}

void apply_cli_overrides(ServerSettings &settings, const std::string &listen,
                         const std::string &log_level)
{
    if (!listen.empty()) {
        settings.listen = listen;
    }
    if (!log_level.empty()) {
        settings.log.level = log_level;
    }
    validate_server_settings(settings); // 覆盖后重新校验（CFG-01）
}

} // namespace fr
