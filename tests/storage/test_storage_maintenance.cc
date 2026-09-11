// test_storage_maintenance.cc - GC 与启动恢复测试（FR-GC、FR-STO-06、TEST-02 #6/#7）。
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"
#include "forgerelay/storage/storage.hpp"

namespace {

class StorageMaintenanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::path(::testing::TempDir()) /
                ("fr_m2_maint_" + std::to_string(++counter_));
        std::filesystem::remove_all(root_); // 跨进程重跑时清理残留
        config_.root = root_;
        config_.clock = [this]() { return now_; };
        storage_ = fr::Storage::open(config_);
    }

    fr::ArtifactInfo publish(const std::string &name, const std::string &content) {
        const std::string digest = fr::sha256_to_hex(fr::sha256(content.data(), content.size()));
        const fr::SessionInfo s =
            storage_->create_session("ci", name, "1.0", content.size(), digest, "u");
        (void)storage_->put_chunk(s.id, 0, content.data(), content.size());
        return storage_->commit_session(s.id);
    }

    static int counter_;
    std::filesystem::path root_;
    int64_t now_ = 1'700'000'000;
    fr::StorageConfig config_;
    std::unique_ptr<fr::Storage> storage_;
};

int StorageMaintenanceTest::counter_ = 0;

TEST_F(StorageMaintenanceTest, GcRespectsProtectionPeriodAndDryRun) {
    const std::string content = "garbage-collect-me";
    (void)publish("victim", content);
    (void)storage_->delete_artifact("ci", "victim", "1.0");

    auto chunk_files = [&] {
        std::error_code ec;
        uint64_t files = 0;
        for (const auto &entry :
             std::filesystem::recursive_directory_iterator(root_ / "chunks", ec)) {
            if (entry.is_regular_file()) {
                files++;
            }
        }
        return files;
    };

    // 保护期内：dry-run 与实际 GC 都不删除（FR-GC-02）。
    fr::GcReport report = storage_->collect_garbage(true);
    EXPECT_EQ(1, report.scanned);
    EXPECT_EQ(0, report.deleted_count);
    report = storage_->collect_garbage(false);
    EXPECT_EQ(1, report.scanned);
    EXPECT_EQ(0, report.deleted_count);
    EXPECT_EQ(1u, chunk_files()); // 块文件仍在

    // 越过保护期：dry-run 只统计（FR-GC-05），实际 GC 释放。
    now_ += 3600 + 1;
    const fr::GcReport plan = storage_->collect_garbage(true);
    EXPECT_EQ(1, plan.scanned);
    EXPECT_EQ(1, plan.deleted_count);
    EXPECT_EQ(static_cast<int64_t>(content.size()), plan.deleted_bytes);
    EXPECT_EQ(1u, chunk_files()); // dry-run 未执行删除

    const fr::GcReport done = storage_->collect_garbage(false);
    EXPECT_EQ(1, done.deleted_count);
    EXPECT_EQ(0u, chunk_files());
    EXPECT_EQ(0u, storage_->used_bytes());
}

TEST_F(StorageMaintenanceTest, GcKeepsReferencedAndSharedChunks) {
    // 两个制品共享同内容块；删除其一后块仍被另一制品引用。
    const std::string content = "shared between artifacts";
    (void)publish("keep-a", content);
    (void)publish("keep-b", content);
    (void)storage_->delete_artifact("ci", "keep-a", "1.0");

    now_ += 3600 + 1;
    const fr::GcReport report = storage_->collect_garbage(false);
    EXPECT_EQ(0, report.deleted_count); // 块仍被 keep-b 引用

    std::string roundtrip;
    storage_->read_artifact("ci", "keep-b", "1.0", 0, content.size(),
                            [&](const void *data, size_t len) {
                                roundtrip.append(static_cast<const char *>(data), len);
                            });
    EXPECT_EQ(content, roundtrip);
}

TEST_F(StorageMaintenanceTest, GcKeepsChunksReferencedByActiveSessions) {
    const std::string content = "in-flight upload content";
    const std::string digest = fr::sha256_to_hex(fr::sha256(content.data(), content.size()));
    // 制品 A 引用块 X。
    (void)publish("ref", content);
    // 会话（OPEN）也登记块 X。
    const fr::SessionInfo s =
        storage_->create_session("ci", "inflight", "1.0", content.size(), digest, "u");
    (void)storage_->put_chunk(s.id, 0, content.data(), content.size());
    // 删除制品 A → 块仍被活动会话引用。
    (void)storage_->delete_artifact("ci", "ref", "1.0");
    now_ += 3600 + 1;
    const fr::GcReport report = storage_->collect_garbage(false);
    EXPECT_EQ(0, report.deleted_count);

    // 会话提交后块被制品引用，同样保留。
    (void)storage_->commit_session(s.id);
    (void)storage_->delete_artifact("ci", "inflight", "1.0");
    now_ += 3600 + 1;
    EXPECT_EQ(1, storage_->collect_garbage(false).deleted_count); // 现在无引用可删
}

TEST_F(StorageMaintenanceTest, RecoveryRollsBackCommittingAndKeepsOpenSessions) {
    const std::string content = "resume-after-crash";
    const std::string digest = fr::sha256_to_hex(fr::sha256(content.data(), content.size()));
    const fr::SessionInfo s =
        storage_->create_session("ci", "resume", "1.0", content.size(), digest, "u");
    (void)storage_->put_chunk(s.id, 0, content.data(), content.size());

    // 模拟进程崩溃：直接销毁并重新打开（FR-STO-06 / TEST-02 #6）。
    storage_.reset();
    config_.root = root_;
    config_.clock = [this]() { return now_; };
    storage_ = fr::Storage::open(config_);

    // 会话与分片保留，可继续上传并成功提交。
    const fr::SessionInfo resumed = storage_->get_session(s.id);
    EXPECT_EQ(fr::SessionState::OPEN, resumed.state);
    (void)storage_->commit_session(s.id);
    std::string roundtrip;
    storage_->read_artifact("ci", "resume", "1.0", 0, content.size(),
                            [&](const void *data, size_t len) {
                                roundtrip.append(static_cast<const char *>(data), len);
                            });
    EXPECT_EQ(content, roundtrip);
}

TEST_F(StorageMaintenanceTest, RecoveryExpiresStaleSessionsAndCleansTemp) {
    const std::string digest = fr::sha256_to_hex(fr::sha256("x", 1));
    const fr::SessionInfo s = storage_->create_session("ci", "stale", "1.0", 4, digest, "u");
    now_ += 24 * 3600 + 5;

    // 重新打开：open 内部即执行启动恢复（FR-STO-06）。
    storage_.reset();
    config_.root = root_;
    config_.clock = [this]() { return now_; };
    storage_ = fr::Storage::open(config_);

    // 过期会话被置为 EXPIRED（run_maintenance 幂等，重复执行计数为 0）。
    EXPECT_EQ(fr::SessionState::EXPIRED, storage_->get_session(s.id).state);
    EXPECT_EQ(0, storage_->run_maintenance().sessions_expired);

    // 遗留临时文件在重新打开时被清理。
    const std::filesystem::path leftover = root_ / "tmp" / "frtmp-crash.part";
    {
        std::ofstream junk(leftover, std::ios::binary);
        junk.write("x", 1);
    }
    EXPECT_TRUE(std::filesystem::exists(leftover));
    storage_.reset();
    config_.root = root_;
    config_.clock = [this]() { return now_; };
    storage_ = fr::Storage::open(config_);
    EXPECT_FALSE(std::filesystem::exists(leftover));
}

} // namespace
