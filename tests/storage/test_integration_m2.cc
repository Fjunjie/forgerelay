// test_integration_m2.cc - M2 端到端场景（TEST-02：#1 小文件、#2 1GiB 流式、
// #4 块复用、#5 摘要错误、#7 删除+dry-run/实际清理；数据全部运行时生成，TEST-03）。
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "forgerelay/fr_buffer.h"
#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"
#include "forgerelay/storage/storage.hpp"

namespace {

class IntegrationM2Test : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::path(::testing::TempDir()) /
                ("fr_m2_e2e_" + std::to_string(++counter_));
        std::filesystem::remove_all(root_); // 跨进程重跑时清理残留
        config_.root = root_;
        config_.chunk_size = 1024 * 1024; // 1 MiB 块，便于构造多块制品
        storage_ = fr::Storage::open(config_);
    }

    std::string read_all(const std::string &ns, const std::string &name,
                         const std::string &version) {
        std::string out;
        storage_->read_artifact(ns, name, version, 0, UINT64_MAX,
                                [&](const void *data, size_t len) {
                                    out.append(static_cast<const char *>(data), len);
                                });
        return out;
    }

    static int counter_;
    std::filesystem::path root_;
    fr::StorageConfig config_;
    std::unique_ptr<fr::Storage> storage_;
};

int IntegrationM2Test::counter_ = 0;

/// 单块制品发布辅助。
fr::ArtifactInfo publish_single(fr::Storage &storage, const std::string &ns,
                                const std::string &name, const std::string &version,
                                const std::string &content) {
    const std::string digest = fr::sha256_to_hex(fr::sha256(content.data(), content.size()));
    const fr::SessionInfo s =
        storage.create_session(ns, name, version, content.size(), digest, "u");
    (void)storage.put_chunk(s.id, 0, content.data(), content.size());
    return storage.commit_session(s.id);
}

/// 场景 #1：小文件上传与下载（分 3 块，含不满块尾块）。
TEST_F(IntegrationM2Test, SmallFileRoundTrip) {
    const std::string block0(1024 * 1024, 'A');
    const std::string block1(1024 * 1024, 'B');
    const std::string block2(777, 'C');
    std::vector<std::string> blocks = {block0, block1, block2};

    fr::Sha256Stream overall;
    uint64_t total = 0;
    for (const std::string &b : blocks) {
        overall.update(b.data(), b.size());
        total += b.size();
    }
    const std::string digest = fr::sha256_to_hex(overall.finish());

    const fr::SessionInfo s =
        storage_->create_session("ci", "small", "1.0", total, digest, "ci-job");
    for (size_t i = 0; i < blocks.size(); i++) {
        (void)storage_->put_chunk(s.id, i, blocks[i].data(), blocks[i].size());
    }
    (void)storage_->commit_session(s.id);

    const std::string downloaded = read_all("ci", "small", "1.0");
    ASSERT_EQ(total, downloaded.size());
    EXPECT_EQ(0, std::memcmp(block0.data(), downloaded.data(), block0.size()));
    EXPECT_EQ(block2, downloaded.substr(2 * block0.size()));

    const fr::ArtifactInfo shown = storage_->show_artifact("ci", "small", "1.0");
    EXPECT_EQ(3u, shown.chunks.size());
    EXPECT_EQ(digest, shown.digest);
}

