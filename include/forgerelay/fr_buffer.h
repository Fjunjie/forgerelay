/*
 * fr_buffer.h - 可增长字节缓冲与边界检查只读读取器。
 *
 * 所有权（ARCH-03）：
 *  - fr_buf 拥有 data（malloc 分配），由 fr_buf_destroy 释放；
 *  - fr_buf_reader 仅借用外部内存，从不释放；
 *  - fr_buf 可赋值移动（fr_buf_move），转移后源缓冲回到空状态。
 *
 * 线程安全：单个实例非线程安全（需外部同步）；无全局可变状态。
 * 失败语义：除 consume/reader 读取按需移动外，所有失败均不修改缓冲状态（强保证）。
 */
#ifndef FR_BUFFER_H
#define FR_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "forgerelay/fr_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 可增长字节缓冲。data 在 len == 0 且 cap == 0 时可为 NULL。 */
typedef struct fr_buf {
    uint8_t *data; /**< 拥有的数据区；所有权归缓冲本身。 */
    size_t len;    /**< 有效字节数。 */
    size_t cap;    /**< 已分配容量。 */
} fr_buf;

/**
 * 初始化为空缓冲（不分配内存）。
 *
 * @param[out] buf 目标缓冲；调用方负责随后调用 fr_buf_destroy()。
 */
void fr_buf_init(fr_buf *buf);

/**
 * 释放缓冲内存并复位为空状态；对空缓冲安全，可重复调用。
 *
 * @param[in,out] buf 目标缓冲；调用后 data 为 NULL，len/cap 为 0。
 */
void fr_buf_destroy(fr_buf *buf);

/**
 * 将 src 的内容移动到 dst，src 复位为空（所有权转移，O(1)）。
 *
 * @param[out] dst 目标缓冲；原内容被释放替换。
 * @param[in,out] src 源缓冲；调用后为空状态。
 */
void fr_buf_move(fr_buf *dst, fr_buf *src);

/**
 * 确保缓冲可再追加 additional 字节（必要时扩容）。
 *
 * @param[in,out] buf 目标缓冲。
 * @param[in] additional 需要的追加容量。
 * @retval FR_OK 成功。
 * @retval FR_E_OVERFLOW 容量计算溢出（PROTO-03）。
 * @retval FR_E_NOMEM 分配失败；缓冲保持原状态。
 */
fr_status fr_buf_reserve(fr_buf *buf, size_t additional);

/**
 * 追加 n 字节到缓冲尾部。
 *
 * @param[in,out] buf 目标缓冲。
 * @param[in] src 数据源；n == 0 时可为 NULL。
 * @param[in] n 追加字节数。
 * @retval FR_OK 成功。
 * @retval FR_E_ARG src 为 NULL 且 n > 0。
 * @retval FR_E_OVERFLOW / FR_E_NOMEM 同 fr_buf_reserve()。
 */
fr_status fr_buf_append(fr_buf *buf, const void *src, size_t n);

/**
 * 追加 1 字节。
 *
 * @param[in,out] buf 目标缓冲。
 * @param[in] v 字节值。
 * @return 同 fr_buf_append()。
 */
fr_status fr_buf_append_u8(fr_buf *buf, uint8_t v);

/**
 * 追加 2 字节大端整数。
 *
 * @param[in,out] buf 目标缓冲。
 * @param[in] v 值。
 * @return 同 fr_buf_append()。
 */
fr_status fr_buf_append_u16be(fr_buf *buf, uint16_t v);

/**
 * 追加 4 字节大端整数。
 *
 * @param[in,out] buf 目标缓冲。
 * @param[in] v 值。
 * @return 同 fr_buf_append()。
 */
fr_status fr_buf_append_u32be(fr_buf *buf, uint32_t v);

/**
 * 追加 8 字节大端整数。
 *
 * @param[in,out] buf 目标缓冲。
 * @param[in] v 值。
 * @return 同 fr_buf_append()。
 */
fr_status fr_buf_append_u64be(fr_buf *buf, uint64_t v);

/**
 * 清空内容（保留已分配容量）。
 *
 * @param[in,out] buf 目标缓冲。
 */
void fr_buf_clear(fr_buf *buf);

