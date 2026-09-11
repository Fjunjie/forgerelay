// fr_storage.cpp - M2 存储层门面实现。
#include "forgerelay/storage/storage.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <utility>

#include "forgerelay/fr_hex.h"
#include "forgerelay/fr_path.h"
#include "forgerelay/storage/chunk_store.hpp"
#include "forgerelay/storage/db.hpp"
#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"
#include "forgerelay/storage/schema.hpp"

namespace fr {

namespace {

/* 会话状态与文本互转（DB 存储口径）。 */
const char *state_to_text(SessionState state)
{
    switch (state) {
    case SessionState::OPEN:
        return "OPEN";
    case SessionState::COMMITTING:
        return "COMMITTING";
    case SessionState::COMPLETED:
        return "COMPLETED";
    case SessionState::ABORTED:
        return "ABORTED";
    case SessionState::EXPIRED:
        return "EXPIRED";
    default:
        throw_error(FR_E_INTERNAL, "unknown session state");
    }
}

SessionState state_from_text(const std::string &text)
{
    if (text == "OPEN") {
        return SessionState::OPEN;
    }
    if (text == "COMMITTING") {
        return SessionState::COMMITTING;
    }
    if (text == "COMPLETED") {
        return SessionState::COMPLETED;
    }
    if (text == "ABORTED") {
        return SessionState::ABORTED;
    }
    if (text == "EXPIRED") {
        return SessionState::EXPIRED;
    }
    throw_error(FR_E_INTERNAL, "unknown session state text: " + text);
}

int64_t default_clock()
{
    return static_cast<int64_t>(time(nullptr));
}

/* 会话 ID：纳秒时间戳 + 进程内计数（唯一性而非机密性，与 D-09 同口径）。 */
std::string new_session_id()
{
    static std::atomic<uint64_t> counter{0};
    const auto now_ns = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const uint64_t seq = counter.fetch_add(1) + 1;
    char buf[32];
    (void)snprintf(buf, sizeof(buf), "%016llx%08llx", static_cast<unsigned long long>(now_ns),
                   static_cast<unsigned long long>(seq & 0xffffffffull));
    return std::string(buf);
}

SessionInfo read_session_row(Statement &stmt)
{
    SessionInfo info;
    info.id = stmt.column_text(0);
    info.owner = stmt.column_text(1);
    info.state = state_from_text(stmt.column_text(2));
    info.ns = stmt.column_text(3);
    info.name = stmt.column_text(4);
    info.version = stmt.column_text(5);
    info.expected_size = static_cast<uint64_t>(stmt.column_int(6));
    info.expected_digest = stmt.column_text(7);
    info.chunk_size = static_cast<uint64_t>(stmt.column_int(8));
    info.created_at = stmt.column_int(9);
    info.expires_at = stmt.column_int(10);
    return info;
}

/* 会话列清单（列序与 read_session_row / INSERT 占位符一致）。 */
constexpr const char *kSessionColumns =
    "id, owner, state, namespace, name, version, expected_size, expected_digest, chunk_size, "
    "created_at, expires_at";

} // namespace

const char *session_state_name(SessionState state)
{
    return state_to_text(state);
}

SessionState session_state_from(const std::string &name)
{
    return state_from_text(name);
}

struct Storage::Impl {
    StorageConfig cfg;
    Database db;
    ChunkStore chunks;

    /** 按条件统计无引用且可清理的块（供删除标记与 GC 双重校验，FR-GC-02）。 */
    bool chunk_referenced(const std::string &digest)
    {
        {
            Statement s(db, "SELECT COUNT(*) FROM artifact_chunk WHERE digest=?1");
            s.bind_text(1, digest);
            if (s.step() && s.column_int(0) > 0) {
                return true;
            }
        }
        {
            Statement s(db,
                        "SELECT COUNT(*) FROM upload_part p JOIN upload_session s "
                        "ON s.id = p.session_id "
                        "WHERE p.digest=?1 AND s.state IN ('OPEN','COMMITTING')");
            s.bind_text(1, digest);
            if (s.step() && s.column_int(0) > 0) {
                return true;
            }
        }
        return false;
    }