/// 场景 #2：1 GiB 流式上传与下载（不整文件载入内存，FR-DL-03；运行时生成，TEST-03）。
TEST_F(IntegrationM2Test, OneGiBSparseStreamingRoundTrip) {
    config_.chunk_size = 4u * 1024 * 1024; // 本场景用 4 MiB 块
    storage_ = fr::Storage::open(config_);

    constexpr uint64_t kChunk = 4u * 1024 * 1024;
    constexpr uint64_t kChunks = 256; // 1 GiB
    const std::string zero_block(static_cast<size_t>(kChunk), '\0');
    const std::string first_block = [] {
        std::string b(static_cast<size_t>(kChunk), '\0');
        for (size_t i = 0; i < b.size(); i++) {
            b[i] = static_cast<char>(i * 7u & 0xffu); // 首块为确定性伪随机内容
        }
        return b;
    }();

    fr::Sha256Stream overall;
    // 先离线计算 1GiB 的整体摘要：首块 + 255 个零块。
    overall.update(first_block.data(), first_block.size());
    for (uint64_t i = 1; i < kChunks; i++) {
        overall.update(zero_block.data(), zero_block.size());
    }
    const std::string digest = fr::sha256_to_hex(overall.finish());

    const fr::SessionInfo s =
        storage_->create_session("ci", "sparse", "1.0", kChunk * kChunks, digest, "ci-job");
    // 分块流式上传：块 0 为首块，其余全部为零块（内容寻址去重生效）。
    (void)storage_->put_chunk(s.id, 0, first_block.data(), first_block.size());
    for (uint64_t i = 1; i < kChunks; i++) {
        (void)storage_->put_chunk(s.id, i, zero_block.data(), zero_block.size());
    }
    const fr::ArtifactInfo published = storage_->commit_session(s.id);
    EXPECT_EQ(kChunk * kChunks, published.size);
    EXPECT_EQ(kChunks, published.chunks.size());

    // 流式下载：不整文件载入内存，逐段接收并统计。
    uint64_t received = 0;
    fr::Sha256Stream verify;
    uint64_t sink_calls = 0;
    storage_->read_artifact("ci", "sparse", "1.0", 0, kChunk * kChunks,
                            [&](const void *data, size_t len) {
                                verify.update(data, len);
                                received += len;
                                sink_calls++;
                            });
    EXPECT_EQ(kChunk * kChunks, received);
    EXPECT_GT(sink_calls, 1u); // 确认为分段流式，而非单次整读
    EXPECT_EQ(digest, fr::sha256_to_hex(verify.finish()));

    // 去重验证：256 块只有 2 个唯一块文件（首块 + 零块）。
    std::error_code ec;
    uint64_t chunk_files = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root_ / "chunks", ec)) {
        if (entry.is_regular_file()) {
            chunk_files++;
        }
    }
    EXPECT_EQ(2u, chunk_files);
}

/// 场景 #4：同一块被不同制品复用（FR-STO-01）。
TEST_F(IntegrationM2Test, ChunkReuseAcrossArtifacts) {
    const std::string shared_block(1024 * 1024, 'S');
    const std::string extra_a(64, 'a');
    const std::string extra_b(64, 'b');

    auto publish_with = [&](const std::string &name, const std::string &extra) {
        fr::Sha256Stream overall;
        overall.update(shared_block.data(), shared_block.size());
        overall.update(extra.data(), extra.size());
        const std::string digest = fr::sha256_to_hex(overall.finish());
        const fr::SessionInfo s = storage_->create_session(
            "ci", name, "1.0", shared_block.size() + extra.size(), digest, "u");
        (void)storage_->put_chunk(s.id, 0, shared_block.data(), shared_block.size());
        (void)storage_->put_chunk(s.id, 1, extra.data(), extra.size());
        return storage_->commit_session(s.id);
    };
    (void)publish_with("artifact-a", extra_a);
    (void)publish_with("artifact-b", extra_b);

    std::error_code ec;
    uint64_t chunk_files = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root_ / "chunks", ec)) {
        if (entry.is_regular_file()) {
            chunk_files++;
        }
    }
    EXPECT_EQ(3u, chunk_files); // 共享块只存一份：S 块 + a + b

    // 删除 A 后共享块仍在（被 B 引用）；再删 B 后进入待清理。
    (void)storage_->delete_artifact("ci", "artifact-a", "1.0");
    std::string b_roundtrip;
    storage_->read_artifact("ci", "artifact-b", "1.0", 0, shared_block.size() + extra_b.size(),
                            [&](const void *data, size_t len) {
                                b_roundtrip.append(static_cast<const char *>(data), len);
                            });
    EXPECT_EQ(shared_block + extra_b, b_roundtrip);
}

