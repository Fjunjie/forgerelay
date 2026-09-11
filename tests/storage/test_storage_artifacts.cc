// test_storage_artifacts.cc - 制品发布/查询/读取/删除测试（FR-ART、FR-DL、FR-GC-01）。
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"
#include "forgerelay/storage/storage.hpp"

namespace {

class StorageArtifactsTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::path(::testing::TempDir()) /
                ("fr_m2_art_" + std::to_string(++counter_));
        std::filesystem::remove_all(root_); // 跨进程重跑时清理残留
        config_.root = root_;
        config_.chunk_size = 1024 * 1024; // 1 MiB 块，便于构造多块制品
        storage_ = fr::Storage::open(config_);
    }

    // 建立一个 N 块制品，块 i 内容为 repeat('a'+i)。
    fr::ArtifactInfo publish_blocks(const std::string &ns, const std::string &name,
                                    const std::string &version,
                                    const std::vector<std::string> &blocks) {
        fr::Sha256Stream overall;
        for (const std::string &block : blocks) {
            overall.update(block.data(), block.size());
        }
        const std::string digest = fr::sha256_to_hex(overall.finish());
        const uint64_t size = blocks.empty() ? 0 : [blocks] {
            uint64_t total = 0;
            for (const std::string &b : blocks) {
                total += b.size();
            }
            return total;
        }();

        const fr::SessionInfo s = storage_->create_session(ns, name, version, size, digest, "ci");
        for (size_t i = 0; i < blocks.size(); i++) {
            (void)storage_->put_chunk(s.id, i, blocks[i].data(), blocks[i].size());
        }
        return storage_->commit_session(s.id);
    }

    static int counter_;
    std::filesystem::path root_;
    fr::StorageConfig config_;
    std::unique_ptr<fr::Storage> storage_;
};

int StorageArtifactsTest::counter_ = 0;

TEST_F(StorageArtifactsTest, PublishShowManifestMatches) {
    // 两块制品：块 0 满 1 MiB，块 1 为剩余 100 字节。
    const std::string block0(1024 * 1024, 'a');
    const std::string block1(100, 'b');
    const fr::ArtifactInfo published = publish_blocks("team", "app", "1.0.0", {block0, block1});

    EXPECT_EQ(block0.size() + block1.size(), published.size);
    EXPECT_EQ("ci", published.creator);
    ASSERT_EQ(2u, published.chunks.size());
    EXPECT_EQ(0, published.chunks[0].ordinal);
    EXPECT_EQ(0u, published.chunks[0].offset);
    EXPECT_EQ(block0.size(), published.chunks[0].length);
    EXPECT_EQ(1, published.chunks[1].ordinal);
    EXPECT_EQ(block0.size(), published.chunks[1].offset);
    EXPECT_EQ(block1.size(), published.chunks[1].length);

    const fr::ArtifactInfo shown = storage_->show_artifact("team", "app", "1.0.0");
    EXPECT_EQ(published.size, shown.size);
    EXPECT_EQ(published.digest, shown.digest);
    ASSERT_EQ(2u, shown.chunks.size());
    EXPECT_EQ(published.chunks[1].digest, shown.chunks[1].digest);
    EXPECT_THROW(storage_->show_artifact("team", "app", "9.9.9"), fr::Error);
}

TEST_F(StorageArtifactsTest, RepublishRejectedByDefault) {
    const std::string content = "do not overwrite me";
    (void)publish_blocks("team", "once", "1.0", {content});
    try {
        (void)publish_blocks("team", "once", "1.0", {content});
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_EXISTS, err.code()); // FR-ART-05 默认不可覆盖
    }
    // 原制品完好。
    const fr::ArtifactInfo shown = storage_->show_artifact("team", "once", "1.0");
    EXPECT_EQ(content.size(), shown.size);
}

TEST_F(StorageArtifactsTest, ReadFullAndRanges) {
    const std::string block0(1024 * 1024, 'x');
    const std::string block1(1000, 'y');
    (void)publish_blocks("team", "readable", "1.0", {block0, block1});
    const std::string full = block0 + block1;

    auto collect = [&](uint64_t offset, uint64_t length) {
        std::string out;
        storage_->read_artifact("team", "readable", "1.0", offset, length,
                                [&](const void *data, size_t len) {
                                    out.append(static_cast<const char *>(data), len);
                                });
        return out;
    };

    EXPECT_EQ(full, collect(0, full.size()));                      // 全量
    EXPECT_EQ(std::string_view(full).substr(0, 5), collect(0, 5)); // 首
    EXPECT_EQ(std::string_view(full).substr(full.size() - 3),
              collect(full.size() - 3, 100));                              // 尾（越界截断）
    EXPECT_EQ(std::string_view(full).substr(1023, 10), collect(1023, 10)); // 跨块
    EXPECT_TRUE(collect(full.size(), 0).empty());                          // 尾后零长合法
    EXPECT_TRUE(collect(full.size(), 50).empty());                         // 尾后读截断为空
    EXPECT_THROW(collect(full.size() + 1, 0), fr::Error);                  // 起点超出（FR-DL-04）

    std::string streamed;
    storage_->read_artifact("team", "readable", "1.0", 0, full.size(),
                            [&](const void *data, size_t len) {
                                streamed.append(static_cast<const char *>(data), len);
                            });
    EXPECT_EQ(full.size(), streamed.size());
}

TEST_F(StorageArtifactsTest, ListArtifactsPaginationAndFilters) {
    const std::string content = "list-item";
    for (int i = 0; i < 5; i++) {
        (void)publish_blocks("ns-a", "app", "1.0." + std::to_string(i), {content});
    }
    for (int i = 0; i < 3; i++) {
        (void)publish_blocks("ns-b", "lib", "2.0." + std::to_string(i), {content});
    }

    const std::vector<fr::ArtifactInfo> all =
        storage_->list_artifacts(std::nullopt, std::nullopt, 100, 0);
    EXPECT_EQ(8u, all.size());
    // created_at 相同时按 id 降序 → 最后发布的在前。
    EXPECT_EQ("ns-b", all[0].ns);

    const std::vector<fr::ArtifactInfo> page =
        storage_->list_artifacts(std::nullopt, std::nullopt, 3, 6);
    EXPECT_EQ(2u, page.size());

    const std::vector<fr::ArtifactInfo> by_ns =
        storage_->list_artifacts(std::optional<std::string>("ns-a"), std::nullopt, 100, 0);
    EXPECT_EQ(5u, by_ns.size());

    const std::vector<fr::ArtifactInfo> by_ns_name = storage_->list_artifacts(
        std::optional<std::string>("ns-b"), std::optional<std::string>("lib"), 100, 0);
    EXPECT_EQ(3u, by_ns_name.size());
    EXPECT_EQ("2.0.2", by_ns_name[0].version);
}

TEST_F(StorageArtifactsTest, DeleteRemovesMetadataAndMarksChunks) {
    const std::string content = "delete-target-content";
    (void)publish_blocks("team", "del", "1.0", {content});

    const int64_t marked = storage_->delete_artifact("team", "del", "1.0");
    EXPECT_EQ(1, marked); // FR-GC-01：先删元数据，再标记无引用块
    EXPECT_THROW(storage_->show_artifact("team", "del", "1.0"), fr::Error);
    EXPECT_THROW(storage_->delete_artifact("team", "del", "1.0"), fr::Error); // 二次删除 NOTFOUND
}

} // namespace
