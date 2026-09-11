// fr_messages.cpp - 协议消息负载编解码实现。
#include "forgerelay/messages.hpp"

#include "forgerelay/fr_version.h"
#include "forgerelay/storage/error.hpp"


namespace fr::msg {

namespace {

constexpr size_t kMaxStringLen = 512;
constexpr size_t kDigestHexLen = 64;

[[noreturn]] void protocol_error(const char *what)
{
    throw_error(FR_E_PROTOCOL, std::string("payload: ") + what);
}

fr_buf_reader reader_of(const fr_frame &frame)
{
    fr_buf_reader in;
    fr_buf_reader_init(&in, frame.payload, frame.payload_len);
    return in;
}

} // namespace

void put_str(fr_buf &out, const std::string &value)
{
    if (value.size() > kMaxStringLen) {
        throw_error(FR_E_ARG, "string field exceeds protocol limit");
    }
    fr_buf_append_u16be(&out, static_cast<uint16_t>(value.size()));
    fr_buf_append(&out, value.data(), value.size());
}

std::string get_str(fr_buf_reader &in)
{
    uint16_t len = 0;
    if (fr_buf_reader_u16be(&in, &len) != FR_OK || len > kMaxStringLen) {
        protocol_error("string length invalid");
    }
    std::string value(len, '\0');
    if (len != 0 && fr_buf_reader_read(&in, value.data(), len) != FR_OK) {
        protocol_error("string truncated");
    }
    return value;
}

void put_u8(fr_buf &out, uint8_t value)
{
    fr_buf_append_u8(&out, value);
}

uint8_t get_u8(fr_buf_reader &in)
{
    uint8_t value = 0;
    if (fr_buf_reader_u8(&in, &value) != FR_OK) {
        protocol_error("u8 truncated");
    }
    return value;
}

void put_i32(fr_buf &out, int32_t value)
{
    fr_buf_append_u32be(&out, static_cast<uint32_t>(value));
}

int32_t get_i32(fr_buf_reader &in)
{
    return static_cast<int32_t>(get_u32(in));
}

void put_u32(fr_buf &out, uint32_t value)
{
    fr_buf_append_u32be(&out, value);
}

uint32_t get_u32(fr_buf_reader &in)
{
    uint32_t value = 0;
    if (fr_buf_reader_u32be(&in, &value) != FR_OK) {
        protocol_error("u32 truncated");
    }
    return value;
}

void put_u64(fr_buf &out, uint64_t value)
{
    fr_buf_append_u64be(&out, value);
}

uint64_t get_u64(fr_buf_reader &in)
{
    uint64_t value = 0;
    if (fr_buf_reader_u64be(&in, &value) != FR_OK) {
        protocol_error("u64 truncated");
    }
    return value;
}

void put_i64(fr_buf &out, int64_t value)
{
    put_u64(out, static_cast<uint64_t>(value));
}

int64_t get_i64(fr_buf_reader &in)
{
    return static_cast<int64_t>(get_u64(in));
}

void put_digest(fr_buf &out, const std::string &hex)
{
    if (hex.size() != kDigestHexLen) {
        throw_error(FR_E_ARG, "digest must be 64 hex chars");
    }
    fr_buf_append(&out, hex.data(), kDigestHexLen);
}

std::string get_digest(fr_buf_reader &in)
{
    std::string value(kDigestHexLen, '\0');
    if (fr_buf_reader_read(&in, value.data(), kDigestHexLen) != FR_OK) {
        protocol_error("digest truncated");
    }
    return value;
}

std::vector<uint8_t> get_rest(fr_buf_reader &in)
{
    std::vector<uint8_t> out;
    const size_t remaining = fr_buf_reader_remaining(&in);
    out.resize(remaining);
    if (remaining != 0 && fr_buf_reader_read(&in, out.data(), remaining) != FR_OK) {
        protocol_error("rest truncated");
    }
    return out;
}

void put_ref(fr_buf &out, const ArtifactRef &ref)
{
    put_str(out, ref.ns);
    put_str(out, ref.name);
    put_str(out, ref.version);
}

ArtifactRef get_ref(fr_buf_reader &in)
{
    ArtifactRef ref;
    ref.ns = get_str(in);
    ref.name = get_str(in);
    ref.version = get_str(in);
    return ref;
}

void encode_hello_req(fr_buf &out, const HelloReq &req)
{
    put_u8(out, req.protocol_version);
    put_str(out, req.client);
}

HelloReq decode_hello_req(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    HelloReq req;
    req.protocol_version = get_u8(in);
    if (req.protocol_version != FR_PROTOCOL_VERSION) {
        protocol_error("unsupported protocol version");
    }
    req.client = get_str(in);
    return req;
}

void encode_auth_req(fr_buf &out, const AuthReq &req)
{
    put_str(out, req.token);
}

AuthReq decode_auth_req(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    AuthReq req;
    req.token = get_str(in);
    return req;
}

void encode_auth_ok(fr_buf &out, const AuthOk &ok)
{
    put_str(out, ok.username);
    put_u8(out, ok.role);
}

AuthOk decode_auth_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    AuthOk ok;
    ok.username = get_str(in);
    ok.role = get_u8(in);
    return ok;
}

