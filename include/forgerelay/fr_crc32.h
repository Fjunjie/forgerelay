/*
 * fr_crc32.h - IEEE CRC32 校验（帧头校验字段，需求 §5.2；DECISIONS D-03）。
 *
 * 多项式 0xEDB88320（反射表示），初始值 0xFFFFFFFF，结果取反——与 zlib/PNG 兼容。
 * 线程安全：纯函数，线程安全。
 */
#ifndef FR_CRC32_H
#define FR_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 计算数据的 IEEE CRC32。
 *
 * @param[in] data 输入数据；len == 0 时可为 NULL。
 * @param[in] len 数据长度。
 * @return CRC32 值；len == 0 时返回 0。
 */
uint32_t fr_crc32(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FR_CRC32_H */
