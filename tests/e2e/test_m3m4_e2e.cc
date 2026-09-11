// test_e2e_m3m4.cc - M3/M4 端到端测试（TEST-02 #8 权限正反向、令牌生命周期、
// 速率限制、审计；回环 TCP + 真实 Server/Client 栈）。
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "forgerelay/client.hpp"
#include "forgerelay/log.hpp"
#include "forgerelay/service.hpp"
#include "forgerelay/storage/db.hpp"
#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"
#include "forgerelay/storage/storage.hpp"
#include "forgerelay/transport.hpp"

namespace {

class M3M4E2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ =
            std::filesystem::path(::testing::TempDir()) / ("fr_m3m4_" + std::to_string(++counter_));
        std::filesystem::remove_all(root_);

        settings_.listen = "127.0.0.1:0"; // 自动端口
        settings_.max_connections = 64;   // 限流测试需要 21 个连接并存
        settings_.worker_threads = 2;
        settings_.request_timeout_seconds = 10;
        settings_.storage.root = root_;
        settings_.storage.chunk_size = 1024 * 1024; // 1 MiB，便于多块场景
        settings_.storage.clock = [this]() { return now_; };

        storage_ = fr::Storage::open(settings_.storage);
        auth_ = std::make_unique<fr::AuthRegistry>((root_ / "metadata.db").string());
        hub_ = std::make_shared<fr::StatusHub>();

        /* 三个角色的用户与令牌（权限矩阵数据，TEST-02 #8）。 */
        auth_->user_add("admin", fr::Role::Admin);
        auth_->user_add("publisher", fr::Role::Publisher);
        auth_->user_add("reader", fr::Role::Reader);
        admin_token_ = auth_->token_create("admin", 0);
        publisher_token_ = auth_->token_create("publisher", 0);
        reader_token_ = auth_->token_create("reader", 0);

        dispatcher_ = std::make_unique<fr::Dispatcher>(*storage_, *auth_, settings_, log_, hub_);
        server_ = fr::Server::start(settings_, *storage_, *dispatcher_, log_, hub_.get());
    }

    void TearDown() override {
        server_->stop();
        auth_.reset(); // 先关闭 SQLite 连接，避免 Windows 文件锁阻塞 remove_all
        storage_.reset();
        std::error_code ec;
        std::filesystem::remove_all(root_, ec); // 清理失败不影响结果（临时目录）
    }

    std::unique_ptr<fr::Client> make_client(const std::string &token) {
        fr::ClientOptions options;
        options.host = "127.0.0.1";
        options.port = server_->port();
        options.timeout_seconds = 5;
        options.token = token;
        auto client = std::make_unique<fr::Client>(options);
        client->connect(); // HELLO + AUTH（token 为空时跳过 AUTH）
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
    std::string publisher_token_;
    std::string reader_token_;
};

int M3M4E2ETest::counter_ = 0;

/// HELLO/STATUS：连接层与状态查询（FR-ADM-01）。
TEST_F(M3M4E2ETest, HelloPingStatus) {
    auto admin = make_client(admin_token_);
    const fr::msg::StatusOk status = admin->status();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(1u, hub_->connections.load()); // 服务端连接计数
    EXPECT_EQ(1u, status.connections);       // STATUS 视角一致
    EXPECT_NE(std::string::npos, status.version.find("forgerelayd"));
    EXPECT_EQ(0u, status.artifacts);
    EXPECT_EQ(settings_.storage.capacity_bytes, status.capacity_bytes);
    EXPECT_EQ(static_cast<uint8_t>(settings_.storage.high_watermark_percent),
              status.high_watermark_percent);
}