void encode_create_upload_req(fr_buf &out, const CreateUploadReq &req)
{
    put_ref(out, req.ref);
    put_u64(out, req.expected_size);
    put_digest(out, req.expected_digest);
}

CreateUploadReq decode_create_upload_req(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    CreateUploadReq req;
    req.ref = get_ref(in);
    req.expected_size = get_u64(in);
    req.expected_digest = get_digest(in);
    return req;
}

void encode_create_upload_ok(fr_buf &out, const CreateUploadOk &ok)
{
    put_str(out, ok.session_id);
    put_u64(out, ok.chunk_size);
    put_i64(out, ok.expires_at);
}

CreateUploadOk decode_create_upload_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    CreateUploadOk ok;
    ok.session_id = get_str(in);
    ok.chunk_size = get_u64(in);
    ok.expires_at = get_i64(in);
    return ok;
}

void encode_put_chunk_req(fr_buf &out, const PutChunkReq &req)
{
    put_str(out, req.session_id);
    put_u64(out, req.ordinal);
    fr_buf_append(&out, req.data.data(), req.data.size());
}

PutChunkReq decode_put_chunk_req(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    PutChunkReq req;
    req.session_id = get_str(in);
    req.ordinal = get_u64(in);
    req.data = get_rest(in);
    return req;
}

void encode_put_chunk_ok(fr_buf &out, const PutChunkOk &ok)
{
    put_digest(out, ok.digest);
    put_u8(out, ok.reused ? 1 : 0);
}

PutChunkOk decode_put_chunk_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    PutChunkOk ok;
    ok.digest = get_digest(in);
    ok.reused = get_u8(in) != 0;
    return ok;
}

void encode_query_upload_ok(fr_buf &out, const QueryUploadOk &ok)
{
    put_u8(out, static_cast<uint8_t>(ok.state));
    put_u64(out, ok.chunk_count);
    /* 编码端对称防御（审计 #5，CWE-197）：位图长度必须可由 u32 表达，
     * 且不超过解码端 1 MiB 上限，防止截断导致协议错位。 */
    if (ok.present_bitmap.size() > static_cast<size_t>(1024 * 1024)) {
        throw_error(FR_E_RANGE, "present bitmap exceeds 1 MiB protocol limit");
    }
    fr_buf_append_u32be(&out, static_cast<uint32_t>(ok.present_bitmap.size()));
    fr_buf_append(&out, ok.present_bitmap.data(), ok.present_bitmap.size());
}

QueryUploadOk decode_query_upload_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    QueryUploadOk ok;
    ok.state = static_cast<SessionState>(get_u8(in));
    ok.chunk_count = get_u64(in);
    const uint32_t bitmap_len = get_u32(in);
    if (bitmap_len > 1024 * 1024) {
        protocol_error("bitmap too large");
    }
    ok.present_bitmap.resize(bitmap_len);
    if (bitmap_len != 0 && fr_buf_reader_read(&in, ok.present_bitmap.data(), bitmap_len) != FR_OK) {
        protocol_error("bitmap truncated");
    }
    return ok;
}

