// fr_schema.cpp - 迁移实现：V1 建立全部 8 张表（需求 §6）。
#include "forgerelay/storage/schema.hpp"

namespace fr {

void migrate(Database &db)
{
    const int64_t current = db.user_version();
    if (current == kSchemaVersion) {
        return;
    }
    if (current > kSchemaVersion) {
        throw_error(FR_E_STATE, "database schema version " + std::to_string(current) +
                                     " is newer than supported " +
                                     std::to_string(kSchemaVersion));
    }

    Tx tx(db);
    if (current < 1) {
        db.exec(R"SQL(
            CREATE TABLE artifact (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                namespace   TEXT    NOT NULL,
                name        TEXT    NOT NULL,
                version     TEXT    NOT NULL,
                size        INTEGER NOT NULL,
                digest      TEXT    NOT NULL,
                chunk_size  INTEGER NOT NULL,
                creator     TEXT    NOT NULL,
                created_at  INTEGER NOT NULL,
                note        TEXT,
                UNIQUE (namespace, name, version)
            );
            CREATE TABLE artifact_chunk (
                artifact_id INTEGER NOT NULL REFERENCES artifact(id) ON DELETE CASCADE,
                ordinal     INTEGER NOT NULL,
                byte_offset INTEGER NOT NULL,
                length      INTEGER NOT NULL,
                digest      TEXT    NOT NULL,
                PRIMARY KEY (artifact_id, ordinal)
            );
            CREATE INDEX idx_artifact_chunk_digest ON artifact_chunk (digest);
            CREATE TABLE chunk (
                digest      TEXT    PRIMARY KEY,
                raw_size    INTEGER NOT NULL,
                stored_size INTEGER NOT NULL,
                state       TEXT    NOT NULL DEFAULT 'ACTIVE',
                pending_at  INTEGER,
                created_at  INTEGER NOT NULL
            );
            CREATE TABLE upload_session (
                id              TEXT    PRIMARY KEY,
                owner           TEXT    NOT NULL,
                state           TEXT    NOT NULL,
                namespace       TEXT    NOT NULL,
                name            TEXT    NOT NULL,
                version         TEXT    NOT NULL,
                expected_size   INTEGER NOT NULL,
                expected_digest TEXT    NOT NULL,
                chunk_size      INTEGER NOT NULL,
                created_at      INTEGER NOT NULL,
                expires_at      INTEGER NOT NULL
            );
            CREATE INDEX idx_session_owner_state ON upload_session (owner, state);
            CREATE INDEX idx_session_state_expiry ON upload_session (state, expires_at);
            CREATE TABLE upload_part (
                session_id TEXT    NOT NULL REFERENCES upload_session(id) ON DELETE CASCADE,
                ordinal    INTEGER NOT NULL,
                digest     TEXT    NOT NULL,
                length     INTEGER NOT NULL,
                PRIMARY KEY (session_id, ordinal)
            );
            CREATE INDEX idx_part_digest ON upload_part (digest);
            CREATE TABLE user (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                username   TEXT    NOT NULL UNIQUE,
                role       TEXT    NOT NULL,
                enabled    INTEGER NOT NULL,
                created_at INTEGER NOT NULL
            );
            CREATE TABLE token (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                user_id    INTEGER NOT NULL REFERENCES user(id),
                token_hash TEXT    NOT NULL UNIQUE,
                expires_at INTEGER,
                revoked_at INTEGER,
                created_at INTEGER NOT NULL
            );
            CREATE TABLE audit_event (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                actor      TEXT    NOT NULL,
                action     TEXT    NOT NULL,
                target     TEXT    NOT NULL,
                result     TEXT    NOT NULL,
                created_at INTEGER NOT NULL
            );
        )SQL");
    }
    db.set_user_version(kSchemaVersion);
    tx.commit();
}

} // namespace fr