/// 全链路：上传（3 块）→ 提交 → 查询 → 全量/范围下载（FR-UP、FR-DL、FR-STO）。
TEST_F(M3M4E2ETest, UploadCommitQueryDownloadCycle) {
    auto publisher = make_client(publisher_token_);

    const std::string block0(1024 * 1024, 'a');
    const std::string block1(1024 * 1024, 'b');
    const std::string block2(5, 'c');
    fr::Sha256Stream overall;
    overall.update(block0.data(), block0.size());
    overall.update(block1.data(), block1.size());
    overall.update(block2.data(), block2.size());
    const std::string digest = fr::sha256_to_hex(overall.finish());
    const uint64_t size = block0.size() + block1.size() + block2.size();

    const fr::msg::CreateUploadOk session =
        publisher->create_upload("team", "app", "1.0.0", size, digest);
    EXPECT_EQ(1024u * 1024, session.chunk_size);
    (void)publisher->put_chunk(session.session_id, 0, block0.data(), block0.size());
    (void)publisher->put_chunk(session.session_id, 1, block1.data(), block1.size());
    (void)publisher->put_chunk(session.session_id, 2, block2.data(), block2.size());
    fprintf(stderr, "[M] uploaded\n");

    /* QUERY_UPLOAD：位图反映全部块已就位。 */
    const fr::msg::QueryUploadOk progress = publisher->query_upload(session.session_id);
    EXPECT_EQ(3u, progress.chunk_count);
    ASSERT_EQ(1u, progress.present_bitmap.size());
    EXPECT_EQ(0x07, progress.present_bitmap[0] & 0x07);

    const fr::msg::CommitUploadOk published = publisher->commit_upload(session.session_id);
    EXPECT_EQ(size, published.size);
    fprintf(stderr, "[M] committed\n");

    /* SHOW + 全量下载（跨 DATA 分片流式）+ 范围下载。 */
    const fr::msg::ShowArtifactOk shown = publisher->show_artifact("team", "app", "1.0.0");
    EXPECT_EQ(3u, shown.info.chunks.size());
    EXPECT_EQ("publisher", shown.info.creator);

    std::string downloaded;
    fprintf(stderr, "[M] downloading\n");
    uint64_t got = 0;
    try {
        got = publisher->download("team", "app", "1.0.0", 0, UINT64_MAX,
                                  [&](const void *data, size_t len) {
                                      downloaded.append(static_cast<const char *>(data), len);
                                      fprintf(stderr, "[M] sink +%zu\n", len);
                                  });
    } catch (const fr::Error &err) {
        fprintf(stderr, "[M] download threw: %s (got=%llu str=%zu)\n", err.what(),
                static_cast<unsigned long long>(got), downloaded.size());
        throw;
    }
    fprintf(stderr, "[M] downloaded %llu\n", static_cast<unsigned long long>(got));
    EXPECT_EQ(size, got);
    EXPECT_EQ(size, downloaded.size());
    EXPECT_EQ(block0, downloaded.substr(0, block0.size()));
    EXPECT_EQ(block2, downloaded.substr(block0.size() + block1.size()));

    std::string range;
    publisher->download(
        "team", "app", "1.0.0", block0.size() + 2, 3,
        [&](const void *data, size_t len) { range.append(static_cast<const char *>(data), len); });
    EXPECT_EQ(std::string("bbb"), range);

    /* LIST 含该制品。 */
    const std::vector<fr::msg::ArtifactSummary> items =
        publisher->list_artifacts(std::nullopt, std::nullopt, 10, 0);
    ASSERT_EQ(1u, items.size());
    EXPECT_EQ("team", items[0].ns);

    /* 会话完成后 SESSION 状态为 COMPLETED（Admin 视角）。 */
    auto admin = make_client(admin_token_);
    const std::vector<fr::msg::SessionItem> sessions = admin->session_list();
    ASSERT_EQ(1u, sessions.size());
    EXPECT_EQ(static_cast<uint8_t>(fr::SessionState::COMPLETED),
              static_cast<uint8_t>(sessions[0].state));
}

/// 权限矩阵正反向验证（TEST-02 #8、FR-AUTH-04）。
TEST_F(M3M4E2ETest, PermissionMatrixPositiveAndNegative) {
    auto admin = make_client(admin_token_);
    auto publisher = make_client(publisher_token_);
    auto reader = make_client(reader_token_);
    auto anonymous = make_client(""); // 未认证

    /* Reader：只读操作允许。 */
    EXPECT_NO_THROW((void)reader->list_artifacts(std::nullopt, std::nullopt, 10, 0));
    EXPECT_NO_THROW((void)reader->status());

    /* Reader：写操作全部拒绝（FR_E_FORBIDDEN）。 */
    try {
        (void)reader->create_upload("ci", "x", "1.0", 0, std::string(64, 'a'));
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_FORBIDDEN, err.code());
    }
    try {
        (void)reader->gc(false);
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_FORBIDDEN, err.code());
    }
    try {
        (void)reader->user_list();
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_FORBIDDEN, err.code());
    }
    try {
        (void)reader->session_list();
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_FORBIDDEN, err.code());
    }

    /* 未认证：业务请求拒绝（FR-AUTH-04）。 */
    try {
        (void)anonymous->status();
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_UNAUTHENTICATED, err.code());
    }

    /* Publisher：可上传并删除本人制品；不能删他人制品、不能管理用户。 */
    const std::string digest = fr::sha256_to_hex(fr::sha256("pub-data", 8));
    const fr::msg::CreateUploadOk session =
        publisher->create_upload("ci", "pub-app", "1.0", 8, digest);
    (void)publisher->put_chunk(session.session_id, 0, "pub-data", 8);
    (void)publisher->commit_upload(session.session_id);

    try {
        (void)publisher->user_add("newuser", fr::Role::Reader);
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_FORBIDDEN, err.code());
    }
    try {
        (void)publisher->session_list();
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_FORBIDDEN, err.code());
    }

    /* Admin 创建的制品，Publisher 不能删。 */
    const std::string admin_digest = fr::sha256_to_hex(fr::sha256("admin-data", 10));
    const fr::msg::CreateUploadOk admin_session =
        admin->create_upload("ci", "admin-app", "1.0", 10, admin_digest);
    (void)admin->put_chunk(admin_session.session_id, 0, "admin-data", 10);
    (void)admin->commit_upload(admin_session.session_id);
    try {
        (void)publisher->delete_artifact("ci", "admin-app", "1.0");
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_FORBIDDEN, err.code());
    }
    EXPECT_NO_THROW((void)publisher->delete_artifact("ci", "pub-app", "1.0"));

    /* Admin：管理操作全部允许。 */
    EXPECT_NO_THROW(admin->user_add("svc", fr::Role::Reader));
    EXPECT_NO_THROW((void)admin->user_list());
    const std::string svc_token = admin->token_create("svc", 1); // 先建令牌再禁用
    EXPECT_FALSE(svc_token.empty());
    EXPECT_NO_THROW(admin->user_disable("svc"));
    EXPECT_NO_THROW((void)admin->token_list("svc"));
    EXPECT_NO_THROW((void)admin->gc(true));
    EXPECT_NO_THROW((void)admin->session_list());
    EXPECT_NO_THROW((void)admin->delete_artifact("ci", "admin-app", "1.0"));
}