void encode_commit_upload_ok(fr_buf &out, const CommitUploadOk &ok)
{
    put_ref(out, ok.ref);
    put_u64(out, ok.size);
    put_digest(out, ok.digest);
}

CommitUploadOk decode_commit_upload_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    CommitUploadOk ok;
    ok.ref = get_ref(in);
    ok.size = get_u64(in);
    ok.digest = get_digest(in);
    return ok;
}

void encode_get_artifact_req(fr_buf &out, const GetArtifactReq &req)
{
    put_ref(out, req.ref);
    put_u64(out, req.offset);
    put_u64(out, req.length);
}

GetArtifactReq decode_get_artifact_req(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    GetArtifactReq req;
    req.ref = get_ref(in);
    req.offset = get_u64(in);
    req.length = get_u64(in);
    return req;
}

void encode_data(fr_buf &out, const DataPayload &payload)
{
    fr_buf_append(&out, payload.data.data(), payload.data.size());
}

DataPayload decode_data(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    DataPayload payload;
    payload.data = get_rest(in);
    return payload;
}

void encode_get_artifact_ok(fr_buf &out, const GetArtifactOk &ok)
{
    put_u64(out, ok.bytes_sent);
}

GetArtifactOk decode_get_artifact_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    GetArtifactOk ok;
    ok.bytes_sent = get_u64(in);
    return ok;
}

void encode_list_artifacts_req(fr_buf &out, const ListArtifactsReq &req)
{
    put_u8(out, req.ns.has_value() ? 1 : 0);
    if (req.ns.has_value()) {
        put_str(out, *req.ns);
    }
    put_u8(out, req.name.has_value() ? 1 : 0);
    if (req.name.has_value()) {
        put_str(out, *req.name);
    }
    put_u32(out, req.limit);
    put_u32(out, req.offset);
}

ListArtifactsReq decode_list_artifacts_req(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    ListArtifactsReq req;
    if (get_u8(in) != 0) {
        req.ns = get_str(in);
    }
    if (get_u8(in) != 0) {
        req.name = get_str(in);
    }
    req.limit = get_u32(in);
    req.offset = get_u32(in);
    return req;
}

void encode_artifact_summary(fr_buf &out, const ArtifactSummary &item)
{
    put_str(out, item.ns);
    put_str(out, item.name);
    put_str(out, item.version);
    put_u64(out, item.size);
    put_digest(out, item.digest);
    put_str(out, item.creator);
    put_i64(out, item.created_at);
}

ArtifactSummary decode_artifact_summary(fr_buf_reader &in)
{
    ArtifactSummary item;
    item.ns = get_str(in);
    item.name = get_str(in);
    item.version = get_str(in);
    item.size = get_u64(in);
    item.digest = get_digest(in);
    item.creator = get_str(in);
    item.created_at = get_i64(in);
    return item;
}

void encode_list_artifacts_ok(fr_buf &out, const ListArtifactsOk &ok)
{
    put_u32(out, static_cast<uint32_t>(ok.items.size()));
    for (const ArtifactSummary &item : ok.items) {
        encode_artifact_summary(out, item);
    }
}

ListArtifactsOk decode_list_artifacts_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    ListArtifactsOk ok;
    const uint32_t count = get_u32(in);
    if (count > 4096) {
        protocol_error("list count too large");
    }
    ok.items.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        ok.items.push_back(decode_artifact_summary(in));
    }
    return ok;
}

void encode_show_artifact_ok(fr_buf &out, const ShowArtifactOk &ok)
{
    put_str(out, ok.info.ns);
    put_str(out, ok.info.name);
    put_str(out, ok.info.version);
    put_u64(out, ok.info.size);
    put_digest(out, ok.info.digest);
    put_u64(out, ok.info.chunk_size);
    put_str(out, ok.info.creator);
    put_i64(out, ok.info.created_at);
    put_str(out, ok.info.note);
    put_u32(out, static_cast<uint32_t>(ok.info.chunks.size()));
    for (const ManifestEntry &entry : ok.info.chunks) {
        put_u32(out, static_cast<uint32_t>(entry.ordinal));
        put_u64(out, entry.offset);
        put_u64(out, entry.length);
        put_digest(out, entry.digest);
    }
}

