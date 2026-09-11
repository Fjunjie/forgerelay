// fr_dispatch.cpp - 请求分发实现（FR-AUTH-04、§2.1 角色矩阵、FR-ADM-05 审计）。
#include "forgerelay/service.hpp"

#include <chrono>
#include <mutex>
#include <cstring>

#include "forgerelay/fr_version.h"
#include "forgerelay/log.hpp"
#include "forgerelay/storage/error.hpp"

namespace fr {

namespace {

constexpr size_t kDataSliceBytes = 1024ull * 1024; // 单个 DATA 分片 1 MiB（AC-06）
constexpr int kAuthFailuresBeforeClose = 5;        // 连接内连续失败阈值（FR-AUTH-05）
constexpr int kIpFailureWindowSeconds = 60;        // IP 失败统计窗口
constexpr int kIpFailureThreshold = 20;            // 窗口内失败阈值
constexpr int kIpBlockSeconds = 60;                // 封禁时长

int64_t now_seconds()
{
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
}

int64_t now_seconds_monotonic()
{
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}

std::string ref_to_string(const msg::ArtifactRef &ref)
{
    return ref.ns + "/" + ref.name + ":" + ref.version;
}

} // namespace

struct Dispatcher::Impl {
    Storage &storage;
    AuthRegistry &auth;
    const ServerSettings &cfg;
    Logger &log;
    std::shared_ptr<StatusHub> status_hub;

    std::mutex mu; // 串行化存储访问 + GET 续传状态 + IP 限流（M2 Storage 非线程安全）

    struct GetState {
        std::string ns;
        std::string name;
        std::string version;
        uint64_t offset = 0;
        uint64_t remaining = 0;
        uint64_t sent = 0;
        uint32_t req_id = 1; // DATA/最终 OK 帧回显请求 ID
    };
    std::map<std::string, GetState> gets;
    uint64_t get_seq = 0;

    struct IpStat {
        int failures = 0;
        int64_t window_start = 0;
        int64_t blocked_until = 0;
    };
    std::map<std::string, IpStat> ip_stats;

    Impl(Storage &storage_ref, AuthRegistry &auth_ref, const ServerSettings &settings,
         Logger &logger, std::shared_ptr<StatusHub> hub)
        : storage(storage_ref), auth(auth_ref), cfg(settings), log(logger),
          status_hub(std::move(hub))
    {
    }

    static int64_t monotonic_seconds()
    {
        return now_seconds_monotonic();
    }

    static std::string ip_of(const std::string &peer)
    {
        const size_t colon = peer.rfind(':');
        return colon == std::string::npos ? peer : peer.substr(0, colon);
    }

    void note_auth_failure(const std::string &peer)
    {
        const int64_t now = now_seconds();
        IpStat &stat = ip_stats[ip_of(peer)];
        if (stat.window_start == 0 || now - stat.window_start > kIpFailureWindowSeconds) {
            stat.window_start = now;
            stat.failures = 0;
        }
        stat.failures++;
        if (stat.failures >= kIpFailureThreshold) {
            stat.blocked_until = now + kIpBlockSeconds;
            stat.failures = 0;
            log.warn("peer blocked for repeated auth failures: " + peer);
        }
    }

    void check_peer_not_blocked(const std::string &peer)
    {
        auto it = ip_stats.find(ip_of(peer));
        if (it != ip_stats.end() && it->second.blocked_until > now_seconds()) {
            throw_error(FR_E_UNAUTHENTICATED, "too many auth failures from your address");
        }
    }

    /* 角色矩阵（§2.1）：Reader 只读；Publisher 上传+删本人制品；Admin 全部。 */
    static void require_role(const ClientSession &session, Role required)
    {
        if (!session.authenticated) {
            throw_error(FR_E_UNAUTHENTICATED, "authentication required");
        }
        if (session.role == Role::Admin) {
            return;
        }
        if (required == Role::Admin) {
            throw_error(FR_E_FORBIDDEN, "admin role required");
        }
        if (required == Role::Publisher && session.role == Role::Reader) {
            throw_error(FR_E_FORBIDDEN, "publisher role required");
        }
    }

