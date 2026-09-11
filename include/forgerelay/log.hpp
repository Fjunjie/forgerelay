// fr/log.hpp - 结构化日志（LOG-01..04）。
//
// - 五级：trace/debug/info/warn/error（LOG-01）；
// - JSON 行格式，每条含时间戳/级别/消息，可选 request_id 与键值对（LOG-02）；
// - 按大小轮转：默认单文件 100 MiB、保留 5 个历史文件（LOG-04）；
// - 高频块传输日志由调用方使用 debug 级（LOG-03）。
//
// 线程安全：所有方法可被多线程调用（内部互斥）。
#ifndef FR_SERVICE_LOG_HPP
#define FR_SERVICE_LOG_HPP

#include <cstdint>
#include <filesystem>
#include <string>

namespace fr {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
};

/** 文本转日志级别；非法值返回 false（CFG-01 校验用）。 */
bool log_level_from(const std::string &name, LogLevel &out);

/** 日志级别名（小写）。 */
const char *log_level_name(LogLevel level);

/** JSON 行文件日志。未 open 时输出到 stderr。 */
class Logger {
public:
    /** 轮转参数（LOG-04）。 */
    struct RotatePolicy {
        uint64_t max_file_bytes = 100ull * 1024 * 1024;
        int keep_files = 5;
    };

    Logger();
    ~Logger();
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    /**
     * 打开日志文件（默认轮转策略）。
     *
     * @param[in] path 日志文件路径；为空则输出 stderr。
     * @param[in] level 最低输出级别。
     * @throws fr::Error 打开/创建失败。
     */
    void open(const std::filesystem::path &path, LogLevel level);

    /** 打开日志文件（自定义轮转策略，LOG-04）。 */
    void open(const std::filesystem::path &path, LogLevel level, const RotatePolicy &policy);

    /** 关闭日志文件。 */
    void close() noexcept;

    /** 是否启用该级别。 */
    bool enabled(LogLevel level) const;

    /**
     * 写一条日志。
     *
     * @param[in] level 级别。
     * @param[in] message 消息（不得含凭据，SEC-07）。
     * @param[in] request_id 关联请求 ID；空串则不输出该字段（LOG-02）。
     * @param[in] keys 附加键；与 values 成对（长度相同，至多 8 对）。
     */
    void log(LogLevel level, const std::string &message, const std::string &request_id,
             const char *const *keys = nullptr, const std::string *values = nullptr,
             size_t kv_count = 0);

    /** 便捷封装。 */
    void info(const std::string &msg, const std::string &req_id = std::string());
    void error(const std::string &msg, const std::string &req_id = std::string());
    void warn(const std::string &msg, const std::string &req_id = std::string());
    void debug(const std::string &msg, const std::string &req_id = std::string());

private:
    void rotate_if_needed_locked(uint64_t incoming_bytes);

    std::filesystem::path path_;
    void *file_ = nullptr; // std::FILE*
    LogLevel level_ = LogLevel::Info;
    RotatePolicy policy_;
    uint64_t current_bytes_ = 0;
    void *mutex_ = nullptr; // std::mutex*
};

} // namespace fr

#endif /* FR_SERVICE_LOG_HPP */