ShowArtifactOk decode_show_artifact_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    ShowArtifactOk ok;
    ok.info.ns = get_str(in);
    ok.info.name = get_str(in);
    ok.info.version = get_str(in);
    ok.info.size = get_u64(in);
    ok.info.digest = get_digest(in);
    ok.info.chunk_size = get_u64(in);
    ok.info.creator = get_str(in);
    ok.info.created_at = get_i64(in);
    ok.info.note = get_str(in);
    const uint32_t count = get_u32(in);
    if (count > 1024 * 1024) {
        protocol_error("manifest count too large");
    }
    ok.info.chunks.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        ManifestEntry entry;
        entry.ordinal = static_cast<int64_t>(get_u32(in));
        entry.offset = get_u64(in);
        entry.length = get_u64(in);
        entry.digest = get_digest(in);
        ok.info.chunks.push_back(std::move(entry));
    }
    return ok;
}

void encode_status_ok(fr_buf &out, const StatusOk &ok)
{
    put_str(out, ok.version);
    put_u64(out, ok.uptime_seconds);
    put_u32(out, ok.connections);
    put_u32(out, ok.active_sessions);
    put_u64(out, ok.artifacts);
    put_u64(out, ok.used_bytes);
    put_u64(out, ok.capacity_bytes);
    put_u8(out, ok.high_watermark_percent);
}

StatusOk decode_status_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    StatusOk ok;
    ok.version = get_str(in);
    ok.uptime_seconds = get_u64(in);
    ok.connections = get_u32(in);
    ok.active_sessions = get_u32(in);
    ok.artifacts = get_u64(in);
    ok.used_bytes = get_u64(in);
    ok.capacity_bytes = get_u64(in);
    ok.high_watermark_percent = get_u8(in);
    return ok;
}

void encode_delete_artifact_ok(fr_buf &out, const DeleteArtifactOk &ok)
{
    put_i64(out, ok.marked_chunks);
}

DeleteArtifactOk decode_delete_artifact_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    DeleteArtifactOk ok;
    ok.marked_chunks = get_i64(in);
    return ok;
}

void encode_run_gc_req(fr_buf &out, const RunGcReq &req)
{
    put_u8(out, req.dry_run ? 1 : 0);
}

RunGcReq decode_run_gc_req(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    RunGcReq req;
    req.dry_run = get_u8(in) != 0;
    return req;
}

void encode_run_gc_ok(fr_buf &out, const RunGcOk &ok)
{
    put_i64(out, ok.scanned);
    put_i64(out, ok.deleted_count);
    put_i64(out, ok.deleted_bytes);
}

RunGcOk decode_run_gc_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    RunGcOk ok;
    ok.scanned = get_i64(in);
    ok.deleted_count = get_i64(in);
    ok.deleted_bytes = get_i64(in);
    return ok;
}

void encode_user_add_req(fr_buf &out, const UserAddReq &req)
{
    put_str(out, req.username);
    put_u8(out, req.role);
}

UserAddReq decode_user_add_req(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    UserAddReq req;
    req.username = get_str(in);
    req.role = get_u8(in);
    return req;
}

void encode_user_item(fr_buf &out, const UserItem &item)
{
    put_str(out, item.username);
    put_u8(out, item.role);
    put_u8(out, item.enabled ? 1 : 0);
    put_i64(out, item.created_at);
}

UserItem decode_user_item(fr_buf_reader &in)
{
    UserItem item;
    item.username = get_str(in);
    item.role = get_u8(in);
    item.enabled = get_u8(in) != 0;
    item.created_at = get_i64(in);
    return item;
}

void encode_user_list_ok(fr_buf &out, const UserListOk &ok)
{
    put_u32(out, static_cast<uint32_t>(ok.items.size()));
    for (const UserItem &item : ok.items) {
        encode_user_item(out, item);
    }
}

