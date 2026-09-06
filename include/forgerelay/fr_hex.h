/*
 * fr_hex.h - hex 编解码与摘要校验。
 *
 * 线程安全：纯函数，线程安全。
 */
#ifndef FR_HEX_H
#define FR_HEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "forgerelay/fr_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SHA-256 摘要的十六进制长度（小写）。 */
#define FR_SHA256_HEX_LEN 64

/**
 * 将原始字节编码为小写 hex 字符串（NUL 结尾）。
 *
 * @param[in] raw 原始字节；raw_len == 0 时可为 NULL。
 * @param[in] raw_len 原始字节数。
 * @param[out] out 输出缓冲。
 * @param[in] out_cap 输出缓冲容量；需 >= raw_len * 2 + 1。
 * @retval FR_OK 成功。
 * @retval FR_E_ARG raw 为 NULL 且 raw_len > 0，或 out 为 NULL。
 * @retval FR_E_RANGE out_cap 不足。
 */
fr_status fr_hex_encode(const uint8_t *raw, size_t raw_len, char *out, size_t out_cap);

/**
 * 将 hex 字符串解码为原始字节；接受大小写混合输入。
 *
 * @param[in] hex 输入字符串；长度必须为偶数且只含 hex 字符。
 * @param[out] out 输出缓冲。
 * @param[in] out_cap 输出缓冲容量；需 >= strlen(hex) / 2。
 * @param[out] out_len 实际写入字节数；可为 NULL。
 * @retval FR_OK 成功。
 * @retval FR_E_ARG 参数为 NULL 或 hex 长度为奇数。
 * @retval FR_E_RANGE out_cap 不足。
 */
fr_status fr_hex_decode(const char *hex, uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * 校验字符串是否为 64 位小写 hex 摘要（SHA-256 表示，FR-STO-01/02）。
 *
 * @param[in] s 待校验字符串；NULL 返回 false。
 * @return true 合法。
 */
bool fr_digest_hex_valid(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* FR_HEX_H */
