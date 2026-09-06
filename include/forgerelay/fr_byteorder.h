/*
 * fr_byteorder.h - 大端（网络字节序）整型读写（需求 §12.1：显式处理字节序）。
 *
 * 实现按字节移位，不假设主机字节序，不要求对齐。
 * 线程安全：所有函数仅操作调用方缓冲区，线程安全。
 */
#ifndef FR_BYTEORDER_H
#define FR_BYTEORDER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 从缓冲区读取 2 字节大端无符号整数。
 *
 * @param[in] p 输入缓冲区；调用方保证至少 2 字节可读。
 * @return 解析值。
 */
uint16_t fr_load_be16(const uint8_t *p);

/**
 * 从缓冲区读取 4 字节大端无符号整数。
 *
 * @param[in] p 输入缓冲区；调用方保证至少 4 字节可读。
 * @return 解析值。
 */
uint32_t fr_load_be32(const uint8_t *p);

/**
 * 从缓冲区读取 8 字节大端无符号整数。
 *
 * @param[in] p 输入缓冲区；调用方保证至少 8 字节可读。
 * @return 解析值。
 */
uint64_t fr_load_be64(const uint8_t *p);

/**
 * 向缓冲区写入 2 字节大端无符号整数。
 *
 * @param[out] p 输出缓冲区；调用方保证至少 2 字节可写。
 * @param[in] v 待写入值。
 */
void fr_store_be16(uint8_t *p, uint16_t v);

/**
 * 向缓冲区写入 4 字节大端无符号整数。
 *
 * @param[out] p 输出缓冲区；调用方保证至少 4 字节可写。
 * @param[in] v 待写入值。
 */
void fr_store_be32(uint8_t *p, uint32_t v);

/**
 * 向缓冲区写入 8 字节大端无符号整数。
 *
 * @param[out] p 输出缓冲区；调用方保证至少 8 字节可写。
 * @param[in] v 待写入值。
 */
void fr_store_be64(uint8_t *p, uint64_t v);

#ifdef __cplusplus
}
#endif

#endif /* FR_BYTEORDER_H */