UserListOk decode_user_list_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    UserListOk ok;
    const uint32_t count = get_u32(in);
    if (count > 4096) {
        protocol_error("list count too large");
    }
    ok.items.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        ok.items.push_back(decode_user_item(in));
    }
    return ok;
}

void encode_token_create_req(fr_buf &out, const TokenCreateReq &req)
{
    put_str(out, req.username);
    put_u32(out, req.ttl_hours);
}

TokenCreateReq decode_token_create_req(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    TokenCreateReq req;
    req.username = get_str(in);
    req.ttl_hours = get_u32(in);
    return req;
}

void encode_token_create_ok(fr_buf &out, const TokenCreateOk &ok)
{
    put_str(out, ok.token);
}

TokenCreateOk decode_token_create_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    TokenCreateOk ok;
    ok.token = get_str(in);
    return ok;
}

void encode_token_item(fr_buf &out, const TokenItem &item)
{
    put_u64(out, item.id);
    put_str(out, item.username);
    put_i64(out, item.created_at);
    put_i64(out, item.expires_at);
    put_u8(out, item.revoked ? 1 : 0);
}

TokenItem decode_token_item(fr_buf_reader &in)
{
    TokenItem item;
    item.id = get_u64(in);
    item.username = get_str(in);
    item.created_at = get_i64(in);
    item.expires_at = get_i64(in);
    item.revoked = get_u8(in) != 0;
    return item;
}

void encode_token_list_ok(fr_buf &out, const TokenListOk &ok)
{
    put_u32(out, static_cast<uint32_t>(ok.items.size()));
    for (const TokenItem &item : ok.items) {
        encode_token_item(out, item);
    }
}

TokenListOk decode_token_list_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    TokenListOk ok;
    const uint32_t count = get_u32(in);
    if (count > 4096) {
        protocol_error("list count too large");
    }
    ok.items.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        ok.items.push_back(decode_token_item(in));
    }
    return ok;
}

void encode_session_item(fr_buf &out, const SessionItem &item)
{
    put_str(out, item.session_id);
    put_str(out, item.owner);
    put_u8(out, static_cast<uint8_t>(item.state));
    put_str(out, item.ns);
    put_str(out, item.name);
    put_str(out, item.version);
    put_u64(out, item.expected_size);
    put_i64(out, item.created_at);
}

SessionItem decode_session_item(fr_buf_reader &in)
{
    SessionItem item;
    item.session_id = get_str(in);
    item.owner = get_str(in);
    item.state = static_cast<SessionState>(get_u8(in));
    item.ns = get_str(in);
    item.name = get_str(in);
    item.version = get_str(in);
    item.expected_size = get_u64(in);
    item.created_at = get_i64(in);
    return item;
}

void encode_session_list_ok(fr_buf &out, const SessionListOk &ok)
{
    put_u32(out, static_cast<uint32_t>(ok.items.size()));
    for (const SessionItem &item : ok.items) {
        encode_session_item(out, item);
    }
}

SessionListOk decode_session_list_ok(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    SessionListOk ok;
    const uint32_t count = get_u32(in);
    if (count > 4096) {
        protocol_error("list count too large");
    }
    ok.items.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        ok.items.push_back(decode_session_item(in));
    }
    return ok;
}

void encode_ok(fr_buf &out, const void *payload, size_t payload_len)
{
    put_i32(out, FR_OK);
    if (payload != nullptr && payload_len != 0) {
        fr_buf_append(&out, payload, payload_len);
    }
}

void encode_error_payload(fr_buf &out, fr_status code, const std::string &message)
{
    put_i32(out, static_cast<int32_t>(code));
    put_str(out, message);
}

ErrorPayload decode_error_payload(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    ErrorPayload payload;
    payload.code = static_cast<fr_status>(get_i32(in));
    payload.message = get_str(in);
    return payload;
}

ArtifactRef decode_ref(const fr_frame &frame)
{
    fr_buf_reader in = reader_of(frame);
    return get_ref(in);
}

} // namespace fr::msg
