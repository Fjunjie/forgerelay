/*
 * fr_error.h - ForgeRelay 稳定错误码与结构化错误（需求 ERR-01/ERR-02）。
 *
 * 约定（DECISIONS D-01）：
 *  - 库代码不调用 exit/abort，全部以 fr_status 返回；
 *  - 错误码数值与名称稳定，按 PROTOCOL.md §4.1 记录；
 *  - 详情文本不含敏感信息（令牌、密钥、内部路径、SQL）。
 *
 * 线程安全：本模块所有函数仅操作传入内存或只读静态数据，线程安全。
 */
#ifndef FR_ERROR_H
#define FR_ERROR_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 稳定错误码。0 表示成功，负值表示失败。
 * 数值一经发布不得变更或复用；新增错误码在文件末尾追加。
 */
typedef enum fr_status {
    FR_OK = 0,           /**< 成功。 */
    FR_E_ARG = -1,       /**< 无效参数（调用方编程错误）。 */
    FR_E_RANGE = -2,     /**< 取值超出允许范围。 */
    FR_E_OVERFLOW = -3,  /**< 算术或容量溢出。 */
    FR_E_NOMEM = -4,     /**< 内存分配失败。 */
    FR_E_IO = -5,        /**< 文件或系统 I/O 失败。 */
    FR_E_EOF = -6,       /**< 输入提前结束（读满语义不足）。 */
    FR_E_PROTOCOL = -7,  /**< 帧格式或字段违规（PROTO-04）。 */
    FR_E_TRUNCATED = -8, /**< 输入流关闭导致帧不完整。 */
    FR_E_STATE = -9,     /**< 状态机非法转换。 */
    FR_E_NOTFOUND = -10, /**< 对象不存在。 */
    FR_E_EXISTS = -11,   /**< 对象已存在。 */
    FR_E_LIMIT = -12,    /**< 超出配额或上限。 */
    FR_E_BUSY = -13,     /**< 资源忙碌（背压，M3+）。 */
    FR_E_INTERNAL = -14, /**< 内部不变量被破坏。 */
    FR_E_CONFLICT = -15, /**< 内容冲突（如同编号块内容不同）。 */
    FR_E_EXPIRED = -16   /**< 对象已过期（如上传会话）。 */
} fr_status;

/** 错误类别（ERR-02），用于客户端区分可重试与永久错误（ERR-04）。 */
typedef enum fr_category {
    FR_CAT_OK = 0,   /**< 成功，非错误。 */
    FR_CAT_ARGUMENT, /**< 参数错误：不可重试。 */
    FR_CAT_RANGE,    /**< 取值越界：不可重试。 */
    FR_CAT_RESOURCE, /**< 资源耗尽（内存/上限）：可重试。 */
    FR_CAT_IO,       /**< 系统 I/O：部分场景可重试。 */
    FR_CAT_PROTOCOL, /**< 协议违规：不可重试。 */
    FR_CAT_STATE,    /**< 状态错误：不可重试。 */
    FR_CAT_INTERNAL  /**< 内部错误：不可重试。 */
} fr_category;

/**
 * 返回错误码所属类别。
 *
 * @param[in] code 错误码。
 * @return 类别；对未知错误码返回 FR_CAT_INTERNAL。
 * @thread_safety 线程安全。
 */
fr_category fr_status_category(fr_status code);

/**
 * 返回错误码的稳定短名称（如 "FR_E_IO"），用于日志与测试断言。
 *
 * @param[in] code 错误码。
 * @return 静态存储期字符串；未知错误码返回 "FR_E_UNKNOWN"。
 * @thread_safety 线程安全。
 */
const char *fr_status_name(fr_status code);

/**
 * 返回错误码的人类可读说明（英文，静态文本，不含动态内容）。
 *
 * @param[in] code 错误码。
 * @return 静态存储期字符串；未知错误码返回通用说明。
 * @thread_safety 线程安全。
 */
const char *fr_status_message(fr_status code);

/** fr_error.detail 的最大长度（含 NUL）。 */
#define FR_ERROR_DETAIL_MAX 192

/**
 * 结构化错误详情。作为可选出参传入各 API；传 NULL 表示不关心详情。
 *
 * 所有权：调用方拥有并负责生命周期；库只写入，不保存指针。
 */
typedef struct fr_error {
    fr_status code;                   /**< 与返回值一致的主要错误码。 */
    char detail[FR_ERROR_DETAIL_MAX]; /**< NUL 结尾的详情文本，可能为空串。 */
} fr_error;

/**
 * 填充错误详情（printf 风格，超长截断）。
 *
 * @param[out] err 错误对象；可为 NULL。成功时写入 code 与格式化详情。
 * @param[in] code 主要错误码。
 * @param[in] fmt printf 格式串；不得包含敏感信息（SEC-07/PROTO-06）。
 * @thread_safety 线程安全（仅操作 err 指向的内存）。
 */
void fr_error_set(fr_error *err, fr_status code, const char *fmt, ...)
#if defined(__GNUC__)
    /* gnu_printf：按 C99 转换语义校验（含 %zu）；UCRT/glibc 运行时均支持。 */
    __attribute__((format(gnu_printf, 3, 4)))
#endif
    ;

/**
 * 填充带 errno 上下文的错误详情，形如 "<what>: <strerror>"。
 *
 * @param[out] err 错误对象；可为 NULL。
 * @param[in] code 主要错误码。
 * @param[in] errno_value 系统调用失败时的 errno。
 * @param[in] what 失败操作的稳定短语（如 "open"）。
 * @thread_safety 线程安全（依赖 strerror 的库级线程安全实现）。
 */
void fr_error_set_errno(fr_error *err, fr_status code, int errno_value, const char *what);

/**
 * 将错误对象复位为“无错误”（code=FR_OK，detail 清空）。
 *
 * @param[out] err 错误对象；可为 NULL（无操作）。
 * @thread_safety 线程安全（仅操作 err 指向的内存）。
 */
void fr_error_clear(fr_error *err);

#ifdef __cplusplus
}
#endif

#endif /* FR_ERROR_H */
