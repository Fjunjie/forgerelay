/*
 * fr_crc32.c - IEEE CRC32 按位实现。
 *
 * 每帧仅校验 20 字节固定头中的前 16 字节，性能不敏感；
 * 按位实现避免静态查找表与初始化线程安全问题（D-03）。
 */
#include "forgerelay/fr_crc32.h"

uint32_t fr_crc32(const void *data, size_t len) {
    if (data == NULL || len == 0) {
        return 0u;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xedb88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xffffffffu;
}
