/*
 * fr_fd.h - 文件描述符与文件系统原语封装（FR-DL-02、FR-STO-04、LIFE-02/04、SEC-03）。
 *
 * 目标平台为 POSIX（Linux x86_64）；开发验证环境（Windows）经同一文件内的
 * 受控 _WIN32 分支适配（DECISIONS D-08），平台差异不出本模块。
 *
 * 约定：
 *  - 所有失败经 errno 映射为稳定 fr_status，并尽量写入 fr_error 详情（含 errno 短语，
 *    不含路径以外的敏感信息）；
 *  - 读写返回字节数的调用具备 EINTR 安全性；
 *  - 块/临时文件以 0600 创建，目录以 0700 创建（SEC-03 最小权限）。
 *
 * 所有权：fr_fd 是对系统 fd 的整数包装，fr_fd_close() 后置为 FR_FD_INVALID（幂等）。
 * 线程安全：不同 fd 上的操作互不影响；同一 fd 需调用方同步；无全局可变状态
 *          （临时文件计数器为进程级原子计数）。
 */
#ifndef FR_FD_H
#define FR_FD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "forgerelay/fr_buffer.h"
#include "forgerelay/fr_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 无效描述符值。 */
#define FR_FD_INVALID (-1)
/** 临时文件名缓冲的建议容量。 */
#define FR_TMPNAME_MAX 128

/** 文件描述符包装。 */
typedef int fr_fd;

/**
 * 以只读方式打开文件（FR_E_NOTFOUND 对应路径不存在）。
 *
 * @param[in] path 文件路径。
 * @param[out] out 成功时写入新 fd。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_ARG 参数非法。
 * @retval FR_E_NOTFOUND 路径不存在。
 * @retval FR_E_IO 其他打开失败。
 */
fr_status fr_fd_open_read(const char *path, fr_fd *out, fr_error *err);

/**
 * 以写入方式打开文件：创建或截断；exclusive 时要求不存在（O_EXCL）。
 *
 * @param[in] path 文件路径。
 * @param[in] exclusive true 时仅在文件不存在时创建（用于临时文件，FR-STO-04）。
 * @param[out] out 成功时写入新 fd。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_EXISTS exclusive 且文件已存在。
 * @retval FR_E_NOTFOUND 父目录不存在。
 * @retval FR_E_IO 其他打开失败。
 */
fr_status fr_fd_open_write(const char *path, bool exclusive, fr_fd *out, fr_error *err);

/**
 * 关闭 fd；幂等（LIFE-02）：无论成败，*fd 复位为 FR_FD_INVALID。
 *
 * @param[in,out] fd 指向 fd 的指针；不可为 NULL。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_IO 关闭系统调用失败（fd 仍被复位）。
 */
fr_status fr_fd_close(fr_fd *fd, fr_error *err);

/**
 * 单次读取语义：读至多 n 字节；*nread == 0 表示 EOF。
 *
 * @param[in] fd 文件描述符。
 * @param[out] buf 目标缓冲；n > 0 时不可为 NULL。
 * @param[in] n 期望读取的最大字节数。
 * @param[out] nread 实际读取字节数。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功（含 EOF）。
 * @retval FR_E_ARG 参数非法。
 * @retval FR_E_IO 读取失败。
 */
fr_status fr_fd_read(fr_fd fd, void *buf, size_t n, size_t *nread, fr_error *err);

/**
 * 写入全部 n 字节；内部处理短写与 EINTR。
 *
 * @param[in] fd 文件描述符。
 * @param[in] buf 数据；n > 0 时不可为 NULL。
 * @param[in] n 字节数。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_ARG 参数非法。
 * @retval FR_E_IO 写入失败。
 */
fr_status fr_fd_write_all(fr_fd fd, const void *buf, size_t n, fr_error *err);

/**
 * 精确读取 n 字节；不足视为 FR_E_EOF。
 *
 * @param[in] fd 文件描述符。
 * @param[out] buf 目标缓冲；n > 0 时不可为 NULL。
 * @param[in] n 字节数。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_EOF 提前到达文件尾。
 * @retval FR_E_ARG / FR_E_IO 同上。
 */
fr_status fr_fd_read_full(fr_fd fd, void *buf, size_t n, fr_error *err);

/**
 * 移动读写位置到 offset（SEEK_SET）。
 *
 * @param[in] fd 文件描述符。
 * @param[in] offset 绝对偏移。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_RANGE offset 超出实现支持范围。
 * @retval FR_E_IO 定位失败。
 */
fr_status fr_fd_seek_set(fr_fd fd, uint64_t offset, fr_error *err);

