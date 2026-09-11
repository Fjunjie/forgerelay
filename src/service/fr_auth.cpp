// fr_auth.cpp - 用户/令牌注册表与审计实现（FR-AUTH、FR-ADM-05）。
#include "forgerelay/service.hpp"

#include <chrono>
#include <mutex>

#include "forgerelay/storage/db.hpp"
#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"
#include "forgerelay/storage/schema.hpp"
#include "forgerelay/fr_path.h"

namespace fr {

const char *role_name(Role role)
{
    switch (role) {
    case Role::Reader:
        return "Reader";
    case Role::Publisher:
        return "Publisher";
    case Role::Admin:
        return "Admin";
    default:
        return "Reader";
    }
}

bool role_from(const std::string &name, Role &out)
{
    if (name == "Reader") {
        out = Role::Reader;
        return true;
    }
    if (name == "Publisher") {
        out = Role::Publisher;
        return true;
    }
    if (name == "Admin") {
        out = Role::Admin;
        return true;
    }
    return false;
}

namespace {

int64_t now_seconds()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string role_to_text(Role role)
{
    return role_name(role);
}

Role role_from_text(const std::string &text)
{
    Role role = Role::Reader;
    if (!role_from(text, role)) {
        throw_error(FR_E_INTERNAL, "unknown role text in database: " + text);
    }
    return role;
}

} // namespace

struct AuthRegistry::Impl {
    Database db;
    std::mutex mu; // 同一连接上的语句不可并发；调用方线程池需串行化

    explicit Impl(const std::string &db_path)
    {
        db.open(db_path, 5000);
        migrate(db);
    }
};

AuthRegistry::AuthRegistry(const std::string &db_path) : impl_(new Impl(db_path)) {}

AuthRegistry::~AuthRegistry() = default;

void AuthRegistry::user_add(const std::string &username, Role role)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (username.empty() || !fr_ident_valid_component(username.c_str())) {
        throw_error(FR_E_ARG, "username must be a valid component (1-64 chars)");
    }
    Statement s(impl_->db, "INSERT INTO user (username, role, enabled, created_at) "
                           "VALUES (?1,?2,1,?3)");
    s.bind_text(1, username);
    s.bind_text(2, role_to_text(role));
    s.bind_int(3, now_seconds());
    s.step(); // UNIQUE 冲突 → SQLITE_CONSTRAINT → fr::Error(FR_E_IO)
}

void AuthRegistry::user_disable(const std::string &username)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    Statement s(impl_->db, "UPDATE user SET enabled=0 WHERE username=?1");
    s.bind_text(1, username);
    s.step();
    if (impl_->db.changes() == 0) {
        throw_error(FR_E_NOTFOUND, "user not found: " + username);
    }
}

std::vector<msg::UserItem> AuthRegistry::user_list()
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<msg::UserItem> out;
    Statement s(impl_->db, "SELECT username, role, enabled, created_at FROM user ORDER BY id");
    while (s.step()) {
        msg::UserItem item;
        item.username = s.column_text(0);
        item.role = static_cast<uint8_t>(role_from_text(s.column_text(1)));
        item.enabled = s.column_int(2) != 0;
        item.created_at = s.column_int(3);
        out.push_back(std::move(item));
    }
    return out;
}

std::string AuthRegistry::token_create(const std::string &username, uint32_t ttl_hours)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    const int64_t now = now_seconds();
    int64_t user_id = 0;
    {
        Statement s(impl_->db, "SELECT id, enabled FROM user WHERE username=?1");
        s.bind_text(1, username);
        if (!s.step()) {
            throw_error(FR_E_NOTFOUND, "user not found: " + username);
        }
        if (s.column_int(1) == 0) {
            throw_error(FR_E_ARG, "user is disabled: " + username);
        }
        user_id = s.column_int(0);
    }

    const Sha256 raw = random_bytes();
    const std::string token = "fr_" + sha256_to_hex(raw);
    const std::string token_hash = sha256_to_hex(sha256(token.data(), token.size()));
    const int64_t expires = ttl_hours == 0 ? 0 : now + static_cast<int64_t>(ttl_hours) * 3600;

    Statement s(impl_->db, "INSERT INTO token (user_id, token_hash, expires_at, revoked_at, "
                           "created_at) VALUES (?1,?2,?3,NULL,?4)");
    s.bind_int(1, user_id);
    s.bind_text(2, token_hash);
    s.bind_int(3, expires);
    s.bind_int(4, now);
    s.step();
    return token;
}