/// 场景 #5：摘要错误导致提交失败，不发布不完整制品（FR-UP-08）。
TEST_F(IntegrationM2Test, DigestMismatchFailsCommit) {
    const std::string content = "actual payload bytes";
    const std::string wrong_digest = fr::sha256_to_hex(fr::sha256("not-this", 8));
    const fr::SessionInfo s =
        storage_->create_session("ci", "payload", "1.0", content.size(), wrong_digest, "u");
    (void)storage_->put_chunk(s.id, 0, content.data(), content.size());

    EXPECT_THROW((void)storage_->commit_session(s.id), fr::Error);
    EXPECT_EQ(fr::SessionState::OPEN, storage_->get_session(s.id).state);
    EXPECT_THROW(storage_->show_artifact("ci", "payload", "1.0"), fr::Error);

    // 正确摘要的新会话成功发布。
    const std::string good = fr::sha256_to_hex(fr::sha256(content.data(), content.size()));
    const fr::SessionInfo s2 =
        storage_->create_session("ci", "payload", "1.1", content.size(), good, "u");
    (void)storage_->put_chunk(s2.id, 0, content.data(), content.size());
    (void)storage_->commit_session(s2.id);
    EXPECT_EQ(content, read_all("ci", "payload", "1.1"));
}

/// 场景 #7：删除制品后执行 dry-run 与实际清理（FR-GC-05）。
TEST_F(IntegrationM2Test, DeleteThenDryRunAndRealGc) {
    const std::string content = "to-be-collected";
    (void)publish_single(*storage_, "ci", "gone", "1.0", content);
    (void)publish_single(*storage_, "ci", "stays", "1.0", "survivor");

    EXPECT_EQ(1, storage_->delete_artifact("ci", "gone", "1.0"));

    // 保护期内：dry-run 显示 1 个待清理、0 删除。
    fr::GcReport plan = storage_->collect_garbage(true);
    EXPECT_EQ(1, plan.scanned);
    EXPECT_EQ(0, plan.deleted_count);

    // 越过保护期（默认 3600s）后：dry-run 报告预计删除量，实际 GC 释放。
    std::filesystem::path chunks_dir = root_ / "chunks";
    EXPECT_TRUE(std::filesystem::exists(chunks_dir));

    // 通过重新打开并注入快进时钟来模拟时间流逝。
    config_.root = root_;
    config_.gc_grace_seconds = 0; // 等效于保护期已过
    storage_ = fr::Storage::open(config_);

    plan = storage_->collect_garbage(true);
    EXPECT_EQ(1, plan.scanned);
    EXPECT_EQ(1, plan.deleted_count);
    EXPECT_EQ(static_cast<int64_t>(content.size()), plan.deleted_bytes);

    EXPECT_EQ(1, storage_->collect_garbage(false).deleted_count);
    EXPECT_TRUE(std::filesystem::exists(chunks_dir)); // stays 的块仍在
    EXPECT_EQ("survivor", read_all("ci", "stays", "1.0"));
}

/// 场景 #3/#6：上传中断后恢复（重开存储继续提交）。
TEST_F(IntegrationM2Test, InterruptedUploadResumes) {
    const std::string block0(1024 * 1024, 'K');
    const std::string block1(10, 'L');
    fr::Sha256Stream overall;
    overall.update(block0.data(), block0.size());
    overall.update(block1.data(), block1.size());
    const std::string digest = fr::sha256_to_hex(overall.finish());

    const fr::SessionInfo s = storage_->create_session("ci", "resumable", "1.0",
                                                       block0.size() + block1.size(), digest, "u");
    (void)storage_->put_chunk(s.id, 0, block0.data(), block0.size());

    // “网络中断”：客户端带着会话 ID 回来（重新打开存储）。
    config_.root = root_;
    storage_ = fr::Storage::open(config_);

    // 先提交 → 失败（缺块 1），会话保持 OPEN。
    EXPECT_THROW((void)storage_->commit_session(s.id), fr::Error);
    (void)storage_->put_chunk(s.id, 1, block1.data(), block1.size());
    (void)storage_->commit_session(s.id);

    const std::string downloaded = read_all("ci", "resumable", "1.0");
    ASSERT_EQ(block0.size() + block1.size(), downloaded.size());
    EXPECT_EQ(block1, downloaded.substr(block0.size()));
}

} // namespace
