// fr_log.cpp - JSON 行日志实现（LOG-01..04）。
#include "forgerelay/log.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

#include "forgerelay/storage/error.hpp"

namespace fr {

namespace {

int64_t now_seconds()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string json_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        const unsigned char b = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (b < 0x20) {
                char buf[8];
                (void)snprintf(buf, sizeof(buf), "\\u%04x", b);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

} // namespace

bool log_level_from(const std::string &name, LogLevel &out)
{
    if (name == "trace") {
        out = LogLevel::Trace;
        return true;
    }
    if (name == "debug") {
        out = LogLevel::Debug;
        return true;
    }
    if (name == "info") {
        out = LogLevel::Info;
        return true;
    }
    if (name == "warn") {
        out = LogLevel::Warn;
        return true;
    }
    if (name == "error") {
        out = LogLevel::Error;
        return true;
    }
    return false;
}

const char *log_level_name(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    default:
        return "info";
    }
}

Logger::Logger()
    : mutex_(new std::mutex())
{
}

Logger::~Logger()
{
    close();
    delete static_cast<std::mutex *>(mutex_);
    mutex_ = nullptr;
}

void Logger::open(const std::filesystem::path &path, LogLevel level)
{
    open(path, level, RotatePolicy{});
}

void Logger::open(const std::filesystem::path &path, LogLevel level, const RotatePolicy &policy)
{
    close();
    level_ = level;
    policy_ = policy;
    if (path.empty()) {
        path_.clear();
        current_bytes_ = 0;
        return;
    }
    std::filesystem::create_directories(path.parent_path());
    FILE *f = nullptr;
#if defined(_WIN32)
    fopen_s(&f, path.string().c_str(), "ab");
#else
    f = fopen(path.string().c_str(), "ab");
#endif
    if (f == nullptr) {
        throw_error(FR_E_IO, "open log file failed: " + path.string());
    }
    path_ = path;
    file_ = f;
    std::error_code ec;
    current_bytes_ = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
}

void Logger::close() noexcept
{
    if (file_ != nullptr) {
        (void)fclose(static_cast<FILE *>(file_));
        file_ = nullptr;
    }
    current_bytes_ = 0;
}

bool Logger::enabled(LogLevel level) const
{
    return static_cast<int>(level) >= static_cast<int>(level_);
}

void Logger::rotate_if_needed_locked(uint64_t incoming_bytes)
{
    if (file_ == nullptr || current_bytes_ + incoming_bytes < policy_.max_file_bytes) {
        return;
    }
    (void)fclose(static_cast<FILE *>(file_));
    file_ = nullptr;
    /* log.4 -> log.5, log.3 -> log.4, ... log -> log.1（保留 keep_files 份）。 */
    for (int i = policy_.keep_files - 1; i >= 1; i--) {
        std::filesystem::path from = path_;
        from += "." + std::to_string(i);
        std::filesystem::path to = path_;
        to += "." + std::to_string(i + 1);
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
    }
    std::error_code ec;
    std::filesystem::rename(path_, path_.string() + ".1", ec);
    FILE *f = nullptr;
#if defined(_WIN32)
    fopen_s(&f, path_.string().c_str(), "wb");
#else
    f = fopen(path_.string().c_str(), "wb");
#endif
    if (f == nullptr) {
        throw_error(FR_E_IO, "reopen log file after rotation failed");
    }
    file_ = f;
    current_bytes_ = 0;
}

void Logger::log(LogLevel level, const std::string &message, const std::string &request_id,
                 const char *const *keys, const std::string *values, size_t kv_count)
{
    if (!enabled(level)) {
        return;
    }
    /* JSON 行：{"ts":...,"level":"...","msg":"...",["req_id":"..."],kvs}（LOG-02）。 */
    std::string line = "{\"ts\":" + std::to_string(now_seconds());
    line += ",\"level\":\"";
    line += log_level_name(level);
    line += "\",\"msg\":\"";
    line += json_escape(message);
    line += "\"";
    if (!request_id.empty()) {
        line += ",\"req_id\":\"";
        line += json_escape(request_id);
        line += "\"";
    }
    if (keys != nullptr && values != nullptr) {
        const size_t n = kv_count > 8 ? 8 : kv_count;
        for (size_t i = 0; i < n; i++) {
            line += ",\"";
            line += json_escape(keys[i]);
            line += "\":\"";
            line += json_escape(values[i]);
            line += "\"";
        }
    }
    line += "}\n";

    std::lock_guard<std::mutex> guard(*static_cast<std::mutex *>(mutex_));
    if (file_ == nullptr) {
        (void)fputs(line.c_str(), stderr);
        (void)fflush(stderr);
        return;
    }
    rotate_if_needed_locked(line.size());
    (void)fputs(line.c_str(), static_cast<FILE *>(file_));
    (void)fflush(static_cast<FILE *>(file_));
    current_bytes_ += line.size();
}

void Logger::info(const std::string &msg, const std::string &req_id)
{
    log(LogLevel::Info, msg, req_id);
}

void Logger::error(const std::string &msg, const std::string &req_id)
{
    log(LogLevel::Error, msg, req_id);
}

void Logger::warn(const std::string &msg, const std::string &req_id)
{
    log(LogLevel::Warn, msg, req_id);
}

void Logger::debug(const std::string &msg, const std::string &req_id)
{
    log(LogLevel::Debug, msg, req_id);
}

} // namespace fr