/**
 * 从头部移除 n 字节（其余前移）。
 *
 * @param[in,out] buf 目标缓冲。
 * @param[in] n 移除字节数。
 * @retval FR_OK 成功。
 * @retval FR_E_RANGE n 超过 len；缓冲保持原状态。
 */
fr_status fr_buf_consume(fr_buf *buf, size_t n);

/** 只读字节视图（借用，不拥有）。 */
typedef struct fr_buf_view {
    const uint8_t *data; /**< 借用的数据指针；len 为 0 时可为 NULL。 */
    size_t len;          /**< 视图长度。 */
} fr_buf_view;

/**
 * 返回缓冲的只读视图。
 *
 * @param[in] buf 源缓冲。
 * @return 视图；有效期与 buf 的数据区一致。
 */
fr_buf_view fr_buf_view_of(const fr_buf *buf);

/**
 * 边界检查的顺序读取器，借用一段只读内存。
 *
 * 典型用途：解析协议负载（PROTO-01/02）；任何越界读取返回 FR_E_RANGE 且不前移位置。
 */
typedef struct fr_buf_reader {
    const uint8_t *data; /**< 借用的数据区。 */
    size_t len;          /**< 数据区总长。 */
    size_t pos;          /**< 当前读取位置。 */
} fr_buf_reader;

/**
 * 初始化读取器。
 *
 * @param[out] r 读取器。
 * @param[in] data 数据区；len == 0 时可为 NULL。
 * @param[in] len 数据区长度。
 */
void fr_buf_reader_init(fr_buf_reader *r, const void *data, size_t len);

/**
 * 返回剩余可读字节数。
 *
 * @param[in] r 读取器。
 * @return 剩余字节数。
 */
size_t fr_buf_reader_remaining(const fr_buf_reader *r);

/**
 * 读取 n 字节到 dst。
 *
 * @param[in,out] r 读取器。
 * @param[out] dst 目标缓冲；调用方保证至少 n 字节可写。
 * @param[in] n 读取字节数。
 * @retval FR_OK 成功，位置前移 n。
 * @retval FR_E_RANGE 剩余不足；位置不变。
 */
fr_status fr_buf_reader_read(fr_buf_reader *r, void *dst, size_t n);

/**
 * 读取 1 字节。
 *
 * @param[in,out] r 读取器。
 * @param[out] out 值。
 * @retval FR_OK 成功。
 * @retval FR_E_RANGE 剩余不足。
 */
fr_status fr_buf_reader_u8(fr_buf_reader *r, uint8_t *out);

/**
 * 读取 2 字节大端整数。
 *
 * @param[in,out] r 读取器。
 * @param[out] out 值。
 * @retval FR_OK 成功。
 * @retval FR_E_RANGE 剩余不足。
 */
fr_status fr_buf_reader_u16be(fr_buf_reader *r, uint16_t *out);

/**
 * 读取 4 字节大端整数。
 *
 * @param[in,out] r 读取器。
 * @param[out] out 值。
 * @retval FR_OK 成功。
 * @retval FR_E_RANGE 剩余不足。
 */
fr_status fr_buf_reader_u32be(fr_buf_reader *r, uint32_t *out);

/**
 * 读取 8 字节大端整数。
 *
 * @param[in,out] r 读取器。
 * @param[out] out 值。
 * @retval FR_OK 成功。
 * @retval FR_E_RANGE 剩余不足。
 */
fr_status fr_buf_reader_u64be(fr_buf_reader *r, uint64_t *out);

/**
 * 跳过 n 字节。
 *
 * @param[in,out] r 读取器。
 * @param[in] n 跳过字节数。
 * @retval FR_OK 成功。
 * @retval FR_E_RANGE 剩余不足。
 */
fr_status fr_buf_reader_skip(fr_buf_reader *r, size_t n);

/**
 * 读取 n 字节并追加到 out 缓冲。
 *
 * @param[in,out] r 读取器。
 * @param[out] out 目标缓冲（追加）。
 * @param[in] n 读取字节数。
 * @retval FR_OK 成功，位置前移 n。
 * @retval FR_E_RANGE 剩余不足；out 不变。
 * @retval FR_E_NOMEM out 扩容失败。
 */
fr_status fr_buf_reader_bytes(fr_buf_reader *r, fr_buf *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* FR_BUFFER_H */