    /** 会话按时间过期（OPEN 且 expires_at <= now）→ EXPIRED。 */
    void expire_session_if_due(const SessionInfo &session, int64_t now)
    {
        if (session.state == SessionState::OPEN && session.expires_at <= now) {
            Statement s(db, "UPDATE upload_session SET state='EXPIRED' WHERE id=?1 AND state='OPEN'");
            s.bind_text(1, session.id);
            s.step();
            mark_unreferenced_parts(session.id, now);
        }
    }

    void delete_parts(const std::string &session_id)
    {
        Statement s(db, "DELETE FROM upload_part WHERE session_id=?1");
        s.bind_text(1, session_id);
        s.step();
    }

    /** 将分片摘要中已无任何引用的块标记为待清理（FR-GC-01/02）。 */
    void mark_unreferenced_parts(const std::string &session_id, int64_t now)
    {
        std::vector<std::string> digests;
        {
            Statement s(db, "SELECT digest FROM upload_part WHERE session_id=?1");
            s.bind_text(1, session_id);
            while (s.step()) {
                digests.push_back(s.column_text(0));
            }
        }
        delete_parts(session_id);
        for (const std::string &digest : digests) {
            if (chunk_referenced(digest)) {
                continue;
            }
            Statement mark(db, "UPDATE chunk SET state='PENDING_DELETE', pending_at=?1 "
                               "WHERE digest=?2 AND state='ACTIVE'");
            mark.bind_int(1, now);
            mark.bind_text(2, digest);
            mark.step();
        }
    }

    /** 条件状态转移（FR-UP-07 条件更新）；返回是否成功。 */
    bool transition(const std::string &session_id, SessionState from, SessionState to)
    {
        Statement s(db, "UPDATE upload_session SET state=?1 WHERE id=?2 AND state=?3");
        s.bind_text(1, state_to_text(to));
        s.bind_text(2, session_id);
        s.bind_text(3, state_to_text(from));
        s.step();
        return db_changes() > 0;
    }

    int64_t db_changes()
    {
        return sqlite3_changes(db.handle());
    }

