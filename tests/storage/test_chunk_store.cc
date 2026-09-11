// test_chunk_store.cc - 内容寻址块存储单元测试（FR-STO-01..04）。
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "forgerelay/fr_buffer.h"
#include "forgerelay/storage/chunk_store.hpp"
#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"

namespace {

class ChunkStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::path(::testing::TempDir()) /
                ("fr_m2_chunks_" + std::to_string(++counter_));
        std::filesystem::remove_all(root_); // 跨进程重跑时清理残留
        store_.open(root_);
    }

    static int counter_;
    std::filesystem::path root_;
    fr::ChunkStore store_;
};

int ChunkStoreTest::counter_ = 0;

std::string put_test_chunk(fr::ChunkStore &store, const std::string &content) {
    const std::string digest = fr::sha256_to_hex(fr::sha256(content.data(), content.size()));
    store.put(content.data(), content.size(), digest);
    return digest;
}

TEST_F(ChunkStoreTest, PutGetRoundTrip) {
    const std::string content = "hello forgerelay chunk";
    const std::string digest = put_test_chunk(store_, content);

    fr_buf out;
    fr_buf_init(&out);
    store_.get(digest, &out);
    ASSERT_EQ(content.size(), out.len);
    EXPECT_EQ(0, std::memcmp(content.data(), out.data, out.len));
    fr_buf_destroy(&out);
}

TEST_F(ChunkStoreTest, EmptyChunkRoundTrip) {
    const std::string digest = fr::sha256_to_hex(fr::sha256(nullptr, 0));
    store_.put(nullptr, 0, digest);
    EXPECT_TRUE(store_.exists(digest));
    EXPECT_EQ(0u, store_.raw_size(digest));

    fr_buf out;
    fr_buf_init(&out);
    store_.get(digest, &out);
    EXPECT_EQ(0u, out.len);
    fr_buf_destroy(&out);
}

TEST_F(ChunkStoreTest, DedupStoresSingleFile) {
    const std::string content = "identical content block";
    const std::string digest = put_test_chunk(store_, content);
    store_.put(content.data(), content.size(), digest); // 重复写入幂等
    store_.put(content.data(), content.size(), digest);

    std::error_code ec;
    uint64_t files = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root_ / "chunks", ec)) {
        if (entry.is_regular_file()) {
            files++;
        }
    }
    EXPECT_EQ(1u, files);
    EXPECT_TRUE(store_.exists(digest));
}

TEST_F(ChunkStoreTest, PathDependsOnlyOnDigest) {
    const std::string digest = put_test_chunk(store_, "path-test");
    const std::string rel = fr::ChunkStore::relative_path(digest);
    // FR-STO-02：路径形如 "ab/cdef…"，仅由摘要生成。
    ASSERT_EQ(65u, rel.size());
    EXPECT_EQ('/', rel[2]);
    EXPECT_EQ(std::filesystem::path(root_ / "chunks" / rel), root_ / "chunks" / rel);
    EXPECT_TRUE(std::filesystem::exists(root_ / "chunks" / rel));
}

TEST_F(ChunkStoreTest, RangeReadBoundaries) {
    const std::string content = "0123456789abcdef";
    const std::string digest = put_test_chunk(store_, content);

    fr_buf out;
    fr_buf_init(&out);

    store_.get_range(digest, 0, 4, &out); // 首块
    EXPECT_EQ(std::string("0123"), std::string(reinterpret_cast<char *>(out.data), out.len));

    fr_buf_clear(&out);
    store_.get_range(digest, 12, 4, &out); // 末块
    EXPECT_EQ(std::string("cdef"), std::string(reinterpret_cast<char *>(out.data), out.len));

    fr_buf_clear(&out);
    store_.get_range(digest, 0, 16, &out); // 全量
    EXPECT_EQ(16u, out.len);

    fr_buf_clear(&out);
    store_.get_range(digest, 16, 0, &out); // 尾后零长合法
    EXPECT_EQ(0u, out.len);

    fr_buf_clear(&out);
    store_.get_range(digest, 0, 100, &out); // 超尾截断到 16
    EXPECT_EQ(16u, out.len);

    fr_buf_clear(&out);
    store_.get_range(digest, 8, 100, &out); // 超尾截断到 8
    EXPECT_EQ(8u, out.len);

    fr_buf_clear(&out);
    EXPECT_THROW(store_.get_range(digest, 17, 0, &out), fr::Error); // 起点越界
    fr_buf_destroy(&out);
}

TEST_F(ChunkStoreTest, CorruptedChunkRejectedByCrc) {
    const std::string content = "integrity-checked data";
    const std::string digest = put_test_chunk(store_, content);
    const std::filesystem::path file = root_ / "chunks" / fr::ChunkStore::relative_path(digest);

    // 篡改数据第一个字节（头部之后）。
    std::fstream file_stream(file, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file_stream.is_open());
    file_stream.seekp(static_cast<std::streamoff>(fr::kChunkHeaderSize));
    const char corrupted = 'X';
    file_stream.write(&corrupted, 1);
    file_stream.close();

    fr_buf out;
    fr_buf_init(&out);
    try {
        store_.get(digest, &out);
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_IO, err.code());
    }
    fr_buf_destroy(&out);
}

TEST_F(ChunkStoreTest, MissingChunkNotFound) {
    fr_buf out;
    fr_buf_init(&out);
    const std::string fake(64, 'a');
    try {
        store_.get(fake, &out);
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_NOTFOUND, err.code());
    }
    fr_buf_destroy(&out);
    EXPECT_FALSE(store_.exists(fake));
}

TEST_F(ChunkStoreTest, PutRejectsDigestMismatch) {
    const std::string content = "the actual content";
    const std::string wrong_digest(64, 'a');
    try {
        store_.put(content.data(), content.size(), wrong_digest);
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_CONFLICT, err.code()); // 内容与摘要不符（内部复核）
    }
}

TEST_F(ChunkStoreTest, RemoveIsIdempotent) {
    const std::string digest = put_test_chunk(store_, "to-be-removed");
    EXPECT_TRUE(store_.exists(digest));
    store_.remove(digest);
    EXPECT_FALSE(store_.exists(digest));
    store_.remove(digest); // 二次删除幂等
    SUCCEED();
}

TEST_F(ChunkStoreTest, CleanupTempFilesAndStoredBytes) {
    EXPECT_EQ(0, store_.cleanup_temp_files());
    const std::string digest = put_test_chunk(store_, "measure me");
    // 容量口径：数据 10 字节 + 32 字节块头。
    EXPECT_EQ(10u + fr::kChunkHeaderSize, store_.stored_bytes());

    // 人工放置一个遗留临时文件（模拟崩溃残留，FR-STO-06）。
    const std::filesystem::path leftover = root_ / "tmp" / "frtmp-leftover-x.part";
    {
        std::ofstream junk(leftover, std::ios::binary);
        ASSERT_TRUE(junk.is_open());
        junk.write("junk", 4);
    }
    EXPECT_EQ(1, store_.cleanup_temp_files());
    EXPECT_FALSE(std::filesystem::exists(leftover));
    EXPECT_TRUE(store_.exists(digest)); // 正式块不受影响
}

} // namespace
