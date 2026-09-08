// test_storage_sessions.cc - 上传会话状态机测试（FR-UP-01..09、FR-GC-04）。
#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"
#include "forgerelay/storage/storage.hpp"

namespace {

class StorageSessionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::path(::testing::TempDir()) /
                ("fr_m2_sess_" + std::to_string(++counter_));
        std::filesystem::remove_all(root_); // 跨进程重跑时清理残留
        config_.root = root_;
        config_.clock = [this]() { return now_; };
        storage_ = fr::Storage::open(config_);
    }

    static fr::StorageConfig base_config(const std::filesystem::path &root) {
        fr::StorageConfig config;
        config.root = root;
        return config;
    }

    static int counter_;
    std::filesystem::path root_;
    int64_t now_ = 1'700'000'000;
    fr::StorageConfig config_;
    std::unique_ptr<fr::Storage> storage_;
};

int StorageSessionsTest::counter_ = 0;

TEST_F(StorageSessionsTest, CreateSessionDefaultsAndExpiry) {
    const std::string digest = fr::sha256_to_hex(fr::sha256("x", 1));
    const fr::SessionInfo info =
        storage_->create_session("ci", "app", "1.0.0", 1024, digest, "builder");
    EXPECT_EQ(fr::SessionState::OPEN, info.state);
    EXPECT_EQ(4u * 1024 * 1024, info.chunk_size); // FR-UP-02 默认 4 MiB
    EXPECT_EQ(now_ + 24 * 3600, info.expires_at); // FR-UP-09 默认 24h
    EXPECT_EQ(digest, info.expected_digest);

    const fr::SessionInfo loaded = storage_->get_session(info.id);
    EXPECT_EQ(info.id, loaded.id);
    EXPECT_EQ("builder", loaded.owner);
}

TEST_F(StorageSessionsTest, CreateSessionValidatesInput) {
    const std::string digest = fr::sha256_to_hex(fr::sha256("x", 1));
    EXPECT_THROW(storage_->create_session("bad ns", "app", "1.0", 1, digest, "u"), fr::Error);
    EXPECT_THROW(storage_->create_session("ci", "app/../x", "1.0", 1, digest, "u"), fr::Error);
    EXPECT_THROW(storage_->create_session("ci", "app", "bad version!", 1, digest, "u"), fr::Error);
    EXPECT_THROW(storage_->create_session("ci", "app", "1.0", 1, std::string(64, 'z'), "u"),
                 fr::Error);
    EXPECT_THROW(storage_->create_session("ci", "app", "1.0", 1, digest, ""), fr::Error);
}

TEST_F(StorageSessionsTest, PerOwnerSessionLimit) {
    config_.max_sessions_per_owner = 2;
    config_.root = root_ / "limit";
    storage_ = fr::Storage::open(config_);

    const std::string digest = fr::sha256_to_hex(fr::sha256("x", 1));
    const fr::SessionInfo a = storage_->create_session("ci", "a", "1.0", 1, digest, "u1");
    (void)storage_->create_session("ci", "b", "1.0", 1, digest, "u1");
    try {
        (void)storage_->create_session("ci", "c", "1.0", 1, digest, "u1");
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_LIMIT, err.code()); // 单用户活动会话上限（§7.3）
    }
    // 其他用户不受影响。
    (void)storage_->create_session("ci", "c", "1.0", 1, digest, "u2");
    // 终止/完成会话后释放名额。
    storage_->abort_session(a.id); // 释放 1 个名额
    const std::string empty_digest = fr::sha256_to_hex(fr::sha256(nullptr, 0));
    const fr::SessionInfo d = storage_->create_session("ci", "d", "1.0", 0, empty_digest, "u1");
    (void)storage_->commit_session(d.id); // 再释放 1 个名额
    (void)storage_->create_session("ci", "e", "1.0", 1, digest, "u1");
}

