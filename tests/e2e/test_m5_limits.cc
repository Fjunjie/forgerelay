// test_m5_limits.cc - 资源限制测试（§7.3：连接上限拒绝、空闲扫描断开）。
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "forgerelay/client.hpp"
#include "forgerelay/log.hpp"
#include "forgerelay/service.hpp"
#include "forgerelay/storage/error.hpp"
#include "forgerelay/storage/storage.hpp"
#include "forgerelay/transport.hpp"

namespace {

class M5LimitsTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ =
            std::filesystem::path(::testing::TempDir()) / ("fr_m5l_" + std::to_string(++counter_));
        std::filesystem::remove_all(root_);

        settings_.listen = "127.0.0.1:0";
        settings_.max_connections = 64;
        settings_.worker_threads = 2;
        settings_.request_timeout_seconds = 30;
        settings_.storage.root = root_;
        settings_.storage.chunk_size = 1024 * 1024;
        settings_.storage.clock = [this]() { return now_; };

        storage_ = fr::Storage::open(settings_.storage);
        auth_ = std::make_unique<fr::AuthRegistry>((root_ / "metadata.db").string());
        hub_ = std::make_shared<fr::StatusHub>();
        auth_->user_add("admin", fr::Role::Admin);
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

    std::unique_ptr<fr::Client> make_client(const std::string &token, int timeout = 5) {
        fr::ClientOptions options;
        options.host = "127.0.0.1";
        options.port = server_->port();
        options.timeout_seconds = timeout;
        options.token = token;
        auto client = std::make_unique<fr::Client>(options);
        client->connect();
        return client;
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
    std::string admin_token_;
};

int M5LimitsTest::counter_ = 0;

/// max_connections 拒绝：连接占满后，新连接的 HELLO 得不到响应/被关闭，
/// 已连接客户端不受影响（§7.3、FR-GC-04 对应的连接面限制）。
TEST_F(M5LimitsTest, MaxConnectionsEnforced) {
    settings_.max_connections = 4;
    settings_.request_timeout_seconds = 2;
    settings_.storage.root = root_ / "small";
    settings_.storage.clock = [this]() { return now_; };
    server_->stop();
    storage_ = fr::Storage::open(settings_.storage);
    auth_ = std::make_unique<fr::AuthRegistry>((root_ / "metadata.db").string());
    dispatcher_ = std::make_unique<fr::Dispatcher>(*storage_, *auth_, settings_, log_, hub_);
    server_ = fr::Server::start(settings_, *storage_, *dispatcher_, log_, hub_.get());

    std::vector<std::unique_ptr<fr::Client>> holders;
    for (int i = 0; i < 4; i++) {
        holders.push_back(make_client(admin_token_));
    }
    EXPECT_EQ(4u, hub_->connections.load());

    // 第 5 个连接被拒：HELLO 无响应（客户端在超时后报连接错误）。
    fr::ClientOptions options;
    options.host = "127.0.0.1";
    options.port = server_->port();
    options.timeout_seconds = 1; // 短超时加速测试
    options.token = admin_token_;
    fr::Client rejected(options);
    EXPECT_ANY_THROW(rejected.connect());

    // 已连接客户端不受影响（FR-GC-04 对应语义：限制只影响新连接）。
    EXPECT_NO_THROW((void)holders[0]->status());
}

/// 空闲扫描：超过 request_timeout 的连接被服务端主动关闭（§7.1 超时）。
TEST_F(M5LimitsTest, IdleConnectionSwept) {
    settings_.request_timeout_seconds = 2;
    settings_.storage.root = root_ / "idle";
    settings_.storage.clock = [this]() { return now_; };
    server_->stop();
    storage_ = fr::Storage::open(settings_.storage);
    dispatcher_ = std::make_unique<fr::Dispatcher>(*storage_, *auth_, settings_, log_, hub_);
    server_ = fr::Server::start(settings_, *storage_, *dispatcher_, log_, hub_.get());

    auto client = make_client(admin_token_);
    EXPECT_EQ(1u, hub_->connections.load());

    /* 空闲超过 2 秒（扫描粒度为整秒，实际在 ≥3s 时关闭）：连接被服务端清除。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    EXPECT_EQ(0u, hub_->connections.load());
    EXPECT_THROW((void)client->status(), fr::Error);

    /* 服务端存活。 */
    auto fresh = make_client(admin_token_);
    EXPECT_NO_THROW((void)fresh->status());
}

} // namespace