    /** 制品删除权限：Admin 任意；Publisher 仅本人制品（§2.1）。 */
    void require_delete_allowed(const ClientSession &session, const std::string &ns,
                                const std::string &name, const std::string &version)
    {
        require_role(session, Role::Reader);
        if (session.role == Role::Admin) {
            return;
        }
        if (session.role != Role::Publisher) {
            throw_error(FR_E_FORBIDDEN, "publisher role required");
        }
        const ArtifactInfo info = storage.show_artifact(ns, name, version);
        if (info.creator != session.username) {
            throw_error(FR_E_FORBIDDEN, "only the creator may delete this artifact");
        }
    }

    /** 上传会话权限：Owner 或 Admin。 */
    void require_session_allowed(const ClientSession &session, const SessionInfo &info)
    {
        require_role(session, Role::Publisher);
        if (session.role != Role::Admin && info.owner != session.username) {
            throw_error(FR_E_FORBIDDEN, "not the owner of this session");
        }
    }
};

Dispatcher::Dispatcher(Storage &storage, AuthRegistry &auth, const ServerSettings &settings,
                       Logger &log, std::shared_ptr<StatusHub> status_hub)
    : impl_(new Impl(storage, auth, settings, log, std::move(status_hub)))
{
}

Dispatcher::~Dispatcher() = default;

bool Dispatcher::allow_peer(const std::string &peer)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->ip_stats.find(Impl::ip_of(peer));
    return it == impl_->ip_stats.end() || it->second.blocked_until <= now_seconds();
}

HandleResult Dispatcher::handle_get_continue(uint64_t conn_id, const std::string &peer,
                                             const std::string &state_key)
{
    (void)conn_id;
    (void)peer;
    Impl &impl = *impl_;
    HandleResult result;
    fr_buf_init(&result.out);

    std::lock_guard<std::mutex> lock(impl.mu);
    auto it = impl.gets.find(state_key);
    if (it == impl.gets.end()) {
        return result; // 连接/状态已消失，空结果
    }
    Impl::GetState &state = it->second;


    const uint64_t slice = state.remaining > kDataSliceBytes ? kDataSliceBytes : state.remaining;
    uint64_t sent = 0;
    if (slice != 0) {
        sent = impl.storage.read_artifact_slice(
            state.ns, state.name, state.version, state.offset, slice,
            [&](const void *data, size_t len) {
                msg::DataPayload payload;
                payload.data.resize(len);
                std::memcpy(payload.data.data(), data, len);
                fr_frame_encode(&result.out, FR_MSG_DATA, 0, state.req_id, payload.data.data(),
                                payload.data.size());
            });
    }
    state.offset += sent;
    state.remaining -= sent;
    state.sent += sent;

    if (state.remaining == 0 || sent == 0) {
        /* 流结束（数据读完或到达制品尾）：最终 OK 携带总字节数。 */
        fr_buf payload;
        fr_buf_init(&payload);
        msg::GetArtifactOk ok;
        ok.bytes_sent = state.sent;
        msg::encode_get_artifact_ok(payload, ok);
        fr_frame_encode(&result.out, FR_MSG_OK, 0, state.req_id, payload.data, payload.len);
        fr_buf_destroy(&payload);
        impl.gets.erase(it);
    } else {
        result.has_more = true;
        result.continuation_key = state_key; // 续传键随结果传递（否则链条断裂）
    }
    return result;
}

