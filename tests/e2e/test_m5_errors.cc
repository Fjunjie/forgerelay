// test_m5_errors.cc - 错误场景测试（§13 错误场景：协议破坏、未知消息、
// 坏参数、不存在资源；服务端必须存活并可继续服务）。
#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "forgerelay/client.hpp"
#include "forgerelay/log.hpp"
#include "forgerelay/service.hpp"
#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"
#include "forgerelay/storage/storage.hpp"
#include "forgerelay/transport.hpp"

namespace {

class M5ErrorTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root_ =
            std::filesystem::path(::testing::TempDir()) / ("fr_m5e_" + std::to_string(++counter_));
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
        auth_->user_add("publisher", fr::Role::Publisher);
        auth_->user_add("admin", fr::Role::Admin);
        publisher_token_ = auth_->token_create("publisher", 0);
        admin_token_ = auth_->token_create("admin", 0);

        dispatcher_ = std::make_unique<fr::Dispatcher>(*storage_, *auth_, settings_, log_, hub_);
        server_ = fr::Server::start(settings_, *storage_, *dispatcher_, log_, hub_.get());
    }

    void TearDown() override
    {
        server_->stop();
        auth_.reset();
        storage_.reset();
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::unique_ptr<fr::Client> make_client(const std::string &token)
    {
        fr::ClientOptions options;
        options.host = "127.0.0.1";
        options.port = server_->port();
        options.timeout_seconds = 5;
        options.token = token;
        auto client = std::make_unique<fr::Client>(options);
        client->connect();
        return client;
    }

    /** 发送一个原始帧（绕过消息编码，直接控制字节）。 */
    void send_raw(fr::Client &client, uint8_t type, const void *payload, size_t len)
    {
        client.send_raw_frame(type, payload, len);
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

int M5ErrorTest::counter_ = 0;

/// 协议破坏（垃圾字节/坏 magic/坏 CRC）：连接被关闭，服务端存活。
TEST_F(M5ErrorTest, ProtocolViolationClosesConnectionServerSurvives)
{
    auto bad = make_client(publisher_token_);
    bad->send_garbage("GARBAGE-NOT-A-FRAME");
    // 服务端应关闭连接：后续收发必然失败。
    EXPECT_THROW((void)bad->status(), fr::Error);

    // 服务端存活：新连接正常服务。
    auto good = make_client(admin_token_);
    EXPECT_NO_THROW((void)good->status());
}

/// 不存在的资源：NOTFOUND（FR-E 语义，非崩溃）。
TEST_F(M5ErrorTest, NotFoundResources)
{
    auto admin = make_client(admin_token_);
    EXPECT_THROW(admin->show_artifact("nope", "nope", "nope"), fr::Error);
    EXPECT_THROW((void)admin->delete_artifact("nope", "nope", "nope"), fr::Error);
    EXPECT_THROW((void)admin->session_abort("no-such-session"), fr::Error);
    try {
        (void)admin->token_revoke(999999);
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_NOTFOUND, err.code());
    }
}

/// 非法标识符：创建会话时拒绝（SEC-05）。
TEST_F(M5ErrorTest, InvalidIdentifiersRejected)
{
    auto publisher = make_client(publisher_token_);
    const std::string digest = fr::sha256_to_hex(fr::sha256("x", 1));
    EXPECT_THROW(publisher->create_upload("bad ns", "app", "1.0", 1, digest), fr::Error);
    EXPECT_THROW(publisher->create_upload("ci", "../evil", "1.0", 1, digest), fr::Error);
    EXPECT_THROW(publisher->create_upload("ci", "app", "bad version!", 1, digest), fr::Error);
    EXPECT_THROW(publisher->create_upload("ci", "app", "1.0", 1, std::string(64, 'z')), fr::Error);
    // 服务端存活。
    auto admin = make_client(admin_token_);
    EXPECT_NO_THROW((void)admin->status());
}

/// 无效令牌：认证失败，错误码为 UNAUTHENTICATED（FR-AUTH）。
TEST_F(M5ErrorTest, InvalidTokenRejectedWithStableCode)
{
    try {
        auto bad = make_client("fr_" + std::string(64, 'e'));
        (void)bad->status();
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_UNAUTHENTICATED, err.code());
    }
    // 服务端存活。
    auto good = make_client(admin_token_);
    EXPECT_NO_THROW((void)good->status());
}

/// 坏参数：非法块序号/长度被拒绝（FR-UP-05/边界）。
TEST_F(M5ErrorTest, BadChunkParametersRejected)
{
    auto publisher = make_client(publisher_token_);
    const std::string content(1024 * 1024, 'p');
    fr::Sha256Stream overall;
    overall.update(content.data(), content.size());
    const std::string digest = fr::sha256_to_hex(overall.finish());
    const fr::msg::CreateUploadOk session =
        publisher->create_upload("ci", "badparam", "1.0", content.size(), digest);

    EXPECT_THROW(
        publisher->put_chunk(session.session_id, 5, content.data(), content.size()),
        fr::Error); // 序号越界
    /* 唯一（末）块的 put 允许部分长度（提交时校验总长，FR-UP-08）。 */
    EXPECT_NO_THROW(publisher->put_chunk(session.session_id, 0, content.data(), 1));
    EXPECT_THROW(
        publisher->put_chunk(session.session_id, 0, content.data(), content.size()),
        fr::Error); // 同编号不同内容 → FR_E_CONFLICT（FR-UP-06）
    EXPECT_THROW((void)publisher->commit_upload(session.session_id), fr::Error); // 总长不符
    /* 会话已回 OPEN（FR-UP-08 失败回滚）：COMMITTING 卡死时 put 会被拒绝，
     * 故重复上传同内容块（幂等，FR-UP-06）成功即为回滚到 OPEN 的证据。 */
    EXPECT_NO_THROW(publisher->put_chunk(session.session_id, 0, content.data(), 1));
}

} // namespace
