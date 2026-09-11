// test_m5_concurrency.cc - 并发测试（§13 并发测试：重复提交、断开、超时、关闭和
// 清理；可重复执行且具有超时保护）。
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "forgerelay/client.hpp"
#include "forgerelay/log.hpp"
#include "forgerelay/service.hpp"
#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"
#include "forgerelay/storage/storage.hpp"
#include "forgerelay/transport.hpp"

namespace {

class M5ConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ =
            std::filesystem::path(::testing::TempDir()) / ("fr_m5c_" + std::to_string(++counter_));
        std::filesystem::remove_all(root_);

        settings_.listen = "127.0.0.1:0";
        settings_.max_connections = 64;
        settings_.worker_threads = 4;
        settings_.request_timeout_seconds = 30; // 并发压测下放宽空闲判定
        settings_.storage.root = root_;
        settings_.storage.chunk_size = 1024 * 1024;
        settings_.storage.clock = [this]() { return now_; };

        storage_ = fr::Storage::open(settings_.storage);
        auth_ = std::make_unique<fr::AuthRegistry>((root_ / "metadata.db").string());
        hub_ = std::make_shared<fr::StatusHub>();
        auth_->user_add("publisher", fr::Role::Publisher);
        auth_->user_add("admin", fr::Role::Admin);
        publisher_token_ = auth_->token_create("publisher", 0);
        admin_token_ = auth_->token_create("admin", 0);

        dispatcher_ = std::make_unique<fr::Dispatcher>(*storage_, *auth_, settings_, log_, hub_);
        server_ = fr::Server::start(settings_, *storage_, *dispatcher_, log_, hub_.get());
    }

    void TearDown() override {
        server_->stop();
        auth_.reset();
        storage_.reset();
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::unique_ptr<fr::Client> make_client(const std::string &token) {
        fr::ClientOptions options;
        options.host = "127.0.0.1";
        options.port = server_->port();
        options.timeout_seconds = 20;
        options.token = token;
        auto client = std::make_unique<fr::Client>(options);
        client->connect();
        return client;
    }

    /** 单客户端完整上传一个制品；name 含线程标识。 */
    void upload_artifact(fr::Client &client, const std::string &name, char fill,
                         uint64_t extra = 0) {
        const std::string block(1024 * 1024, fill);
        const std::string tail(extra, fill);
        fr::Sha256Stream overall;
        overall.update(block.data(), block.size());
        if (extra != 0) {
            overall.update(tail.data(), tail.size());
        }
        const std::string digest = fr::sha256_to_hex(overall.finish());
        const uint64_t size = block.size() + extra;

        const fr::msg::CreateUploadOk session =
            client.create_upload("ci", name, "1.0", size, digest);
        (void)client.put_chunk(session.session_id, 0, block.data(), block.size());
        if (extra != 0) {
            (void)client.put_chunk(session.session_id, 1, tail.data(), tail.size());
        }
        (void)client.commit_upload(session.session_id);
    }

    static int counter_;
    std::filesystem::path root_;
    int64_t now_ = 1'700'000'000;
    fr::ServerSettings settings_;
    fr::Logger log_;
    std::unique_ptr<fr::Storage> storage_;
    std::unique_ptr<fr::AuthRegistry> auth_;
    std::shared_ptr<fr::StatusHub> hub_;
    std::unique_ptr<fr::Dispatcher> dispatcher_;
    std::unique_ptr<fr::Server> server_;
    std::string publisher_token_;
    std::string admin_token_;
};

int M5ConcurrencyTest::counter_ = 0;

