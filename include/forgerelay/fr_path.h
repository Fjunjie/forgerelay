/*
 * fr_path.h - 制品标识校验、摘要路径映射与安全路径拼接。
 *
 * 规则（FR-ART-02/03、FR-STO-02、SEC-05、DB-04）：
 *  - namespace/name：[A-Za-z0-9._-]{1,64}，且不得为 "." 或 ".."；
 *  - version：[A-Za-z0-9._+-]{1,96}，且不得为 "." 或 ".."；
 *  - 块文件相对路径仅由 SHA-256 摘要 hex 生成：<hex[0:2]>/<hex[2:]>；
 *  - 相对路径校验拒绝绝对路径、"."/".." 分量、控制字符与非白名单字符。
 *
 * 所有路径以 '/' 分隔（存储层的逻辑路径，DB 只保存该相对路径）。
 * 线程安全：纯函数，线程安全。
 */
#ifndef FR_PATH_H
#define FR_PATH_H

#include <stdbool.h>
#include <stddef.h>

#include "forgerelay/fr_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/** namespace/name 的最大长度（FR-ART-02）。 */
#define FR_IDENT_COMPONENT_MAX 64
/** version 的最大长度（FR-ART-03）。 */
#define FR_IDENT_VERSION_MAX 96
/** 摘要相对路径长度：2 + 1 + 62 + NUL。 */
#define FR_CHUNK_REL_PATH_LEN (2 + 1 + 62 + 1)
/** 逻辑路径缓冲的建议容量。 */
#define FR_PATH_BUF_MAX 1024

/**
 * 校验 namespace 或 name（FR-ART-02）。
 *
 * @param[in] s 待校验字符串；NULL 返回 false。
 * @return true 合法。
 */
bool fr_ident_valid_component(const char *s);

/**
 * 校验 version（FR-ART-03）。允许语义化版本、提交哈希、日期格式。
 *
 * @param[in] s 待校验字符串；NULL 返回 false。
 * @return true 合法。
 */
bool fr_ident_valid_version(const char *s);

/**
 * 由 SHA-256 摘要 hex 生成块文件的相对路径（FR-STO-02）。
 *
 * @param[in] hex_digest 64 位小写 hex 摘要（先经 fr_digest_hex_valid() 语义校验）。
 * @param[out] out 输出缓冲。
 * @param[in] out_cap 缓冲容量；需 >= FR_CHUNK_REL_PATH_LEN。
 * @retval FR_OK 成功，写入形如 "ab/cdef…" 的相对路径（NUL 结尾）。
 * @retval FR_E_ARG 参数为 NULL 或摘要非法。
 * @retval FR_E_RANGE out_cap 不足。
 */
fr_status fr_chunk_rel_path(const char *hex_digest, char *out, size_t out_cap);

/**
 * 校验存储层逻辑相对路径（DB-04、SEC-05）。
 *
 * 规则：非空；不含前导 '/' 或反斜杠；不含空分量（禁止 "//"）；
 * 分量不得为 "." / ".."；只允许 [A-Za-z0-9._/-]；不含控制字符。
 *
 * @param[in] rel 待校验路径。
 * @retval FR_OK 合法。
 * @retval FR_E_ARG rel 为 NULL。
 * @retval FR_E_RANGE 违反规则（长度或字符集）。
 */
fr_status fr_path_validate_rel(const char *rel);

/**
 * 将逻辑相对路径拼接到存储根目录（不做系统调用）。
 *
 * @param[in] root 根目录；非空，末尾多余的 '/' 会被容忍。
 * @param[in] rel 相对路径；必须通过 fr_path_validate_rel()。
 * @param[out] out 输出缓冲。
 * @param[in] out_cap 缓冲容量。
 * @retval FR_OK 成功。
 * @retval FR_E_ARG 参数为 NULL。
 * @retval FR_E_RANGE rel 非法或输出缓冲不足。
 */
fr_status fr_path_join(const char *root, const char *rel, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* FR_PATH_H */
