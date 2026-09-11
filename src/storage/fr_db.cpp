// fr_db.cpp - SQLite RAII 封装实现。
#include "forgerelay/storage/db.hpp"

#include <utility>

namespace fr {

namespace {

void check_rc(int rc, sqlite3 *db, const char *what)
{
    if (rc != SQLITE_OK) {
        std::string detail = what != nullptr ? what : "sqlite";
        detail += ": ";
        const char *msg = db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        detail += msg != nullptr ? msg : "unknown error";
        throw_error(FR_E_IO, detail);
    }
}

} // namespace

Database::~Database()
{
    close();
}

Database::Database(Database &&other) noexcept : handle_(other.handle_)
{
    other.handle_ = nullptr;
}

Database &Database::operator=(Database &&other) noexcept
{
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

void Database::open(const std::string &path, int busy_timeout_ms)
{
    close();
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    check_rc(sqlite3_open_v2(path.c_str(), &handle_, flags, nullptr), nullptr, "sqlite3_open_v2");
    try {
        check_rc(sqlite3_busy_timeout(handle_, busy_timeout_ms), handle_, "busy_timeout");
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA foreign_keys=ON");
        exec("PRAGMA synchronous=FULL");
    } catch (...) {
        close();
        throw;
    }
}

void Database::close() noexcept
{
    if (handle_ != nullptr) {
        sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }
}

void Database::exec(std::string_view sql)
{
    if (handle_ == nullptr) {
        throw_error(FR_E_STATE, "database not open");
    }
    char *errmsg = nullptr;
    const int rc = sqlite3_exec(handle_, std::string(sql).c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string detail = "exec: ";
        detail += errmsg != nullptr ? errmsg : sqlite3_errstr(rc);
        sqlite3_free(errmsg);
        throw_error(FR_E_IO, detail);
    }
}

sqlite3_stmt *Database::prepare(std::string_view sql)
{
    if (handle_ == nullptr) {
        throw_error(FR_E_STATE, "database not open");
    }
    sqlite3_stmt *stmt = nullptr;
    check_rc(sqlite3_prepare_v2(handle_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr),
             handle_, "prepare");
    return stmt;
}

int64_t Database::user_version()
{
    Statement stmt(*this, "PRAGMA user_version");
    (void)stmt.step();
    return stmt.column_int(0);
}

void Database::set_user_version(int64_t version)
{
    exec("PRAGMA user_version=" + std::to_string(version));
}

void Database::begin_immediate()
{
    exec("BEGIN IMMEDIATE");
}

void Database::commit()
{
    exec("COMMIT");
}

void Database::rollback() noexcept
{
    if (handle_ != nullptr) {
        (void)sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

Statement::Statement(Database &db, std::string_view sql) : stmt_(db.prepare(sql)) {}

Statement::~Statement()
{
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
    }
}

void Statement::bind_int(int index, int64_t value)
{
    check_rc(sqlite3_bind_int64(stmt_, index, value), sqlite3_db_handle(stmt_), "bind_int");
}

void Statement::bind_text(int index, std::string_view value)
{
    /* SQLITE_TRANSIENT：SQLite 在 step 前复制字符串，调用方缓冲可立即复用。 */
    check_rc(sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                               SQLITE_TRANSIENT),
             sqlite3_db_handle(stmt_), "bind_text");
}

void Statement::bind_blob(int index, std::span<const uint8_t> value)
{
    const void *data = value.empty() ? nullptr : value.data();
    check_rc(sqlite3_bind_blob(stmt_, index, data, static_cast<int>(value.size()), SQLITE_TRANSIENT),
             sqlite3_db_handle(stmt_), "bind_blob");
}

void Statement::bind_null(int index)
{
    check_rc(sqlite3_bind_null(stmt_, index), sqlite3_db_handle(stmt_), "bind_null");
}

bool Statement::step()
{
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    check_rc(rc, sqlite3_db_handle(stmt_), "step");
    return false;
}

void Statement::reset()
{
    check_rc(sqlite3_reset(stmt_), sqlite3_db_handle(stmt_), "reset");
    check_rc(sqlite3_clear_bindings(stmt_), sqlite3_db_handle(stmt_), "clear_bindings");
}

int64_t Statement::column_int(int index) const
{
    return sqlite3_column_int64(stmt_, index);
}

std::string Statement::column_text(int index) const
{
    const unsigned char *text = sqlite3_column_text(stmt_, index);
    if (text == nullptr) {
        return std::string();
    }
    const int bytes = sqlite3_column_bytes(stmt_, index);
    return std::string(reinterpret_cast<const char *>(text), static_cast<size_t>(bytes));
}


int64_t fr::Database::changes() const
{
    return handle_ != nullptr ? sqlite3_changes(handle_) : 0;
}

} // namespace fr