/// 4 线程并发上传 8 个制品：全部成功且内容互不串扰（§13 并发）。
TEST_F(M5ConcurrencyTest, ParallelUploadsDistinctArtifacts) {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 2;
    std::atomic<int> failures{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&, t] {
            try {
                auto client = make_client(publisher_token_);
                for (int i = 0; i < kPerThread; i++) {
                    const std::string name = "app-t" + std::to_string(t) + "-i" + std::to_string(i);
                    const std::string block(1024 * 1024, static_cast<char>('A' + t));
                    fr::Sha256Stream overall;
                    overall.update(block.data(), block.size());
                    const std::string digest = fr::sha256_to_hex(overall.finish());
                    const fr::msg::CreateUploadOk session =
                        client->create_upload("ci", name, "1.0", block.size(), digest);
                    (void)client->put_chunk(session.session_id, 0, block.data(), block.size());
                    (void)client->commit_upload(session.session_id);
                }
            } catch (const std::exception &err) {
                ADD_FAILURE() << "thread " << t << ": " << err.what();
                failures.fetch_add(1);
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    EXPECT_EQ(0, failures.load());

    auto admin = make_client(admin_token_);
    const std::vector<fr::msg::ArtifactSummary> items =
        admin->list_artifacts(std::nullopt, std::nullopt, 100, 0);
    EXPECT_EQ(kThreads * kPerThread, static_cast<int>(items.size()));
}

/// 多线程并发下载同一制品：内容一致、总量正确。
TEST_F(M5ConcurrencyTest, ParallelDownloadsSameArtifact) {
    auto publisher = make_client(publisher_token_);
    const std::string block(3 * 1024 * 1024, 'Z');
    {
        fr::Sha256Stream overall;
        overall.update(block.data(), block.size());
        const std::string digest = fr::sha256_to_hex(overall.finish());
        const fr::msg::CreateUploadOk session =
            publisher->create_upload("ci", "shared", "1.0", block.size(), digest);
        for (uint64_t ordinal = 0; ordinal < 3; ordinal++) {
            (void)publisher->put_chunk(session.session_id, ordinal,
                                       block.data() + ordinal * 1024 * 1024, 1024 * 1024);
        }
        (void)publisher->commit_upload(session.session_id);
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 3; t++) {
        threads.emplace_back([&] {
            try {
                auto reader = make_client(publisher_token_);
                std::string got;
                reader->download("ci", "shared", "1.0", 0, UINT64_MAX,
                                 [&](const void *data, size_t len) {
                                     got.append(static_cast<const char *>(data), len);
                                 });
                if (got != block) {
                    ADD_FAILURE() << "downloaded content mismatch (thread " << t << ")";
                    failures.fetch_add(1);
                }
            } catch (const std::exception &err) {
                ADD_FAILURE() << "thread " << t << ": " << err.what();
                failures.fetch_add(1);
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    EXPECT_EQ(0, failures.load());
}

/// 客户端异常断开（无 CLOSE 直接析构）：服务端清理续传状态并继续服务（审计 #3）。
TEST_F(M5ConcurrencyTest, AbruptDisconnectCleanedAndServerSurvives) {
    /* 建立连接、发 HELLO，然后硬断开（析构不发 CLOSE——连接中断场景）。 */
    {
        fr::ClientOptions options;
        options.host = "127.0.0.1";
        options.port = server_->port();
        options.timeout_seconds = 5;
        fr::Client abrupt(options);
        abrupt.connect();
        // 作用域结束：析构路径走“连接已断”语义（直接关闭 fd）
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 服务端存活且可正常服务。
    auto publisher = make_client(publisher_token_);
    EXPECT_NO_THROW(upload_artifact(*publisher, "after-drop", 'D'));
    EXPECT_EQ(1u, hub_->connections.load()); // 断开连接已被清理
}

/// 重复提交同一块：幂等成功（FR-UP-06）。
TEST_F(M5ConcurrencyTest, DuplicateChunkSubmissionIdempotent) {
    auto publisher = make_client(publisher_token_);
    const std::string content(1024 * 1024, 'R');
    fr::Sha256Stream overall;
    overall.update(content.data(), content.size());
    const std::string digest = fr::sha256_to_hex(overall.finish());
    const fr::msg::CreateUploadOk session =
        publisher->create_upload("ci", "dup", "1.0", content.size(), digest);

    const fr::msg::PutChunkOk first =
        publisher->put_chunk(session.session_id, 0, content.data(), content.size());
    EXPECT_FALSE(first.reused);
    const fr::msg::PutChunkOk second =
        publisher->put_chunk(session.session_id, 0, content.data(), content.size());
    EXPECT_TRUE(second.reused); // FR-UP-06 幂等
    (void)publisher->commit_upload(session.session_id);
}

} // namespace
