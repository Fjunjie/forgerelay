// fr/db.hpp - SQLite RAII 封装（DB-01/DB-02/DB-03/DB-05）。
//
// - 连接：sqlite3_open_v2(READWRITE|CREATE)，WAL 模式、busy timeout、
//   foreign_keys=ON、synchronous=FULL（发布事务的断电持久性，FR-STO-05）；
// - 语句：prepare/bind/step/column 全部带错误检查，参数一律绑定（DB-02）；
// - 事务：begin_immediate/commit/rollback 显式接口，RAII 守卫 Tx；
// - 迁移：PRAGMA user_version 单调递增，每个版本在单个事务内执行（DB-03）。
//
// 线程安全：单个 Database/Statement 实例非线程安全（需外部同步）；
// 无全局可变状态。错误以 fr::Error 异常抛出（D-16）。
#ifndef FR_STORAGE_DB_HPP
#define FR_STORAGE_DB_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <sqlite3.h>

#include "forgerelay/storage/error.hpp"

namespace fr {

/** 打开/复用元数据数据库的连接封装。 */
class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    Database(Database &&other) noexcept;
    Database &operator=(Database &&other) noexcept;

    /**
     * 打开（必要时创建）数据库文件并设置 WAL/超时/外键/同步级别。
     *
     * @param[in] path 数据库文件路径。
     * @param[in] busy_timeout_ms 忙等超时（毫秒，DB-01）。
     * @throws fr::Error 打开或 PRAGMA 失败。
     */
    void open(const std::string &path, int busy_timeout_ms);

    /** 关闭连接；可安全重复调用。 */
    void close() noexcept;

    /** 是否已打开。 */
    bool is_open() const noexcept { return handle_ != nullptr; }

    /** 执行无结果 SQL。 */
    void exec(std::string_view sql);

    /** 预编译 SQL 语句。 */
    sqlite3_stmt *prepare(std::string_view sql);

    /** 当前 user_version。 */
    int64_t user_version();

    /** 设置 user_version（调用方负责事务）。 */
    void set_user_version(int64_t version);

    /** 开始 IMMEDIATE 事务。 */
    void begin_immediate();

    /** 提交当前事务。 */
    void commit();

    /** 回滚当前事务（无活动事务时为无害操作）。 */
    void rollback() noexcept;

    /** 底层句柄（供语句封装使用）。 */
    sqlite3 *handle() const noexcept { return handle_; }

private:
    sqlite3 *handle_ = nullptr;
};

/** 事务 RAII 守卫：析构时未提交则回滚（LIFE-04）。 */
class Tx {
public:
    explicit Tx(Database &db) : db_(db) { db_.begin_immediate(); }
    ~Tx() {
        if (!done_) {
            db_.rollback();
        }
    }
    Tx(const Tx &) = delete;
    Tx &operator=(const Tx &) = delete;

    void commit() {
        db_.commit();
        done_ = true;
    }

    /** 显式回滚并标记完成（析构不再回滚）。 */
    void rollback() {
        db_.rollback();
        done_ = true;
    }

private:
    Database &db_;
    bool done_ = false;
};

/** 预编译语句封装：绑定参数、步进、取列（DB-02）。 */
class Statement {
public:
    Statement(Database &db, std::string_view sql);
    ~Statement();
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    void bind_int(int index, int64_t value);
    void bind_text(int index, std::string_view value);
    void bind_blob(int index, std::span<const uint8_t> value);
    void bind_null(int index);

    /**
     * 步进一行。
     *
     * @return true 有可用行；false 完成（DONE）。
     * @throws fr::Error SQLITE 错误。
     */
    bool step();

    /** 重置语句以便复用（保留绑定之外的状态清空）。 */
    void reset();

    int64_t column_int(int index) const;
    std::string column_text(int index) const;

private:
    sqlite3_stmt *stmt_ = nullptr;
};

} // namespace fr

#endif /* FR_STORAGE_DB_HPP */
