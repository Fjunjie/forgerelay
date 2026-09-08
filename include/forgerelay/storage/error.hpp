// fr/error.hpp - 存储层（C++）错误类型。
//
// 复用 C 核心库的稳定 fr_status 错误码（D-01）；C++ 层以异常传递错误，
// 在 M3/M4 的 C API/协议边界统一捕获并映射回 fr_status（D-16）。
// 库代码不调用 exit/abort（ERR-01）。
#ifndef FR_STORAGE_ERROR_HPP
#define FR_STORAGE_ERROR_HPP

#include <stdexcept>
#include <string>

#include "forgerelay/fr_error.h"

namespace fr {

/** 存储层异常：携带稳定错误码与不含敏感信息的说明（ERR-02）。 */
class Error : public std::exception {
public:
    Error(fr_status code, std::string message) : code_(code), message_(std::move(message)) {}

    /** 稳定错误码。 */
    fr_status code() const noexcept { return code_; }

    /** 错误说明。 */
    const char *what() const noexcept override { return message_.c_str(); }

private:
    fr_status code_;
    std::string message_;
};

/** 抛出 fr::Error 的便捷函数（保持 throw 点可检索）。 */
[[noreturn]] inline void throw_error(fr_status code, std::string message) {
    throw Error(code, std::move(message));
}

} // namespace fr

#endif /* FR_STORAGE_ERROR_HPP */