HandleResult Dispatcher::handle(uint64_t conn_id, const std::string &peer, ClientSession &session,
                                const fr_frame &frame)
{
    Impl &impl = *impl_;
    HandleResult result;
    fr_buf_init(&result.out);

    const std::string req_id = std::to_string(frame.req_id);
    /* 分发器直接产出完整帧（传输层原样落盘）。
     * req_id == 0 的帧在解析层已被拒绝；此处兜底避免编码器拒收。 */
    const uint32_t out_req_id = frame.req_id == 0 ? 1u : frame.req_id;
    auto make_ok = [&](const void *payload, size_t len) {
        fr_frame_encode(&result.out, FR_MSG_OK, 0, out_req_id, payload, len);
    };
    auto ok_empty = [&](const void *, size_t) {
        fr_frame_encode(&result.out, FR_MSG_OK, 0, out_req_id, nullptr, 0);
    };

    try {
        switch (frame.type) {
        case FR_MSG_HELLO: {
            const msg::HelloReq req = msg::decode_hello_req(frame);
            (void)req;
            session.hello_done = true;
            fr_buf payload;
            fr_buf_init(&payload);
            msg::put_str(payload, std::string("forgerelayd ") + fr_core_version_string());
            make_ok(payload.data, payload.len);
            fr_buf_destroy(&payload);
            break;
        }
        case FR_MSG_AUTH: {
            {
                std::lock_guard<std::mutex> lock(impl.mu);
                impl.check_peer_not_blocked(peer);
            }
            const msg::AuthReq req = msg::decode_auth_req(frame);
            const msg::AuthOk ok = impl.auth.verify(req.token);
            session.authenticated = true;
            session.username = ok.username;
            session.role = static_cast<Role>(ok.role);
            session.auth_failures = 0;
            fr_buf payload;
            fr_buf_init(&payload);
            msg::encode_auth_ok(payload, ok);
            make_ok(payload.data, payload.len);
            fr_buf_destroy(&payload);
            impl.log.info("auth ok: " + ok.username, req_id);
            break;
        }
        case FR_MSG_PING: {
            (void)msg::decode_data(frame);
            ok_empty(nullptr, 0);
            break;
        }
        case FR_MSG_CLOSE: {
            ok_empty(nullptr, 0);
            result.close_connection = true;
            break;
        }
        default: {
            /* 业务消息一律先鉴权（FR-AUTH-04）。 */
            if (!session.authenticated) {
                throw_error(FR_E_UNAUTHENTICATED, "authentication required");
            }
            dispatch_business(conn_id, session, frame, result, make_ok, ok_empty);
            break;
        }
        }
    } catch (const fr::Error &err) {
        if (err.code() == FR_E_UNAUTHENTICATED && frame.type == FR_MSG_AUTH) {
            session.auth_failures++;
            {
                std::lock_guard<std::mutex> lock(impl.mu);
                impl.note_auth_failure(peer);
            }
            if (session.auth_failures >= kAuthFailuresBeforeClose) {
                result.close_connection = true; // FR-AUTH-05 连接级限流
            }
            impl.log.warn("auth failed: " + std::string(err.what()), req_id);
        } else {
            impl.log.warn("request failed: " + std::string(err.what()), req_id);
        }
        fr_buf_clear(&result.out);
        fr_buf err_payload;
        fr_buf_init(&err_payload);
        msg::encode_error_payload(err_payload, err.code(), err.what());
        fr_frame_encode(&result.out, FR_MSG_ERROR, 0, out_req_id, err_payload.data,
                        err_payload.len);
        fr_buf_destroy(&err_payload);
    }
    return result;
}

