// fr/config.hpp - 服务端配置解析与校验（CFG-01/CFG-02、§9.1）。
//
// TOML 结构见 config/forgerelay.example.toml；所有字段在加载时完整校验
// （CFG-01：字段无效输出明确错误并拒绝启动——抛 fr::Error）。
// TOML 解析使用 toml++（D-21）；字段范围约束对应需求 §7.3/FR-UP/FR-GC。
#ifndef FR_SERVICE_CONFIG_HPP
#define FR_SERVICE_CONFIG_HPP

#include <cstdint>
#include <filesystem>
#include <string>

#include "forgerelay/storage/storage.hpp"

namespace fr {

/** TLS 配置（§5.1、SEC-01..03）。 */
struct TlsSettings {
    bool enabled = false;
    std::string certificate; // PEM 证书路径
    std::string private_key; // PEM 私钥路径
};

/** 日志配置（LOG-01..04）。 */
struct LogSettings {
    std::string level = "info";                     // trace/debug/info/warn/error
    std::string format = "json";                    // 首版仅支持 json（LOG-02）
    std::string file;                               // 空则输出 stderr
    uint64_t max_file_bytes = 100ull * 1024 * 1024; // LOG-04
    int keep_files = 5;
};

/** 服务端配置（§9.1 + §7.3 资源上限 + 存储参数）。 */
struct ServerSettings {
    std::string listen = "127.0.0.1:7443";      // host:port
    int max_connections = 128;                  // §7.3：16–1024
    int worker_threads = 4;                     // §7.3：1–32
    uint64_t max_payload = 8ull * 1024 * 1024;  // 固定上限（§7.3）
    int request_timeout_seconds = 120;          // 请求超时（连接空闲上限）
    int64_t maintenance_interval_seconds = 300; // 后台维护周期（§7.1）

    TlsSettings tls;
    LogSettings log;

    StorageConfig storage;     // root/capacity/high_watermark/session_hours/chunk_size/...
    std::string database_path; // 空 → storage.root/metadata.db
};

/**
 * 校验配置完整性（CFG-01）：字段无效时抛 fr::Error 并附明确原因。
 * 包括：端口/范围、路径非空、TLS 开启时证书与私钥必填、日志级别合法等。
 */
void validate_server_settings(ServerSettings &settings);

/**
 * 从 TOML 文件加载配置（CFG-01）。文件不存在/解析失败/字段非法均抛 fr::Error。
 *
 * @param[in] path 配置文件路径。
 * @return 已通过校验的配置。
 */
ServerSettings load_server_config(const std::string &path);

/**
 * 应用命令行覆盖（CFG-02）：仅覆盖非空参数。
 *
 * @param[in,out] settings 目标配置。
 * @param[in] listen 监听地址；空则忽略。
 * @param[in] log_level 日志级别；空则忽略。
 */
void apply_cli_overrides(ServerSettings &settings, const std::string &listen,
                         const std::string &log_level);

/** 解析 "host:port"；返回 false 表示格式非法（CFG-01）。 */
bool parse_listen_address(const std::string &text, std::string &host_out, uint16_t &port_out);

} // namespace fr

#endif /* FR_SERVICE_CONFIG_HPP */
