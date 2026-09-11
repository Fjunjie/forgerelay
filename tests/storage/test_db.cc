// test_db.cc - SQLite RAII 封装与迁移测试（DB-01/DB-02/DB-03）。
#include <string>

#include <gtest/gtest.h>

#include "forgerelay/storage/db.hpp"
#include "forgerelay/storage/error.hpp"
#include "forgerelay/storage/schema.hpp"

namespace {

class DbTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ =
            std::string(::testing::TempDir()) + "fr_m2_db_" + std::to_string(++counter_) + ".db";
        std::remove(path_.c_str());
    }

    void TearDown() override {
        std::remove(path_.c_str());
        std::remove((path_ + "-wal").c_str());
        std::remove((path_ + "-shm").c_str());
    }

    static int counter_;
    std::string path_;
};

int DbTest::counter_ = 0;

TEST_F(DbTest, OpenSetsWalAndForeignKeys) {
    fr::Database db;
    db.open(path_, 5000);
    fr::Statement mode(db, "PRAGMA journal_mode");
    ASSERT_TRUE(mode.step());
    EXPECT_EQ("wal", mode.column_text(0));

    fr::Statement fk(db, "PRAGMA foreign_keys");
    ASSERT_TRUE(fk.step());
    EXPECT_EQ(1, fk.column_int(0));
}

TEST_F(DbTest, MigrateIsMonotonicAndIdempotent) {
    {
        fr::Database db;
        db.open(path_, 5000);
        EXPECT_EQ(0, db.user_version());
        fr::migrate(db);
        EXPECT_EQ(fr::kSchemaVersion, db.user_version());
    }
    {
        // 重新打开：已是最新版本，迁移为无害操作。
        fr::Database db;
        db.open(path_, 5000);
        fr::migrate(db);
        EXPECT_EQ(fr::kSchemaVersion, db.user_version());
    }
}

TEST_F(DbTest, MigrateRejectsNewerSchema) {
    {
        fr::Database db;
        db.open(path_, 5000);
        db.exec("PRAGMA user_version=99");
    }
    fr::Database db;
    db.open(path_, 5000);
    try {
        fr::migrate(db);
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_STATE, err.code());
    }
}

TEST_F(DbTest, SchemaEnforcesUniqueArtifactIdentity) {
    fr::Database db;
    db.open(path_, 5000);
    fr::migrate(db);
    const char *insert =
        "INSERT INTO artifact (namespace, name, version, size, digest, chunk_size, creator, "
        "created_at, note) VALUES ('ns','app','1.0',1,'d',4,'ci',1,NULL)";
    db.exec(insert);
    try {
        db.exec(insert);
        FAIL() << "expected unique violation";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_IO, err.code()); // 约束冲突映射为 IO 层错误
    }
}

TEST_F(DbTest, SchemaEnforcesPartUniquePerOrdinal) {
    fr::Database db;
    db.open(path_, 5000);
    fr::migrate(db);
    db.exec("INSERT INTO upload_session (id, owner, state, namespace, name, version, "
            "expected_size, expected_digest, chunk_size, created_at, expires_at) "
            "VALUES ('s1','ci','OPEN','ns','app','1.0',4,'d',4,1,100)");
    db.exec("INSERT INTO upload_part (session_id, ordinal, digest, length) "
            "VALUES ('s1',0,'aa',4)");
    try {
        db.exec("INSERT INTO upload_part (session_id, ordinal, digest, length) "
                "VALUES ('s1',0,'bb',4)");
        FAIL() << "expected unique violation";
    } catch (const fr::Error &) {
        SUCCEED();
    }
}

TEST_F(DbTest, BindParamsAreUsed) {
    fr::Database db;
    db.open(path_, 5000);
    fr::migrate(db);
    {
        fr::Statement s(db, "INSERT INTO user (username, role, enabled, created_at) "
                            "VALUES (?1,?2,?3,?4)");
        s.bind_text(1, "alice");
        s.bind_text(2, "ADMIN");
        s.bind_int(3, 1);
        s.bind_int(4, 42);
        s.step();
    }
    fr::Statement q(db, "SELECT role, created_at FROM user WHERE username=?1");
    q.bind_text(1, std::string("alice"));
    ASSERT_TRUE(q.step());
    EXPECT_EQ("ADMIN", q.column_text(0));
    EXPECT_EQ(42, q.column_int(1));
}

} // namespace
