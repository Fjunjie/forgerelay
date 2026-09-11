// fr/chunk_store.hpp - 内容寻址块存储（FR-STO-01…04）。
//
// - 块文件路径仅由 SHA-256 摘要生成：<root>/chunks/<hex[0:2]>/<hex[2:]>（FR-STO-02）；
// - 块文件头部（FR-STO-03，大端）：
//     [0..3]  magic "FRCH"
//     [4]     format version = 1
//     [5]     reserved = 0
//     [6..13] raw length（原始数据长度）
//     [14..21] stored length（存储长度；首版不压缩，恒等于 raw length，FR-STO-07）
//     [22..25] CRC32 of stored data
//     [26..31] reserved = 0
// - 写入：<root>/tmp 下临时文件（0600）→ 写头部+数据 → fsync → 原子 rename
//   进入 chunks/（FR-STO-04）；同内容重复写入幂等复用（FR-STO-01）。
//
// 线程安全：单个实例的方法可被多线程调用（内部互斥保护临时命名与去重路径），
// 但同一摘要的并发写入以“后到者复用先到者结果”处理，不保证文件级并行写。
#ifndef FR_STORAGE_CHUNK_STORE_HPP
#define FR_STORAGE_CHUNK_STORE_HPP

#include <cstdint>
#include <filesystem>
#include <string>

#include "forgerelay/fr_buffer.h"

namespace fr {

/** 块文件固定头长度。 */
constexpr size_t kChunkHeaderSize = 32;
/** 块文件格式版本。 */
constexpr uint8_t kChunkFormatVersion = 1;

class ChunkStore {
public:
    ChunkStore() = default;

    /**
     * 打开（必要时创建）块存储目录结构 chunks/ 与 tmp/。
     *
     * @param[in] root 存储根目录。
     * @throws fr::Error 目录创建失败。
     */
    void open(const std::filesystem::path &root);

    /**
     * 写入一个数据块：校验为空、写临时文件、同步、原子落位；重复内容幂等。
     *
     * @param[in] data 数据。
     * @param[in] len 数据长度。
     * @param[in] digest_hex 64 位小写 hex 摘要（调用方先验证，FR-UP-05）。
     * @return 摘要（与入参一致，便于链式使用）。
     * @throws fr::Error I/O 失败或摘要与内容不符（内部复核）。
     */
    std::string put(const void *data, size_t len, const std::string &digest_hex);

    /**
     * 读取整块数据（含头部校验）。
     *
     * @param[in] digest_hex 摘要。
     * @param[out] out 追加存储的数据（不含头部）。
     * @throws fr::Error FR_E_NOTFOUND 不存在；FR_E_IO 头部/数据损坏。
     */
    void get(const std::string &digest_hex, fr_buf *out);

    /**
     * 读取块内范围 [offset, offset + len)。
     *
     * @param[in] digest_hex 摘要。
     * @param[in] offset 块内偏移。
     * @param[in] len 请求长度（可超尾，实际读出为截断后长度）。
     * @param[out] out 追加读取的数据。
     */
    void get_range(const std::string &digest_hex, uint64_t offset, size_t len, fr_buf *out);

    /** 块原始长度；不存在抛 FR_E_NOTFOUND。 */
    uint64_t raw_size(const std::string &digest_hex);

    /** 块文件是否已存在。 */
    bool exists(const std::string &digest_hex);

    /**
     * 删除块文件（用于 GC）。
     *
     * @param[in] digest_hex 摘要。
     * @throws fr::Error 删除失败（不存在视为成功，幂等）。
     */
    void remove(const std::string &digest_hex);

    /**
     * 清理遗留临时文件（启动恢复，FR-STO-06）。
     *
     * @return 删除的临时文件数量。
     */
    int cleanup_temp_files();

    /** 块文件总字节（含 32 字节头；容量/高水位口径）。 */
    uint64_t stored_bytes() const;

    /** 块文件相对逻辑路径 "ab/cdef…"（DB-04）。 */
    static std::string relative_path(const std::string &digest_hex);

private:
    std::filesystem::path chunk_path(const std::string &digest_hex) const;
    void write_chunk_file(const std::filesystem::path &target, const void *data, size_t len);

    std::filesystem::path root_;
    std::filesystem::path chunks_dir_;
    std::filesystem::path tmp_dir_;
};

} // namespace fr

#endif /* FR_STORAGE_CHUNK_STORE_HPP */
