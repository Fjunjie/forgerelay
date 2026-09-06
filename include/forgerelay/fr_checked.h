/*
 * fr_checked.h - 溢出检查算术（需求 PROTO-03、§12.1）。
 *
 * 全部函数遵循统一约定：
 *  - 成功返回 true 并写出 *out；
 *  - 失败返回 false 且不修改 *out（强保证）；
 *  - out 为 NULL 且计算成功时视为丢弃结果（仍返回 true）；失败路径不写入。
 *
 * size_t 版本统一经 64 位计算后收窄，兼容 32 位平台。
 * 线程安全：纯函数，线程安全。
 */
#ifndef FR_CHECKED_H
#define FR_CHECKED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 计算 a + b，检测无符号 32 位溢出。
 *
 * @param[in] a 加数。
 * @param[in] b 加数。
 * @param[out] out 结果；失败时不修改。
 * @return true 成功；false 溢出。
 */
bool fr_checked_add_u32(uint32_t a, uint32_t b, uint32_t *out);

/**
 * 计算 a + b，检测无符号 64 位溢出。
 *
 * @param[in] a 加数。
 * @param[in] b 加数。
 * @param[out] out 结果；失败时不修改。
 * @return true 成功；false 溢出。
 */
bool fr_checked_add_u64(uint64_t a, uint64_t b, uint64_t *out);

/**
 * 计算 a - b；要求 a >= b，否则视为下溢。
 *
 * @param[in] a 被减数。
 * @param[in] b 减数。
 * @param[out] out 结果；失败时不修改。
 * @return true 成功；false 下溢。
 */
bool fr_checked_sub_u64(uint64_t a, uint64_t b, uint64_t *out);

/**
 * 计算 a * b，检测无符号 64 位溢出。
 *
 * @param[in] a 乘数。
 * @param[in] b 乘数。
 * @param[out] out 结果；失败时不修改。
 * @return true 成功；false 溢出。
 */
bool fr_checked_mul_u64(uint64_t a, uint64_t b, uint64_t *out);

/**
 * 计算 a + b（size_t 域）。
 *
 * @param[in] a 加数。
 * @param[in] b 加数。
 * @param[out] out 结果；失败时不修改。
 * @return true 成功；false 溢出。
 */
bool fr_checked_add_size(size_t a, size_t b, size_t *out);

/**
 * 计算 a - b（size_t 域）；要求 a >= b。
 *
 * @param[in] a 被减数。
 * @param[in] b 减数。
 * @param[out] out 结果；失败时不修改。
 * @return true 成功；false 下溢。
 */
bool fr_checked_sub_size(size_t a, size_t b, size_t *out);

/**
 * 计算 a * b（size_t 域）。
 *
 * @param[in] a 乘数。
 * @param[in] b 乘数。
 * @param[out] out 结果；失败时不修改。
 * @return true 成功；false 溢出。
 */
bool fr_checked_mul_size(size_t a, size_t b, size_t *out);

/**
 * 将 uint64_t 安全收窄为 size_t。
 *
 * @param[in] v 待收窄值。
 * @param[out] out 结果；失败时不修改。
 * @return true 成功；false 值超出 size_t 表示范围。
 */
bool fr_size_from_u64(uint64_t v, size_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FR_CHECKED_H */