/**
 * 刷盘：将 fd 关联文件的数据同步到存储（FR-STO-04）。
 *
 * @param[in] fd 文件描述符。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_IO 同步失败。
 */
fr_status fr_fd_fsync(fr_fd fd, fr_error *err);

/**
 * 原子替换重命名：to 已存在时原子覆盖（FR-STO-04）。
 *
 * @param[in] from 源路径。
 * @param[in] to 目标路径。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_NOTFOUND 源或父目录不存在。
 * @retval FR_E_IO 其他失败。
 */
fr_status fr_file_rename(const char *from, const char *to, fr_error *err);

/**
 * 删除文件；不存在返回 FR_E_NOTFOUND（调用方可自行实现幂等删除）。
 *
 * @param[in] path 文件路径。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_NOTFOUND 不存在。
 * @retval FR_E_IO 其他失败。
 */
fr_status fr_file_unlink(const char *path, fr_error *err);

/**
 * 判断路径存在与否（符号链接跟随到目标）。
 *
 * @param[in] path 路径。
 * @param[out] out 是否存在；仅 FR_OK 时有意义。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_ARG 参数非法。
 * @retval FR_E_IO stat 失败（非 ENOENT）。
 */
fr_status fr_file_exists(const char *path, bool *out, fr_error *err);

/**
 * 查询常规文件大小（字节）。
 *
 * @param[in] path 文件路径。
 * @param[out] out 大小。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_NOTFOUND 路径不存在。
 * @retval FR_E_RANGE 路径存在但不是常规文件。
 * @retval FR_E_IO 其他失败。
 */
fr_status fr_file_size(const char *path, uint64_t *out, fr_error *err);

/**
 * 读取整个文件到内存，但不超过 max_bytes（避免整文件载入内存，FR-DL-03）。
 *
 * @param[in] path 文件路径。
 * @param[out] out 目标缓冲（重置后填充）。
 * @param[in] max_bytes 允许的最大字节数。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_LIMIT 文件大于 max_bytes。
 * @retval FR_E_NOTFOUND / FR_E_RANGE / FR_E_NOMEM / FR_E_IO 同上各语义。
 */
fr_status fr_file_read_limited(const char *path, fr_buf *out, size_t max_bytes, fr_error *err);

/**
 * 创建目录；已存在且确为目录时幂等成功（LIFE-02）。
 *
 * @param[in] path 目录路径。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功或已存在。
 * @retval FR_E_NOTFOUND 父目录缺失。
 * @retval FR_E_EXISTS 同名但不是目录。
 * @retval FR_E_IO 其他失败。
 */
fr_status fr_dir_create(const char *path, fr_error *err);

/**
 * 逐级创建目录（mkdir -p 语义）。
 *
 * @param[in] path 目录路径；绝对或相对均可。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功（或已存在）。
 * @retval FR_E_ARG / FR_E_RANGE / FR_E_IO 同上各语义。
 */
fr_status fr_dir_create_all(const char *path, fr_error *err);

/**
 * 同步目录项以保证 rename/unlink 的持久性（FR-STO-04）。
 * POSIX 上 fsync 目录 fd；Windows 开发分支为受控 no-op（D-08）。
 *
 * @param[in] path 目录路径。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_NOTFOUND 目录不存在。
 * @retval FR_E_IO 同步失败。
 */
fr_status fr_dir_sync(const char *path, fr_error *err);

/**
 * 在 dir 下创建独占临时文件（O_CREAT|O_EXCL、0600），返回 fd 与文件名。
 *
 * 名称形如 "frtmp-<prefix>-<pid>-<tick>-<counter>.part"；唯一性由 O_EXCL 保证，
 * 冲突时有限次重试（D-09：唯一性而非机密性）。
 *
 * @param[in] dir 目标目录（须已存在）。
 * @param[in] prefix 名称前缀；NULL 视为 "t"；仅允许 [A-Za-z0-9._-]{0,32}。
 * @param[out] name_out 文件名（不含目录）；容量需 >= FR_TMPNAME_MAX。
 * @param[in] name_cap name_out 容量。
 * @param[out] out 成功时写入新 fd。
 * @param[out] err 可选错误详情。
 * @retval FR_OK 成功。
 * @retval FR_E_ARG 参数非法（含前缀字符非法）。
 * @retval FR_E_RANGE name_cap 不足。
 * @retval FR_E_IO 重试后仍无法创建。
 */
fr_status fr_tmpfile_create(const char *dir, const char *prefix, char *name_out, size_t name_cap,
                            fr_fd *out, fr_error *err);

#ifdef __cplusplus
}
#endif

#endif /* FR_FD_H */