void Dispatcher::dispatch_business(uint64_t conn_id, ClientSession &session, const fr_frame &frame,
                                   HandleResult &result, const MakeOk &make_ok,
                                   const MakeOk &ok_empty)
{
    Impl &impl = *impl_;
    const std::string req_id = std::to_string(frame.req_id);

    switch (frame.type) {
    case FR_MSG_CREATE_UPLOAD: {
        Impl::require_role(session, Role::Publisher);
        const msg::CreateUploadReq req = msg::decode_create_upload_req(frame);
        SessionInfo created;
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            created = impl.storage.create_session(req.ref.ns, req.ref.name, req.ref.version,
                                                  req.expected_size, req.expected_digest,
                                                  session.username);
        }
        fr_buf payload;
        fr_buf_init(&payload);
        msg::CreateUploadOk ok;
        ok.session_id = created.id;
        ok.chunk_size = created.chunk_size;
        ok.expires_at = created.expires_at;
        msg::encode_create_upload_ok(payload, ok);
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        break;
    }
    case FR_MSG_PUT_CHUNK: {
        const msg::PutChunkReq req = msg::decode_put_chunk_req(frame);
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            const SessionInfo info = impl.storage.get_session(req.session_id);
            impl.require_session_allowed(session, info);
        }
        ChunkAccept accept;
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            accept = impl.storage.put_chunk(req.session_id, req.ordinal, req.data.data(),
                                            req.data.size());
        }
        fr_buf payload;
        fr_buf_init(&payload);
        msg::PutChunkOk ok;
        ok.digest = accept.digest;
        ok.reused = accept.reused;
        msg::encode_put_chunk_ok(payload, ok);
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        impl.log.debug("chunk accepted: " + accept.digest.substr(0, 12), req_id); // LOG-03
        break;
    }
    case FR_MSG_QUERY_UPLOAD: {
        fr_buf_reader in;
        fr_buf_reader_init(&in, frame.payload, frame.payload_len);
        const std::string session_id = msg::get_str(in);
        std::lock_guard<std::mutex> lock(impl.mu);
        const SessionInfo info = impl.storage.get_session(session_id);
        impl.require_session_allowed(session, info);
        const Storage::SessionProgress progress = impl.storage.session_progress(session_id);
        msg::QueryUploadOk ok;
        ok.state = info.state;
        ok.chunk_count = progress.chunk_count;
        ok.present_bitmap.assign((progress.chunk_count + 7) / 8, 0);
        for (uint64_t ordinal : progress.present_ordinals) {
            ok.present_bitmap[ordinal / 8] |= static_cast<uint8_t>(1u << (ordinal % 8));
        }
        fr_buf payload;
        fr_buf_init(&payload);
        msg::encode_query_upload_ok(payload, ok);
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        break;
    }
    case FR_MSG_COMMIT_UPLOAD: {
        fr_buf_reader in;
        fr_buf_reader_init(&in, frame.payload, frame.payload_len);
        const std::string session_id = msg::get_str(in);
        fr_buf payload;
        fr_buf_init(&payload);
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            const SessionInfo info = impl.storage.get_session(session_id);
            impl.require_session_allowed(session, info);
            const ArtifactInfo published = impl.storage.commit_session(session_id);
            msg::CommitUploadOk ok;
            ok.ref.ns = published.ns;
            ok.ref.name = published.name;
            ok.ref.version = published.version;
            ok.size = published.size;
            ok.digest = published.digest;
            msg::encode_commit_upload_ok(payload, ok);
        }
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        break;
    }
    case FR_MSG_ABORT_UPLOAD: {
        fr_buf_reader in;
        fr_buf_reader_init(&in, frame.payload, frame.payload_len);
        const std::string session_id = msg::get_str(in);
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            const SessionInfo info = impl.storage.get_session(session_id);
            impl.require_session_allowed(session, info);
            impl.storage.abort_session(session_id);
            impl.auth.audit(session.username, "session.abort", session_id, "ok");
        }
        ok_empty(nullptr, 0);
        break;
    }
    case FR_MSG_GET_ARTIFACT: {
        Impl::require_role(session, Role::Reader);
        const msg::GetArtifactReq req = msg::decode_get_artifact_req(frame);
        std::string key;
        bool remaining_zero = false;
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            key = "get-" + std::to_string(conn_id) + "-" + std::to_string(impl.get_seq++);
            Impl::GetState state;
            state.ns = req.ref.ns;
            state.name = req.ref.name;
            state.version = req.ref.version;
            state.offset = req.offset;
            state.remaining = req.length;
            state.req_id = frame.req_id == 0 ? 1u : frame.req_id;
            auto emplaced = impl.gets.emplace(key, std::move(state));
            Impl::GetState &state_ref = emplaced.first->second;
            const uint64_t slice =
                state_ref.remaining > kDataSliceBytes ? kDataSliceBytes : state_ref.remaining;
            if (slice != 0) {
                const uint64_t got = impl.storage.read_artifact_slice(
                    state_ref.ns, state_ref.name, state_ref.version, state_ref.offset, slice,
                    [&](const void *data, size_t len) {
                        msg::DataPayload payload;
                        payload.data.resize(len);
                        std::memcpy(payload.data.data(), data, len);
                        fr_frame_encode(&result.out, FR_MSG_DATA, 0, state_ref.req_id,
                                        payload.data.data(), payload.data.size());
                    });
                state_ref.offset += got;
                state_ref.remaining -= got;
                state_ref.sent += got;
            }
            remaining_zero = state_ref.remaining == 0;
            if (remaining_zero) {
                fr_buf payload;
                fr_buf_init(&payload);
                msg::GetArtifactOk ok;
                ok.bytes_sent = state_ref.sent;
                msg::encode_get_artifact_ok(payload, ok);
                make_ok(payload.data, payload.len);
                fr_buf_destroy(&payload);
                impl.gets.erase(key);
            }
        }
        if (!remaining_zero) {
            result.has_more = true;
            result.continuation_key = key;
        }
        break;
    }
    case FR_MSG_LIST_ARTIFACTS: {
        Impl::require_role(session, Role::Reader);
        const msg::ListArtifactsReq req = msg::decode_list_artifacts_req(frame);
        msg::ListArtifactsOk ok;
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            const std::vector<ArtifactInfo> items =
                impl.storage.list_artifacts(req.ns, req.name, static_cast<int64_t>(req.limit),
                                            static_cast<int64_t>(req.offset));
            ok.items.reserve(items.size());
            for (const ArtifactInfo &info : items) {
                msg::ArtifactSummary item;
                item.ns = info.ns;
                item.name = info.name;
                item.version = info.version;
                item.size = info.size;
                item.digest = info.digest;
                item.creator = info.creator;
                item.created_at = info.created_at;
                ok.items.push_back(std::move(item));
            }
        }
        fr_buf payload;
        fr_buf_init(&payload);
        msg::encode_list_artifacts_ok(payload, ok);
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        break;
    }
    case FR_MSG_SHOW_ARTIFACT: {
        Impl::require_role(session, Role::Reader);
        const msg::ArtifactRef ref = msg::decode_ref(frame);
        msg::ShowArtifactOk ok;
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            ok.info = impl.storage.show_artifact(ref.ns, ref.name, ref.version);
        }
        fr_buf payload;
        fr_buf_init(&payload);
        msg::encode_show_artifact_ok(payload, ok);
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        break;
    }
    case FR_MSG_STATUS: {
        Impl::require_role(session, Role::Reader);
        msg::StatusOk ok;
        ok.version = std::string("forgerelayd ") + fr_core_version_string();
        if (impl.status_hub) {
            ok.connections = impl.status_hub->connections.load();
            const int64_t start =
                static_cast<int64_t>(impl.status_hub->start_steady_seconds.load());
            ok.uptime_seconds =
                start == 0 ? 0 : static_cast<uint64_t>(now_seconds_monotonic() - start);
        }
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            ok.artifacts = static_cast<uint64_t>(impl.storage.artifact_count());
            ok.used_bytes = impl.storage.used_bytes();
        }
        ok.active_sessions = static_cast<uint32_t>(impl.auth.active_session_count());
        ok.capacity_bytes = impl.cfg.storage.capacity_bytes;
        ok.high_watermark_percent =
            static_cast<uint8_t>(impl.cfg.storage.high_watermark_percent);
        fr_buf payload;
        fr_buf_init(&payload);
        msg::encode_status_ok(payload, ok);
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        break;
    }
    case FR_MSG_DELETE_ARTIFACT: {
        const msg::ArtifactRef ref = msg::decode_ref(frame);
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            impl.require_delete_allowed(session, ref.ns, ref.name, ref.version);
        }
        int64_t marked = 0;
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            marked = impl.storage.delete_artifact(ref.ns, ref.name, ref.version);
        }
        impl.auth.audit(session.username, "artifact.delete", ref_to_string(ref), "ok");
        fr_buf payload;
        fr_buf_init(&payload);
        msg::DeleteArtifactOk ok;
        ok.marked_chunks = marked;
        msg::encode_delete_artifact_ok(payload, ok);
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        break;
    }
    case FR_MSG_RUN_GC: {
        Impl::require_role(session, Role::Admin);
        const msg::RunGcReq req = msg::decode_run_gc_req(frame);
        msg::RunGcOk ok;
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            const GcReport report = impl.storage.collect_garbage(req.dry_run);
            ok.scanned = report.scanned;
            ok.deleted_count = report.deleted_count;
            ok.deleted_bytes = report.deleted_bytes;
        }
        impl.auth.audit(session.username, req.dry_run ? "gc.dry_run" : "gc.run", "storage", "ok");
        fr_buf payload;
        fr_buf_init(&payload);
        msg::encode_run_gc_ok(payload, ok);
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        break;
    }
    case FR_MSG_USER_ADD: {
        Impl::require_role(session, Role::Admin);
        const msg::UserAddReq req = msg::decode_user_add_req(frame);
        impl.auth.user_add(req.username, static_cast<Role>(req.role));
        impl.auth.audit(session.username, "user.add", req.username, "ok");
        ok_empty(nullptr, 0);
        break;
    }
    case FR_MSG_USER_DISABLE: {
        Impl::require_role(session, Role::Admin);
        fr_buf_reader in;
        fr_buf_reader_init(&in, frame.payload, frame.payload_len);
        const std::string username = msg::get_str(in);
        impl.auth.user_disable(username);
        impl.auth.audit(session.username, "user.disable", username, "ok");
        ok_empty(nullptr, 0);
        break;
    }
    case FR_MSG_USER_LIST: {
        Impl::require_role(session, Role::Admin);
        msg::UserListOk ok;
        ok.items = impl.auth.user_list();
        fr_buf payload;
        fr_buf_init(&payload);
        msg::encode_user_list_ok(payload, ok);
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        break;
    }
    case FR_MSG_TOKEN_CREATE: {
        Impl::require_role(session, Role::Admin);
        const msg::TokenCreateReq req = msg::decode_token_create_req(frame);
        msg::TokenCreateOk ok;
        ok.token = impl.auth.token_create(req.username, req.ttl_hours);
        impl.auth.audit(session.username, "token.create", req.username, "ok");
        fr_buf payload;
        fr_buf_init(&payload);
        msg::encode_token_create_ok(payload, ok);
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        break;
    }
    case FR_MSG_TOKEN_REVOKE: {
        Impl::require_role(session, Role::Admin);
        fr_buf_reader in;
        fr_buf_reader_init(&in, frame.payload, frame.payload_len);
        const uint64_t token_id = msg::get_u64(in);
        impl.auth.token_revoke(token_id);
        impl.auth.audit(session.username, "token.revoke", std::to_string(token_id), "ok");
        ok_empty(nullptr, 0);
        break;
    }
    case FR_MSG_TOKEN_LIST: {
        Impl::require_role(session, Role::Admin);
        fr_buf_reader in;
        fr_buf_reader_init(&in, frame.payload, frame.payload_len);
        const std::string username = msg::get_str(in);
        msg::TokenListOk ok;
        ok.items = impl.auth.token_list(username);
        fr_buf payload;
        fr_buf_init(&payload);
        msg::encode_token_list_ok(payload, ok);
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        break;
    }
    case FR_MSG_SESSION_LIST: {
        Impl::require_role(session, Role::Admin);
        msg::SessionListOk ok;
        {
            std::lock_guard<std::mutex> lock(impl.mu);
            const std::vector<SessionInfo> sessions =
                impl.storage.list_sessions(std::nullopt);
            ok.items.reserve(sessions.size());
            for (const SessionInfo &info : sessions) {
                msg::SessionItem item;
                item.session_id = info.id;
                item.owner = info.owner;
                item.state = info.state;
                item.ns = info.ns;
                item.name = info.name;
                item.version = info.version;
                item.expected_size = info.expected_size;
                item.created_at = info.created_at;
                ok.items.push_back(std::move(item));
            }
        }
        fr_buf payload;
        fr_buf_init(&payload);
        msg::encode_session_list_ok(payload, ok);
        make_ok(payload.data, payload.len);
        fr_buf_destroy(&payload);
        break;
    }
    case FR_MSG_SESSION_ABORT: {
        Impl::require_role(session, Role::Admin);
        fr_buf_reader in;
        fr_buf_reader_init(&in, frame.payload, frame.payload_len);
        const std::string session_id = msg::get_str(in);
        impl.storage.abort_session(session_id);
        impl.auth.audit(session.username, "session.abort", session_id, "ok");
        ok_empty(nullptr, 0);
        break;
    }
    default:
        throw_error(FR_E_PROTOCOL, "unhandled message type");
    }
}

} // namespace fr