TEST_F(StorageSessionsTest, HighWatermarkRejectsNewSessions) {
    config_.capacity_bytes = 1024;
    config_.high_watermark_percent = 50; // 512 字节即达高水位
    config_.root = root_ / "wm";
    storage_ = fr::Storage::open(config_);

    const std::string content(600, 'w');
    const std::string digest = fr::sha256_to_hex(fr::sha256(content.data(), content.size()));
    const fr::SessionInfo s =
        storage_->create_session("ci", "big", "1.0", content.size(), digest, "u");
    (void)storage_->put_chunk(s.id, 0, content.data(), content.size());
    (void)storage_->commit_session(s.id);

    // 已用 600+32 字节 > 512：新会话被拒（FR-GC-04），已有会话/查询不受影响。
    try {
        (void)storage_->create_session("ci", "more", "1.0", 1, digest, "u");
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_LIMIT, err.code());
    }
    EXPECT_EQ(content.size(), storage_->used_bytes()); // DB 口径为数据字节
}

TEST_F(StorageSessionsTest, PutChunkValidatesOrdinalAndLength) {
    const std::string digest = fr::sha256_to_hex(fr::sha256("x", 1));
    const fr::SessionInfo s =
        storage_->create_session("ci", "app", "1.0", 4 * 1024 * 1024 + 1, digest, "u");

    const std::string block(4 * 1024 * 1024, 'a');
    EXPECT_THROW( // 序号超出期望块数（2 块）
        storage_->put_chunk(s.id, 2, block.data(), block.size()), fr::Error);
    EXPECT_THROW( // 空块
        storage_->put_chunk(s.id, 0, block.data(), 0), fr::Error);
    EXPECT_THROW( // 超过块大小
        storage_->put_chunk(s.id, 0, block.data(), 4 * 1024 * 1024 + 1), fr::Error);
    EXPECT_NO_THROW(storage_->put_chunk(s.id, 0, block.data(), block.size()));
}

TEST_F(StorageSessionsTest, PutChunkDigestVerified) {
    const std::string digest = fr::sha256_to_hex(fr::sha256("x", 1));
    const fr::SessionInfo s = storage_->create_session("ci", "app", "1.0", 4, digest, "u");
    const std::string content = "abcd";
    // 摘要按内容校验：声明期望整体摘要与块内容不符 → put 阶段即拒（FR-UP-05）。
    // 注意：期望整体摘要在此用例中等于块摘要（单块制品）。
    const fr::ChunkAccept accept = storage_->put_chunk(s.id, 0, content.data(), content.size());
    EXPECT_EQ(fr::sha256_to_hex(fr::sha256(content.data(), content.size())), accept.digest);
    EXPECT_FALSE(accept.reused);
}

TEST_F(StorageSessionsTest, PutChunkIdempotentAndConflict) {
    const std::string content = "same block content";
    const std::string overall = fr::sha256_to_hex(fr::sha256(content.data(), content.size()));
    const fr::SessionInfo s =
        storage_->create_session("ci", "app", "1.0", content.size(), overall, "u");

    const fr::ChunkAccept first = storage_->put_chunk(s.id, 0, content.data(), content.size());
    const fr::ChunkAccept second = storage_->put_chunk(s.id, 0, content.data(), content.size());
    EXPECT_FALSE(first.reused);
    EXPECT_TRUE(second.reused); // 同编号同内容幂等成功（FR-UP-06）

    const std::string other = "different block!";
    try {
        (void)storage_->put_chunk(s.id, 0, other.data(), other.size());
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_CONFLICT, err.code()); // 同编号不同内容冲突（FR-UP-06）
    }
}