/// 令牌生命周期（FR-AUTH-02/03）：创建可用 → 撤销后 AUTH 阶段即被拒。
TEST_F(M3M4E2ETest, TokenLifecycle) {
    auto admin = make_client(admin_token_);
    const std::string temp_token = admin->token_create("reader", 0);

    auto temp_client = make_client(temp_token);
    EXPECT_NO_THROW((void)temp_client->status());

    /* 列出 reader 的令牌并全部撤销。 */
    const std::vector<fr::msg::TokenItem> tokens = admin->token_list("reader");
    EXPECT_GE(tokens.size(), 1u);
    for (const fr::msg::TokenItem &item : tokens) {
        if (!item.revoked) {
            admin->token_revoke(item.id);
        }
    }

    /* 撤销后：make_client 在 AUTH 阶段即被拒（连接层抛 UNAUTHENTICATED）。 */
    try {
        auto revoked_client = make_client(temp_token);
        (void)revoked_client->status();
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_UNAUTHENTICATED, err.code()) << "what=" << err.what();
    }
    try {
        auto reader_client = make_client(reader_token_);
        (void)reader_client->status();
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_UNAUTHENTICATED, err.code()) << "what=" << err.what();
    }
}

/// 认证失败速率限制（FR-AUTH-05）：连接内连续失败断开；窗口内超限封禁来源 IP。
TEST_F(M3M4E2ETest, AuthRateLimit) {
    /* 连接内连续 5 次失败：第 5 次失败时服务端主动断开（连接层抛错）。 */
    for (int i = 0; i < 5; i++) {
        try {
            auto bad = make_client("fr_" + std::string(60, static_cast<char>('a' + i)));
            (void)bad->status();
            FAIL() << "expected fr::Error";
        } catch (const fr::Error &err) {
            EXPECT_TRUE(err.code() == FR_E_UNAUTHENTICATED || err.code() == FR_E_IO)
                << "what=" << err.what();
        }
    }
    /* 跨连接累计 21 次失败（窗口内）→ 来源 IP 封禁。 */
    for (int i = 0; i < 16; i++) {
        try {
            auto bad = make_client("fr_" + std::string(60, static_cast<char>('b' + i)));
            (void)bad->status();
            FAIL() << "expected fr::Error";
        } catch (const fr::Error &err) {
            EXPECT_TRUE(err.code() == FR_E_UNAUTHENTICATED || err.code() == FR_E_IO)
                << "what=" << err.what();
        }
    }
    /* 封禁生效后：合法令牌的 AUTH 也被拒（同源 IP，FR-AUTH-05）。 */
    try {
        auto good = make_client(admin_token_);
        (void)good->status();
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_UNAUTHENTICATED, err.code()) << "what=" << err.what();
    }
}

/// 审计记录（FR-ADM-05）：管理操作写入 audit_event（失败路径不上报）。
TEST_F(M3M4E2ETest, AuditTrailRecordsAdminOperations) {
    auto admin = make_client(admin_token_);
    (void)admin->gc(true);
    EXPECT_THROW((void)admin->delete_artifact("ci", "nope", "1.0"), fr::Error);
    (void)admin->user_add("audituser", fr::Role::Reader);
    (void)admin->user_disable("audituser");

    fr::Database db;
    db.open((root_ / "metadata.db").string(), 5000);
    fr::Statement s(db, "SELECT actor, action, target, result FROM audit_event ORDER BY id");
    std::vector<std::string> actions;
    while (s.step()) {
        EXPECT_EQ("admin", s.column_text(0));
        actions.push_back(s.column_text(1));
        EXPECT_EQ("ok", s.column_text(3));
    }
    ASSERT_GE(actions.size(), 3u); // gc.dry_run + user.add + user.disable
    EXPECT_EQ("gc.dry_run", actions[0]);
    EXPECT_EQ("user.add", actions[1]);
    EXPECT_EQ("user.disable", actions[2]);
}

/// 并发连接下的多客户端往返（§7.1 冒烟）。
TEST_F(M3M4E2ETest, ConcurrentClientsSmoke) {
    auto admin = make_client(admin_token_);
    std::vector<std::unique_ptr<fr::Client>> clients;
    for (int i = 0; i < 5; i++) {
        clients.push_back(make_client(reader_token_));
    }
    for (const auto &client : clients) {
        EXPECT_NO_THROW((void)client->status());
    }
    EXPECT_EQ(6u, admin->status().connections);
}

} // namespace
