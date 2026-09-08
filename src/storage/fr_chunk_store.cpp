// fr_chunk_store.cpp - 内容寻址块存储实现。
#include "forgerelay/storage/chunk_store.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "forgerelay/fr_crc32.h"
#include "forgerelay/fr_byteorder.h"
#include "forgerelay/fr_fd.h"
#include "forgerelay/fr_hex.h"
#include "forgerelay/fr_path.h"
#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"

namespace fr {

namespace {

void store_header(uint8_t header[kChunkHeaderSize], size_t raw_len, uint32_t crc)
{
    std::memset(header, 0, kChunkHeaderSize);
    header[0] = 'F';
    header[1] = 'R';
    header[2] = 'C';
    header[3] = 'H';
    header[4] = kChunkFormatVersion;
    header[5] = 0; // reserved
    fr_store_be64(header + 6, static_cast<uint64_t>(raw_len));
    fr_store_be64(header + 14, static_cast<uint64_t>(raw_len)); // stored == raw（不压缩）
    fr_store_be32(header + 22, crc);
    fr_store_be32(header + 26, 0); // reserved
}

struct ChunkMeta {
    uint64_t raw_len = 0;
    uint64_t stored_len = 0;
    uint32_t crc = 0;
};

ChunkMeta parse_header(const uint8_t header[kChunkHeaderSize])
{
    if (header[0] != 'F' || header[1] != 'R' || header[2] != 'C' || header[3] != 'H') {
        throw_error(FR_E_IO, "chunk file magic mismatch");
    }
    if (header[4] != kChunkFormatVersion) {
        throw_error(FR_E_IO, "chunk file version mismatch");
    }
    if (header[5] != 0) {
        throw_error(FR_E_IO, "chunk file reserved byte nonzero");
    }
    ChunkMeta meta;
    meta.raw_len = fr_load_be64(header + 6);
    meta.stored_len = fr_load_be64(header + 14);
    meta.crc = fr_load_be32(header + 22);
    if (fr_load_be32(header + 26) != 0) {
        throw_error(FR_E_IO, "chunk file reserved field nonzero");
    }
    if (meta.raw_len != meta.stored_len) {
        throw_error(FR_E_IO, "chunk stored length mismatch (compression unsupported)");
    }
    return meta;
}

} // namespace

void ChunkStore::open(const std::filesystem::path &root)
{
    fr_error err;
    fr_status st = fr_dir_create_all(root.string().c_str(), &err);
    if (st != FR_OK) {
        throw_error(st, std::string("create storage root: ") + err.detail);
    }
    chunks_dir_ = root / "chunks";
    tmp_dir_ = root / "tmp";
    st = fr_dir_create(chunks_dir_.string().c_str(), &err);
    if (st != FR_OK) {
        throw_error(st, std::string("create chunks dir: ") + err.detail);
    }
    st = fr_dir_create(tmp_dir_.string().c_str(), &err);
    if (st != FR_OK) {
        throw_error(st, std::string("create tmp dir: ") + err.detail);
    }
    root_ = root;
}

std::string ChunkStore::relative_path(const std::string &digest_hex)
{
    char rel[FR_CHUNK_REL_PATH_LEN];
    if (fr_chunk_rel_path(digest_hex.c_str(), rel, sizeof(rel)) != FR_OK) {
        throw_error(FR_E_ARG, "invalid digest for chunk path");
    }
    return std::string(rel);
}

std::filesystem::path ChunkStore::chunk_path(const std::string &digest_hex) const
{
    return chunks_dir_ / std::filesystem::path(relative_path(digest_hex));
}

void ChunkStore::write_chunk_file(const std::filesystem::path &target, const void *data, size_t len)
{
    char tmp_name[FR_TMPNAME_MAX];
    fr_error err;
    fr_fd fd = FR_FD_INVALID;
    fr_status st = fr_tmpfile_create(tmp_dir_.string().c_str(), "chunk", tmp_name, sizeof(tmp_name),
                                     &fd, &err);
    if (st != FR_OK) {
        throw_error(st, std::string("create temp chunk: ") + err.detail);
    }

    auto fail = [&](fr_status code, const std::string &what) {
        (void)fr_fd_close(&fd, nullptr);
        (void)fr_file_unlink((tmp_dir_ / tmp_name).string().c_str(), nullptr);
        throw_error(code, what);
    };

    uint8_t header[kChunkHeaderSize];
    store_header(header, len, fr_crc32(data, len));
    st = fr_fd_write_all(fd, header, sizeof(header), &err);
    if (st != FR_OK) {
        fail(st, std::string("write chunk header: ") + err.detail);
    }
    if (len != 0) {
        st = fr_fd_write_all(fd, data, len, &err);
        if (st != FR_OK) {
            fail(st, std::string("write chunk data: ") + err.detail);
        }
    }
    st = fr_fd_fsync(fd, &err);
    if (st != FR_OK) {
        fail(st, std::string("fsync chunk: ") + err.detail);
    }
    st = fr_fd_close(&fd, &err);
    if (st != FR_OK) {
        fail(st, std::string("close chunk: ") + err.detail);
    }

    const std::filesystem::path tmp_path = tmp_dir_ / tmp_name;
    /* 原子落位（FR-STO-04）：先确保两级摘要目录存在，再重命名。 */
    fr_status dir_st = fr_dir_create_all(target.parent_path().string().c_str(), &err);
    if (dir_st != FR_OK) {
        (void)fr_file_unlink(tmp_path.string().c_str(), nullptr);
        throw_error(dir_st, std::string("create chunk fan-out dir: ") + err.detail);
    }
    /* 目标已存在（并发/重复写入）则复用已有文件。 */
    if (std::filesystem::exists(target)) {
        (void)fr_file_unlink(tmp_path.string().c_str(), nullptr);
        return;
    }
    st = fr_file_rename(tmp_path.string().c_str(), target.string().c_str(), &err);
    if (st != FR_OK) {
        (void)fr_file_unlink(tmp_path.string().c_str(), nullptr);
        if (std::filesystem::exists(target)) {
            return; // 输掉的竞速者：复用赢家文件。
        }
        throw_error(st, std::string("rename chunk into place: ") + err.detail);
    }
}

std::string ChunkStore::put(const void *data, size_t len, const std::string &digest_hex)
{
    if (data == nullptr && len != 0) {
        throw_error(FR_E_ARG, "chunk put: null buffer with length");
    }
    if (!fr_digest_hex_valid(digest_hex.c_str())) {
        throw_error(FR_E_ARG, "chunk put: invalid digest");
    }
    /* 内容与摘要复核：块文件路径由摘要决定，写入错误=数据损坏（FR-STO-02/05）。 */
    const Sha256 actual = sha256(data, len);
    if (sha256_to_hex(actual) != digest_hex) {
        throw_error(FR_E_CONFLICT, "chunk put: digest does not match content");
    }

    const std::filesystem::path target = chunk_path(digest_hex);
    if (!std::filesystem::exists(target)) {
        write_chunk_file(target, data, len);
    }
    return digest_hex;
}

void ChunkStore::get(const std::string &digest_hex, fr_buf *out)
{
    get_range(digest_hex, 0, SIZE_MAX, out);
}

void ChunkStore::get_range(const std::string &digest_hex, uint64_t offset, size_t len, fr_buf *out)
{
    if (out == nullptr || !fr_digest_hex_valid(digest_hex.c_str())) {
        throw_error(FR_E_ARG, "chunk get: bad arguments");
    }
    const std::filesystem::path path = chunk_path(digest_hex);
    fr_error err;
    fr_fd fd = FR_FD_INVALID;
    fr_status st = fr_fd_open_read(path.string().c_str(), &fd, &err);
    if (st == FR_E_NOTFOUND) {
        throw_error(FR_E_NOTFOUND, "chunk file missing");
    }
    if (st != FR_OK) {
        throw_error(st, std::string("open chunk: ") + err.detail);
    }

    auto fail = [&](fr_status code, const std::string &what) {
        (void)fr_fd_close(&fd, nullptr);
        throw_error(code, what);
    };

    uint8_t header[kChunkHeaderSize];
    st = fr_fd_read_full(fd, header, sizeof(header), &err);
    if (st != FR_OK) {
        fail(st == FR_E_EOF ? FR_E_IO : st, "read chunk header: " + std::string(err.detail));
    }
    const ChunkMeta meta = parse_header(header);

    if (offset > meta.raw_len) {
        (void)fr_fd_close(&fd, nullptr);
        throw_error(FR_E_RANGE, "chunk range out of bounds");
    }
    /* 超出块尾的请求截断到块尾；SIZE_MAX（get() 语义）即"读到块尾"。 */
    size_t remaining = len;
    const uint64_t available = meta.raw_len - offset;
    if (static_cast<uint64_t>(remaining) > available) {
        remaining = static_cast<size_t>(available);
    }
    st = fr_fd_seek_set(fd, offset + kChunkHeaderSize, &err);
    if (st != FR_OK) {
        fail(st, "seek chunk: " + std::string(err.detail));
    }

    fr_buf_reserve(out, remaining);
    const size_t base = out->len;
    if (remaining != 0) {
        st = fr_fd_read_full(fd, out->data + base, remaining, &err);
        if (st != FR_OK) {
            fail(st, "read chunk data: " + std::string(err.detail));
        }
        out->len = base + remaining;
    }
    st = fr_fd_close(&fd, &err);
    if (st != FR_OK) {
        throw_error(st, "close chunk: " + std::string(err.detail));
    }

    /* 数据完整性复核（块头 CRC 字段，FR-STO-03）：
     * 仅整块读取时校验（部分读取无法对齐整块 CRC）。 */
    if (offset == 0 && out->len - base == meta.raw_len &&
        fr_crc32(out->data + base, out->len - base) != meta.crc) {
        throw_error(FR_E_IO, "chunk crc mismatch (corrupted chunk file)");
    }
}

uint64_t ChunkStore::raw_size(const std::string &digest_hex)
{
    const std::filesystem::path path = chunk_path(digest_hex);
    fr_error err;
    uint64_t size = 0;
    if (fr_file_size(path.string().c_str(), &size, &err) != FR_OK) {
        throw_error(FR_E_IO, std::string("stat chunk: ") + err.detail);
    }
    if (size < kChunkHeaderSize) {
        throw_error(FR_E_IO, "chunk file truncated header");
    }
    return size - kChunkHeaderSize;
}

bool ChunkStore::exists(const std::string &digest_hex)
{
    if (!fr_digest_hex_valid(digest_hex.c_str())) {
        return false;
    }
    fr_error err;
    bool present = false;
    if (fr_file_exists(chunk_path(digest_hex).string().c_str(), &present, &err) != FR_OK) {
        return false;
    }
    return present;
}

void ChunkStore::remove(const std::string &digest_hex)
{
    const std::filesystem::path path = chunk_path(digest_hex);
    fr_error err;
    fr_status st = fr_file_unlink(path.string().c_str(), &err);
    if (st == FR_E_NOTFOUND) {
        return; // 幂等删除（LIFE-02）。
    }
    if (st != FR_OK) {
        throw_error(st, std::string("remove chunk: ") + err.detail);
    }
}

int ChunkStore::cleanup_temp_files()
{
    int removed = 0;
    std::error_code ec;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(tmp_dir_, ec)) {
        if (ec) {
            break;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind("frtmp-", 0) == 0) {
            fr_error err;
            if (fr_file_unlink(entry.path().string().c_str(), &err) == FR_OK) {
                removed++;
            }
        }
    }
    return removed;
}

uint64_t ChunkStore::stored_bytes() const
{
    uint64_t total = 0;
    std::error_code ec;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::recursive_directory_iterator(chunks_dir_, ec)) {
        std::error_code entry_ec;
        if (entry.is_regular_file(entry_ec)) {
            const uintmax_t size = entry.file_size(entry_ec);
            if (!entry_ec) {
                total += static_cast<uint64_t>(size);
            }
        }
    }
    return total;
}

} // namespace fr