    SessionInfo load_session(const std::string &session_id)
    {
        Statement s(db, std::string("SELECT ") + kSessionColumns +
                            " FROM upload_session WHERE id=?1");
        s.bind_text(1, session_id);
        if (!s.step()) {
            throw_error(FR_E_NOTFOUND, "session not found");
        }
        return read_session_row(s);
    }
};

Storage::~Storage() = default;

std::unique_ptr<Storage> Storage::open(StorageConfig config)
{
    if (config.root.empty()) {
        throw_error(FR_E_ARG, "storage root must not be empty");
    }
    if (config.high_watermark_percent < 1 || config.high_watermark_percent > 100) {
        throw_error(FR_E_ARG, "high_watermark_percent must be within [1, 100]");
    }
    if (config.chunk_size < 1024ull * 1024 || config.chunk_size > 8ull * 1024 * 1024) {
        throw_error(FR_E_ARG, "chunk_size must be within [1 MiB, 8 MiB]"); // FR-UP-02
    }
    if (config.session_hours < 1 || config.session_hours > 168) {
        throw_error(FR_E_ARG, "session_hours must be within [1, 168]");
    }
    if (!config.clock) {
        config.clock = default_clock;
    }

    auto storage = std::unique_ptr<Storage>(new Storage());
    storage->impl_ = std::unique_ptr<Impl>(new Impl());
    Impl &impl = *storage->impl_;
    impl.cfg = std::move(config);

    std::filesystem::create_directories(impl.cfg.root);
    impl.chunks.open(impl.cfg.root);
    impl.db.open((impl.cfg.root / "metadata.db").string(), impl.cfg.busy_timeout_ms);
    migrate(impl.db);

    /* 启动恢复（FR-STO-06）：临时文件清理 + COMMITTING 回滚 + 过期会话处理。 */
    (void)storage->run_maintenance();
    return storage;
}

// ---- 上传会话 ------------------------------------------------------------

SessionInfo Storage::create_session(const std::string &ns, const std::string &name,
                                    const std::string &version, uint64_t expected_size,
                                    const std::string &expected_digest, const std::string &owner)
{
    Impl &impl = *impl_;
    if (!fr_ident_valid_component(ns.c_str()) || !fr_ident_valid_component(name.c_str()) ||
        !fr_ident_valid_version(version.c_str())) {
        throw_error(FR_E_ARG, "invalid artifact identifier");
    }
    if (owner.empty()) {
        throw_error(FR_E_ARG, "owner must not be empty");
    }
    if (!fr_digest_hex_valid(expected_digest.c_str())) {
        throw_error(FR_E_ARG, "expected digest must be 64 lowercase hex chars");
    }
    const int64_t now = impl.cfg.clock();
    const uint64_t chunk_size = impl.cfg.chunk_size; // FR-UP-02 可配置，默认 4 MiB

    Tx tx(impl.db);
    {
        Statement s(impl.db, "SELECT COUNT(*) FROM upload_session WHERE owner=?1 AND state='OPEN'");
        s.bind_text(1, owner);
        if (s.step() && s.column_int(0) >= impl.cfg.max_sessions_per_owner) {
            throw_error(FR_E_LIMIT, "per-owner active session limit reached");
        }
    }
    {
        Statement s(impl.db, "SELECT COALESCE(SUM(raw_size),0) FROM chunk WHERE state='ACTIVE'");
        if (s.step()) {
            const uint64_t used = static_cast<uint64_t>(s.column_int(0));
            const uint64_t watermark = impl.cfg.capacity_bytes *
                                       static_cast<uint64_t>(impl.cfg.high_watermark_percent) / 100;
            if (used >= watermark) {
                throw_error(FR_E_LIMIT, "storage high watermark reached"); // FR-GC-04
            }
        }
    }

    SessionInfo info;
    info.id = new_session_id();
    info.owner = owner;
    info.state = SessionState::OPEN;
    info.ns = ns;
    info.name = name;
    info.version = version;
    info.expected_size = expected_size;
    info.expected_digest = expected_digest;
    info.chunk_size = chunk_size;
    info.created_at = now;
    info.expires_at = now + static_cast<int64_t>(impl.cfg.session_hours) * 3600;

        Statement s(impl.db, std::string("INSERT INTO upload_session (") + kSessionColumns +
                                 ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)");
    s.bind_text(1, info.id);
    s.bind_text(2, info.owner);
    s.bind_text(3, state_to_text(info.state));
    s.bind_text(4, info.ns);
    s.bind_text(5, info.name);
    s.bind_text(6, info.version);
    s.bind_int(7, static_cast<int64_t>(info.expected_size));
    s.bind_text(8, info.expected_digest);
    s.bind_int(9, static_cast<int64_t>(info.chunk_size));
    s.bind_int(10, info.created_at);
    s.bind_int(11, info.expires_at);
    s.step();
    tx.commit();
    return info;
}

SessionInfo Storage::get_session(const std::string &session_id)
{
    return impl_->load_session(session_id);
}

ChunkAccept Storage::put_chunk(const std::string &session_id, uint64_t ordinal, const void *data,
                               size_t len)
{
    Impl &impl = *impl_;
    const int64_t now = impl.cfg.clock();
    SessionInfo session = impl.load_session(session_id);
    impl.expire_session_if_due(session, now);
    session = impl.load_session(session_id);
    if (session.state == SessionState::EXPIRED) {
        throw_error(FR_E_EXPIRED, "upload session expired");
    }
    if (session.state != SessionState::OPEN) {
        throw_error(FR_E_STATE, "session is not OPEN");
    }

    const uint64_t chunk_count =
        session.expected_size == 0 ? 0 : (session.expected_size + session.chunk_size - 1) / session.chunk_size;
    if (ordinal >= chunk_count) {
        throw_error(FR_E_RANGE, "chunk ordinal beyond expectation");
    }
    if (len == 0 || len > session.chunk_size) {
        throw_error(FR_E_RANGE, "chunk length must be within (0, chunk_size]");
    }

    /* 内容摘要校验由 chunk_store.put 内部复核（FR-UP-05）。 */
    const Sha256 digest = sha256(data, len);
    const std::string digest_hex = sha256_to_hex(digest);
    impl.chunks.put(data, len, digest_hex);

    Tx tx(impl.db);
    {
        /* 分片登记：同编号同内容幂等，不同内容冲突（FR-UP-06）。 */
        Statement existing(impl.db,
                           "SELECT digest FROM upload_part WHERE session_id=?1 AND ordinal=?2");
        existing.bind_text(1, session_id);
        existing.bind_int(2, static_cast<int64_t>(ordinal));
        if (existing.step()) {
            if (existing.column_text(0) == digest_hex) {
                ChunkAccept accept;
                accept.digest = digest_hex;
                accept.reused = true;
                return accept; // Tx 析构回滚无害（无写入）。
            }
            throw_error(FR_E_CONFLICT, "chunk ordinal already used with different content");
        }
    }
    {
        Statement s(impl.db,
                    "INSERT OR IGNORE INTO chunk (digest, raw_size, stored_size, state, created_at) "
                    "VALUES (?1,?2,?3,'ACTIVE',?4)");
        s.bind_text(1, digest_hex);
        s.bind_int(2, static_cast<int64_t>(len));
        s.bind_int(3, static_cast<int64_t>(len));
        s.bind_int(4, now);
        s.step();
        /* 复用已存在但被标记待清理的块：重新激活（防 GC 误删，FR-GC-02）。 */
        Statement reactivate(impl.db,
                             "UPDATE chunk SET state='ACTIVE', pending_at=NULL "
                             "WHERE digest=?1 AND state='PENDING_DELETE'");
        reactivate.bind_text(1, digest_hex);
        reactivate.step();
    }
    {
        Statement s(impl.db, "INSERT INTO upload_part (session_id, ordinal, digest, length) "
                             "VALUES (?1,?2,?3,?4)");
        s.bind_text(1, session_id);
        s.bind_int(2, static_cast<int64_t>(ordinal));
        s.bind_text(3, digest_hex);
        s.bind_int(4, static_cast<int64_t>(len));
        s.step();
    }
    tx.commit();

    ChunkAccept accept;
    accept.digest = digest_hex;
    accept.reused = false;
    return accept;
}

ArtifactInfo Storage::commit_session(const std::string &session_id)
{
    Impl &impl = *impl_;
    const int64_t now = impl.cfg.clock();
    SessionInfo session = impl.load_session(session_id);
    impl.expire_session_if_due(session, now);
    if (!impl.transition(session_id, SessionState::OPEN, SessionState::COMMITTING)) {
        session = impl.load_session(session_id);
        if (session.state == SessionState::EXPIRED) {
            throw_error(FR_E_EXPIRED, "upload session expired");
        }
        throw_error(FR_E_STATE, "session is not OPEN");
    }

    auto rollback_to_open = [&]() {
        (void)impl.transition(session_id, SessionState::COMMITTING, SessionState::OPEN);
    };

    /* 校验块数/顺序/长度（FR-UP-08）。 */
    const uint64_t chunk_count = session.expected_size == 0
                                     ? 0
                                     : (session.expected_size + session.chunk_size - 1) / session.chunk_size;
    std::vector<ManifestEntry> manifest;
    manifest.reserve(chunk_count);
    {
        Statement s(impl.db, "SELECT ordinal, digest, length FROM upload_part "
                             "WHERE session_id=?1 ORDER BY ordinal");
        s.bind_text(1, session_id);
        int64_t expected_ordinal = 0;
        uint64_t total = 0;
        while (s.step()) {
            ManifestEntry entry;
            entry.ordinal = s.column_int(0);
            entry.digest = s.column_text(1);
            entry.length = static_cast<uint64_t>(s.column_int(2));
            if (entry.ordinal != expected_ordinal) {
                rollback_to_open();
                throw_error(FR_E_STATE, "chunk ordinals are not contiguous");
            }
            expected_ordinal++;
            if (entry.length > session.chunk_size) {
                rollback_to_open();
                throw_error(FR_E_STATE, "chunk length exceeds chunk_size");
            }
            entry.offset = total;
            total += entry.length;
            manifest.push_back(std::move(entry));
        }
        if (static_cast<uint64_t>(manifest.size()) != chunk_count) {
            rollback_to_open();
            throw_error(FR_E_STATE, "chunk count mismatch: got " +
                                        std::to_string(manifest.size()) + ", want " +
                                        std::to_string(chunk_count));
        }
        if (total != session.expected_size) {
            rollback_to_open();
            throw_error(FR_E_STATE, "total chunk length mismatch");
        }
    }

    /* 整体摘要校验（FR-UP-08）：按序流式读取块文件计算 SHA-256。 */
    {
        Sha256Stream stream;
        fr_buf buffer;
        fr_buf_init(&buffer);
        try {
            for (const ManifestEntry &entry : manifest) {
                fr_buf_clear(&buffer);
                impl.chunks.get(entry.digest, &buffer);
                stream.update(buffer.data, buffer.len);
            }
        } catch (...) {
            fr_buf_destroy(&buffer);
            rollback_to_open();
            throw;
        }
        fr_buf_destroy(&buffer);
        if (sha256_to_hex(stream.finish()) != session.expected_digest) {
            rollback_to_open();
            throw_error(FR_E_CONFLICT, "overall digest mismatch");
        }
    }

    /* 原子发布：元数据 + 清单 + 会话完成，单事务（FR-STO-05）。 */
    Tx tx(impl.db);
    {
        Statement s(impl.db, "SELECT 1 FROM artifact WHERE namespace=?1 AND name=?2 AND version=?3");
        s.bind_text(1, session.ns);
        s.bind_text(2, session.name);
        s.bind_text(3, session.version);
        if (s.step()) {
            tx.rollback(); // 回滚发布，会话回 OPEN（FR-ART-05 不可覆盖）。
            rollback_to_open();
            throw_error(FR_E_EXISTS, "artifact already exists and cannot be overwritten");
        }
    }
    int64_t artifact_id = 0;
    {
        Statement s(impl.db,
                    "INSERT INTO artifact (namespace, name, version, size, digest, chunk_size, "
                    "creator, created_at, note) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,NULL)");
        s.bind_text(1, session.ns);
        s.bind_text(2, session.name);
        s.bind_text(3, session.version);
        s.bind_int(4, static_cast<int64_t>(session.expected_size));
        s.bind_text(5, session.expected_digest);
        s.bind_int(6, static_cast<int64_t>(session.chunk_size));
        s.bind_text(7, session.owner);
        s.bind_int(8, now);
        s.step();
        artifact_id = sqlite3_last_insert_rowid(impl.db.handle());
    }
    for (const ManifestEntry &entry : manifest) {
        Statement reactivate(impl.db,
                             "UPDATE chunk SET state='ACTIVE', pending_at=NULL "
                             "WHERE digest=?1 AND state='PENDING_DELETE'");
        reactivate.bind_text(1, entry.digest);
        reactivate.step();

        Statement s(impl.db, "INSERT INTO artifact_chunk (artifact_id, ordinal, byte_offset, "
                             "length, digest) VALUES (?1,?2,?3,?4,?5)");
        s.bind_int(1, artifact_id);
        s.bind_int(2, entry.ordinal);
        s.bind_int(3, static_cast<int64_t>(entry.offset));
        s.bind_int(4, static_cast<int64_t>(entry.length));
        s.bind_text(5, entry.digest);
        s.step();
    }
    {
        Statement s(impl.db, "UPDATE upload_session SET state='COMPLETED' WHERE id=?1");
        s.bind_text(1, session_id);
        s.step();
    }
    tx.commit();

    ArtifactInfo info;
    info.ns = session.ns;
    info.name = session.name;
    info.version = session.version;
    info.size = session.expected_size;
    info.digest = session.expected_digest;
    info.chunk_size = session.chunk_size;
    info.creator = session.owner;
    info.created_at = now;
    info.chunks = std::move(manifest);
    return info;
}

void Storage::abort_session(const std::string &session_id)
{
    Impl &impl = *impl_;
    const int64_t now = impl.cfg.clock();
    SessionInfo session = impl.load_session(session_id);
    if (session.state == SessionState::ABORTED || session.state == SessionState::EXPIRED ||
        session.state == SessionState::COMPLETED) {
        return; // 幂等（LIFE-02）。
    }
    Tx tx(impl.db);
    (void)impl.transition(session_id, session.state, SessionState::ABORTED);
    impl.mark_unreferenced_parts(session_id, now);
    tx.commit();
}

std::vector<SessionInfo> Storage::list_sessions(const std::optional<SessionState> &state)
{
    Impl &impl = *impl_;
    std::vector<SessionInfo> out;
    if (state.has_value()) {
        Statement s(impl.db, std::string("SELECT ") + kSessionColumns +
                                 " FROM upload_session WHERE state=?1 ORDER BY created_at DESC");
        s.bind_text(1, state_to_text(*state));
        while (s.step()) {
            out.push_back(read_session_row(s));
        }
    } else {
        Statement s(impl.db, std::string("SELECT ") + kSessionColumns +
                                 " FROM upload_session ORDER BY created_at DESC");
        while (s.step()) {
            out.push_back(read_session_row(s));
        }
    }
    return out;
}

// ---- 制品 ----------------------------------------------------------------

ArtifactInfo Storage::show_artifact(const std::string &ns, const std::string &name,
                                    const std::string &version)
{
    Impl &impl = *impl_;
    ArtifactInfo info;
    {
        Statement s(impl.db, "SELECT namespace, name, version, size, digest, chunk_size, creator, "
                             "created_at, note FROM artifact WHERE namespace=?1 AND name=?2 AND "
                             "version=?3");
        s.bind_text(1, ns);
        s.bind_text(2, name);
        s.bind_text(3, version);
        if (!s.step()) {
            throw_error(FR_E_NOTFOUND, "artifact not found");
        }
        info.ns = s.column_text(0);
        info.name = s.column_text(1);
        info.version = s.column_text(2);
        info.size = static_cast<uint64_t>(s.column_int(3));
        info.digest = s.column_text(4);
        info.chunk_size = static_cast<uint64_t>(s.column_int(5));
        info.creator = s.column_text(6);
        info.created_at = s.column_int(7);
        info.note = s.column_text(8);
    }
    {
        Statement s(impl.db, "SELECT ordinal, byte_offset, length, digest FROM artifact_chunk "
                             "WHERE artifact_id=(SELECT id FROM artifact WHERE namespace=?1 AND "
                             "name=?2 AND version=?3) ORDER BY ordinal");
        s.bind_text(1, ns);
        s.bind_text(2, name);
        s.bind_text(3, version);
        while (s.step()) {
            ManifestEntry entry;
            entry.ordinal = s.column_int(0);
            entry.offset = static_cast<uint64_t>(s.column_int(1));
            entry.length = static_cast<uint64_t>(s.column_int(2));
            entry.digest = s.column_text(3);
            info.chunks.push_back(std::move(entry));
        }
    }
    return info;
}

void Storage::read_artifact(const std::string &ns, const std::string &name,
                            const std::string &version, uint64_t offset, uint64_t length,
                            const Sink &sink)
{
    if (!sink) {
        throw_error(FR_E_ARG, "read_artifact requires a sink");
    }
    const ArtifactInfo info = show_artifact(ns, name, version);
    if (offset > info.size) {
        throw_error(FR_E_RANGE, "read offset beyond artifact size");
    }
    uint64_t remaining = length;
    if (remaining > info.size - offset) {
        remaining = info.size - offset; // 截断到文件尾（FR-DL-04 边界语义）。
    }

    Impl &impl = *impl_;
    fr_buf buffer;
    fr_buf_init(&buffer);
    try {
        for (const ManifestEntry &entry : info.chunks) {
            if (remaining == 0) {
                break;
            }
            const uint64_t chunk_end = entry.offset + entry.length;
            if (offset >= chunk_end) {
                continue; // 块整体在窗口之前。
            }
            if (entry.offset >= offset + remaining) {
                break; // 块整体在窗口之后。
            }
            const uint64_t local_offset = offset > entry.offset ? offset - entry.offset : 0;
            uint64_t take = chunk_end - (offset > entry.offset ? offset : entry.offset);
            if (take > remaining) {
                take = remaining;
            }
            fr_buf_clear(&buffer);
            impl.chunks.get_range(entry.digest, local_offset, static_cast<size_t>(take), &buffer);
            sink(buffer.data, buffer.len);
            remaining -= take;
            offset += take;
        }
    } catch (...) {
        fr_buf_destroy(&buffer);
        throw;
    }
    fr_buf_destroy(&buffer);
}

std::vector<ArtifactInfo> Storage::list_artifacts(const std::optional<std::string> &ns,
                                                  const std::optional<std::string> &name,
                                                  int64_t limit, int64_t offset)
{
    Impl &impl = *impl_;
    if (limit < 0 || offset < 0) {
        throw_error(FR_E_ARG, "pagination must be non-negative");
    }
    std::string sql = "SELECT namespace, name, version, size, digest, chunk_size, creator, "
                      "created_at, note FROM artifact";
    if (ns.has_value()) {
        sql += " WHERE namespace=?1";
        if (name.has_value()) {
            sql += " AND name=?2";
        }
    } else if (name.has_value()) {
        sql += " WHERE name=?1";
    }
    sql += " ORDER BY created_at DESC, id DESC LIMIT ?9 OFFSET ?10";

    std::vector<ArtifactInfo> out;
    Statement s(impl.db, sql);
    int index = 1;
    if (ns.has_value()) {
        s.bind_text(index++, *ns);
    }
    if (name.has_value()) {
        s.bind_text(index++, *name);
    }
    s.bind_int(9, limit);
    s.bind_int(10, offset);
    while (s.step()) {
        ArtifactInfo info;
        info.ns = s.column_text(0);
        info.name = s.column_text(1);
        info.version = s.column_text(2);
        info.size = static_cast<uint64_t>(s.column_int(3));
        info.digest = s.column_text(4);
        info.chunk_size = static_cast<uint64_t>(s.column_int(5));
        info.creator = s.column_text(6);
        info.created_at = s.column_int(7);
        info.note = s.column_text(8);
        out.push_back(std::move(info));
    }
    return out;
}

int64_t Storage::delete_artifact(const std::string &ns, const std::string &name,
                                 const std::string &version)
{
    Impl &impl = *impl_;
    const int64_t now = impl.cfg.clock();
    std::vector<std::string> digests;
    Tx tx(impl.db);
    {
        Statement s(impl.db, "SELECT id FROM artifact WHERE namespace=?1 AND name=?2 AND version=?3");
        s.bind_text(1, ns);
        s.bind_text(2, name);
        s.bind_text(3, version);
        if (!s.step()) {
            throw_error(FR_E_NOTFOUND, "artifact not found");
        }
        const int64_t artifact_id = s.column_int(0);

        Statement manifest(impl.db,
                           "SELECT digest FROM artifact_chunk WHERE artifact_id=?1 ORDER BY ordinal");
        manifest.bind_int(1, artifact_id);
        while (manifest.step()) {
            digests.push_back(manifest.column_text(0));
        }

        /* 先删可见元数据（级联删除清单），FR-GC-01。 */
        Statement del(impl.db, "DELETE FROM artifact WHERE id=?1");
        del.bind_int(1, artifact_id);
        del.step();
    }
    int64_t marked = 0;
    for (const std::string &digest : digests) {
        if (impl.chunk_referenced(digest)) {
            continue;
        }
        Statement mark(impl.db, "UPDATE chunk SET state='PENDING_DELETE', pending_at=?1 "
                                "WHERE digest=?2 AND state='ACTIVE'");
        mark.bind_int(1, now);
        mark.bind_text(2, digest);
        mark.step();
        marked += impl.db_changes();
    }
    tx.commit();
    return marked;
}

// ---- 维护 ----------------------------------------------------------------

GcReport Storage::collect_garbage(bool dry_run)
{
    Impl &impl = *impl_;
    const int64_t now = impl.cfg.clock();
    GcReport report;
    struct Candidate {
        std::string digest;
        int64_t raw_size;
        int64_t pending_at;
    };
    std::vector<Candidate> candidates;
    {
        Statement s(impl.db, "SELECT digest, raw_size, pending_at FROM chunk "
                             "WHERE state='PENDING_DELETE' AND pending_at IS NOT NULL");
        while (s.step()) {
            candidates.push_back(
                Candidate{s.column_text(0), s.column_int(1), s.column_int(2)});
        }
    }
    report.scanned = static_cast<int64_t>(candidates.size());
    for (const Candidate &candidate : candidates) {
        if (candidate.pending_at > now - static_cast<int64_t>(impl.cfg.gc_grace_seconds)) {
            continue; // 保护期未过（FR-GC-02），仅计入 scanned。
        }
        /* 双重校验：标记之后又被引用的块跳过并重新激活。 */
        if (impl.chunk_referenced(candidate.digest)) {
            Statement reactivate(impl.db,
                                 "UPDATE chunk SET state='ACTIVE', pending_at=NULL WHERE digest=?1");
            reactivate.bind_text(1, candidate.digest);
            reactivate.step();
            continue;
        }
        report.deleted_count += 1;
        report.deleted_bytes += candidate.raw_size;
        if (!dry_run) {
            impl.chunks.remove(candidate.digest);
            Statement del(impl.db, "DELETE FROM chunk WHERE digest=?1");
            del.bind_text(1, candidate.digest);
            del.step();
        }
    }
    return report;
}

RecoveryReport Storage::run_maintenance()
{
    Impl &impl = *impl_;
    const int64_t now = impl.cfg.clock();
    RecoveryReport report;

    report.temp_files_removed = impl.chunks.cleanup_temp_files();

    {
        Statement s(impl.db, "UPDATE upload_session SET state='EXPIRED' WHERE state='OPEN' "
                             "AND expires_at <= ?1");
        s.bind_int(1, now);
        s.step();
        report.sessions_expired = impl.db_changes();
    }
    {
        /* 回滚遗留的 COMMITTING（进程崩溃于发布事务之外的场景，FR-STO-06）。 */
        Statement s(impl.db, "UPDATE upload_session SET state='OPEN' WHERE state='COMMITTING'");
        s.step();
        report.sessions_rolled_back = impl.db_changes();
    }
    {
        /* 终态会话的分片登记不再需要：登记块标记待清理（其文件由 GC 删除）。 */
        Statement s(impl.db, "SELECT id FROM upload_session WHERE state IN ('EXPIRED','ABORTED')");
        std::vector<std::string> terminal;
        while (s.step()) {
            terminal.push_back(s.column_text(0));
        }
        for (const std::string &id : terminal) {
            impl.mark_unreferenced_parts(id, now);
        }
    }
    return report;
}

uint64_t Storage::used_bytes()
{
    Impl &impl = *impl_;
    Statement s(impl.db, "SELECT COALESCE(SUM(raw_size),0) FROM chunk WHERE state='ACTIVE'");
    if (s.step()) {
        return static_cast<uint64_t>(s.column_int(0));
    }
    return 0;
}

} // namespace fr