TEST_F(StorageSessionsTest, CommitValidatesChunkCountAndOverallDigest) {
    // 两块制品：第一块 4 MiB + 第二块 1 MiB。
    const std::string block0(4 * 1024 * 1024, '0');
    const std::string block1(1024 * 1024, '1');
    fr::Sha256Stream overall;
    overall.update(block0.data(), block0.size());
    overall.update(block1.data(), block1.size());
    const std::string overall_hex = fr::sha256_to_hex(overall.finish());

    const fr::SessionInfo s = storage_->create_session(
        "ci", "two-blocks", "1.0", block0.size() + block1.size(), overall_hex, "u");
    (void)storage_->put_chunk(s.id, 0, block0.data(), block0.size());
    (void)storage_->put_chunk(s.id, 1, block1.data(), block1.size());

    // 缺块提交失败，会话保持 OPEN（FR-UP-08）。
    const fr::SessionInfo missing = storage_->create_session(
        "ci", "missing", "1.0", block0.size() + block1.size(), overall_hex, "u");
    (void)storage_->put_chunk(missing.id, 0, block0.data(), block0.size());
    try {
        (void)storage_->commit_session(missing.id);
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_STATE, err.code());
    }
    EXPECT_EQ(fr::SessionState::OPEN, storage_->get_session(missing.id).state);

    // 整体摘要不符 → 提交失败且不发布（FR-UP-08）。
    const fr::SessionInfo wrong =
        storage_->create_session("ci", "wrong-digest", "1.0", block0.size() + block1.size(),
                                 fr::sha256_to_hex(fr::sha256("wrong", 5)), "u");
    (void)storage_->put_chunk(wrong.id, 0, block0.data(), block0.size());
    (void)storage_->put_chunk(wrong.id, 1, block1.data(), block1.size());
    try {
        (void)storage_->commit_session(wrong.id);
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_CONFLICT, err.code());
    }
    EXPECT_EQ(fr::SessionState::OPEN, storage_->get_session(wrong.id).state);
    EXPECT_THROW(storage_->show_artifact("ci", "wrong-digest", "1.0"), fr::Error);

    // 正确提交。
    const fr::ArtifactInfo published = storage_->commit_session(s.id);
    EXPECT_EQ(block0.size() + block1.size(), published.size);
    EXPECT_EQ(2u, published.chunks.size());
    EXPECT_EQ(fr::SessionState::COMPLETED, storage_->get_session(s.id).state);
}

TEST_F(StorageSessionsTest, CommitZeroSizeArtifact) {
    const std::string empty_digest = fr::sha256_to_hex(fr::sha256(nullptr, 0));
    const fr::SessionInfo s = storage_->create_session("ci", "empty", "1.0", 0, empty_digest, "u");
    const fr::ArtifactInfo published = storage_->commit_session(s.id);
    EXPECT_EQ(0u, published.size);
    EXPECT_EQ(0u, published.chunks.size());
}

TEST_F(StorageSessionsTest, ExpiredSessionRejected) {
    const std::string digest = fr::sha256_to_hex(fr::sha256("x", 1));
    const fr::SessionInfo s = storage_->create_session("ci", "app", "1.0", 4, digest, "u");
    now_ += 24 * 3600 + 1; // 越过默认 24 小时有效期

    EXPECT_THROW(storage_->put_chunk(s.id, 0, "x", 1), fr::Error);
    EXPECT_EQ(fr::SessionState::EXPIRED, storage_->get_session(s.id).state);
    try {
        (void)storage_->commit_session(s.id);
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_EXPIRED, err.code());
    }
}

TEST_F(StorageSessionsTest, AbortIsIdempotentAndFreesChunks) {
    const std::string content = "abort me";
    const std::string overall = fr::sha256_to_hex(fr::sha256(content.data(), content.size()));
    const fr::SessionInfo s =
        storage_->create_session("ci", "app", "1.0", content.size(), overall, "u");
    (void)storage_->put_chunk(s.id, 0, content.data(), content.size());

    storage_->abort_session(s.id);
    storage_->abort_session(s.id); // 幂等
    EXPECT_EQ(fr::SessionState::ABORTED, storage_->get_session(s.id).state);

    // 终态会话分片已清理，其块不再被引用 → GC 可回收（FR-GC-02）。
    now_ += 3600 + 1;
    const fr::GcReport report = storage_->collect_garbage(false);
    EXPECT_EQ(1, report.deleted_count);
    EXPECT_EQ(static_cast<int64_t>(content.size()), report.deleted_bytes);
}

TEST_F(StorageSessionsTest, ListSessionsFilterByState) {
    const std::string digest = fr::sha256_to_hex(fr::sha256("x", 1));
    const fr::SessionInfo open1 = storage_->create_session("ci", "a", "1.0", 1, digest, "u");
    const fr::SessionInfo open2 = storage_->create_session("ci", "b", "1.0", 1, digest, "u");
    storage_->abort_session(open2.id);

    const std::vector<fr::SessionInfo> all = storage_->list_sessions(std::nullopt);
    EXPECT_EQ(2u, all.size());
    const std::vector<fr::SessionInfo> open = storage_->list_sessions(fr::SessionState::OPEN);
    ASSERT_EQ(1u, open.size());
    EXPECT_EQ(open1.id, open[0].id);
    const std::vector<fr::SessionInfo> aborted = storage_->list_sessions(fr::SessionState::ABORTED);
    ASSERT_EQ(1u, aborted.size());
    EXPECT_EQ(open2.id, aborted[0].id);
}

} // namespace