std::vector<msg::TokenItem> AuthRegistry::token_list(const std::string &username)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<msg::TokenItem> out;
    Statement s(impl_->db,
                "SELECT t.id, u.username, t.created_at, t.expires_at, t.revoked_at "
                "FROM token t JOIN user u ON u.id = t.user_id WHERE u.username=?1 ORDER BY t.id");
    s.bind_text(1, username);
    while (s.step()) {
        msg::TokenItem item;
        item.id = static_cast<uint64_t>(s.column_int(0));
        item.username = s.column_text(1);
        item.created_at = s.column_int(2);
        item.expires_at = s.column_int(3);
        item.revoked = s.column_int(4) != 0;
        out.push_back(std::move(item));
    }
    return out;
}

void AuthRegistry::token_revoke(uint64_t token_id)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    Statement s(impl_->db, "UPDATE token SET revoked_at=?1 WHERE id=?2 AND revoked_at IS NULL");
    s.bind_int(1, now_seconds());
    s.bind_int(2, static_cast<int64_t>(token_id));
    s.step();
    if (impl_->db.changes() == 0) {
        throw_error(FR_E_NOTFOUND, "token not found or already revoked");
    }
}

msg::AuthOk AuthRegistry::verify(const std::string &token)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (token.empty()) {
        throw_error(FR_E_UNAUTHENTICATED, "empty token");
    }
    const std::string token_hash = sha256_to_hex(sha256(token.data(), token.size()));
    Statement s(impl_->db,
                "SELECT t.expires_at, t.revoked_at, u.username, u.role, u.enabled "
                "FROM token t JOIN user u ON u.id = t.user_id WHERE t.token_hash=?1");
    s.bind_text(1, token_hash);
    if (!s.step()) {
        throw_error(FR_E_UNAUTHENTICATED, "unknown token");
    }
    const int64_t expires = s.column_int(0);
    const bool revoked = s.column_int(1) != 0;
    msg::AuthOk ok;
    ok.username = s.column_text(2);
    ok.role = static_cast<uint8_t>(role_from_text(s.column_text(3)));
    const bool enabled = s.column_int(4) != 0;
    if (revoked) {
        throw_error(FR_E_UNAUTHENTICATED, "token revoked");
    }
    if (expires != 0 && expires <= now_seconds()) {
        throw_error(FR_E_UNAUTHENTICATED, "token expired");
    }
    if (!enabled) {
        throw_error(FR_E_UNAUTHENTICATED, "user disabled");
    }
    return ok;
}

void AuthRegistry::audit(const std::string &actor, const std::string &action,
                         const std::string &target, const std::string &result)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    Statement s(impl_->db, "INSERT INTO audit_event (actor, action, target, result, created_at) "
                           "VALUES (?1,?2,?3,?4,?5)");
    s.bind_text(1, actor);
    s.bind_text(2, action);
    s.bind_text(3, target);
    s.bind_text(4, result);
    s.bind_int(5, now_seconds());
    s.step();
}

bool AuthRegistry::has_no_users()
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    Statement s(impl_->db, "SELECT COUNT(*) FROM user");
    return s.step() && s.column_int(0) == 0;
}

int64_t AuthRegistry::active_session_count()
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    Statement s(impl_->db, "SELECT COUNT(*) FROM upload_session WHERE state='OPEN'");
    return s.step() ? s.column_int(0) : 0;
}

} // namespace fr
