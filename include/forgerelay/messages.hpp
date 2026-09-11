// fr/messages.hpp - 协议消息负载编解码（§5.3；格式同步记录于 docs/PROTOCOL.md §6）。
//
// 编码约定（全部大端）：
//   - 字符串：u16be 长度 + UTF-8 字节（长度上限见各字段说明）；
//   - 错误响应负载：i32be fr_status + 字符串说明（PROTO-06：不含敏感信息）；
//   - 解码遇到长度/字段越界一律抛 FR_E_PROTOCOL（PROTO-02/04）。
//
// 线程安全：所有函数为纯函数；缓冲区实例由调用方持有。
#ifndef FR_SERVICE_MESSAGES_HPP
#define FR_SERVICE_MESSAGES_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "forgerelay/fr_buffer.h"
#include "forgerelay/fr_error.h"
#include "forgerelay/fr_frame.h"
#include "forgerelay/storage/storage.hpp"

namespace fr {

/** 用户角色（§2.1；协议层共享，含 frctl/client 使用）。 */
enum class Role : uint8_t { Reader = 0, Publisher = 1, Admin = 2 };

const char *role_name(Role role);
bool role_from(const std::string &name, Role &out);

} // namespace fr

namespace fr::msg {

// ---- 基础编码原语 ------------------------------------------------------

void put_str(fr_buf &out, const std::string &value); // u16be len + bytes
void put_u8(fr_buf &out, uint8_t value);
void put_i32(fr_buf &out, int32_t value);
void put_u32(fr_buf &out, uint32_t value);
void put_u64(fr_buf &out, uint64_t value);
void put_i64(fr_buf &out, int64_t value);
void put_digest(fr_buf &out, const std::string &hex); // 64 字节 ASCII

std::string get_str(fr_buf_reader &in); // 上限 512 字符
uint8_t get_u8(fr_buf_reader &in);
int32_t get_i32(fr_buf_reader &in);
uint32_t get_u32(fr_buf_reader &in);
uint64_t get_u64(fr_buf_reader &in);
int64_t get_i64(fr_buf_reader &in);
std::string get_digest(fr_buf_reader &in);
std::vector<uint8_t> get_rest(fr_buf_reader &in); // 剩余全部字节

/** 身份三元组（制品标识）。 */
struct ArtifactRef {
    std::string ns;
    std::string name;
    std::string version;
};

void put_ref(fr_buf &out, const ArtifactRef &ref);
ArtifactRef get_ref(fr_buf_reader &in);
ArtifactRef decode_ref(const fr_frame &frame);

// ---- 各消息负载 --------------------------------------------------------

struct HelloReq {
    uint8_t protocol_version = 1;
    std::string client;
};
void encode_hello_req(fr_buf &out, const HelloReq &req);
HelloReq decode_hello_req(const fr_frame &frame);

struct AuthReq {
    std::string token;
};
void encode_auth_req(fr_buf &out, const AuthReq &req);
AuthReq decode_auth_req(const fr_frame &frame);

struct AuthOk {
    std::string username;
    uint8_t role = 0; // 0=Reader 1=Publisher 2=Admin
};
void encode_auth_ok(fr_buf &out, const AuthOk &ok);
AuthOk decode_auth_ok(const fr_frame &frame);

struct CreateUploadReq {
    ArtifactRef ref;
    uint64_t expected_size = 0;
    std::string expected_digest;
};
void encode_create_upload_req(fr_buf &out, const CreateUploadReq &req);
CreateUploadReq decode_create_upload_req(const fr_frame &frame);

struct CreateUploadOk {
    std::string session_id;
    uint64_t chunk_size = 0;
    int64_t expires_at = 0;
};
void encode_create_upload_ok(fr_buf &out, const CreateUploadOk &ok);
CreateUploadOk decode_create_upload_ok(const fr_frame &frame);

struct PutChunkReq {
    std::string session_id;
    uint64_t ordinal = 0;
    std::vector<uint8_t> data;
};
void encode_put_chunk_req(fr_buf &out, const PutChunkReq &req);
PutChunkReq decode_put_chunk_req(const fr_frame &frame);

struct PutChunkOk {
    std::string digest;
    bool reused = false;
};
void encode_put_chunk_ok(fr_buf &out, const PutChunkOk &ok);
PutChunkOk decode_put_chunk_ok(const fr_frame &frame);

struct QueryUploadOk {
    SessionState state = SessionState::OPEN;
    uint64_t chunk_count = 0;
    std::vector<uint8_t> present_bitmap; // 位序：ordinal/8 字节内低位在前
};
void encode_query_upload_ok(fr_buf &out, const QueryUploadOk &ok);
QueryUploadOk decode_query_upload_ok(const fr_frame &frame);

struct CommitUploadOk {
    ArtifactRef ref;
    uint64_t size = 0;
    std::string digest;
};
void encode_commit_upload_ok(fr_buf &out, const CommitUploadOk &ok);
CommitUploadOk decode_commit_upload_ok(const fr_frame &frame);

struct GetArtifactReq {
    ArtifactRef ref;
    uint64_t offset = 0;
    uint64_t length = UINT64_MAX; // UINT64_MAX 表示读到文件尾
};
void encode_get_artifact_req(fr_buf &out, const GetArtifactReq &req);
GetArtifactReq decode_get_artifact_req(const fr_frame &frame);

struct DataPayload {
    std::vector<uint8_t> data;
};
void encode_data(fr_buf &out, const DataPayload &payload);
DataPayload decode_data(const fr_frame &frame);

struct GetArtifactOk {
    uint64_t bytes_sent = 0;
};
void encode_get_artifact_ok(fr_buf &out, const GetArtifactOk &ok);
GetArtifactOk decode_get_artifact_ok(const fr_frame &frame);

struct ListArtifactsReq {
    std::optional<std::string> ns;
    std::optional<std::string> name;
    uint32_t limit = 50;
    uint32_t offset = 0;
};
void encode_list_artifacts_req(fr_buf &out, const ListArtifactsReq &req);
ListArtifactsReq decode_list_artifacts_req(const fr_frame &frame);

struct ArtifactSummary {
    std::string ns;
    std::string name;
    std::string version;
    uint64_t size = 0;
    std::string digest;
    std::string creator;
    int64_t created_at = 0;
};
void encode_artifact_summary(fr_buf &out, const ArtifactSummary &item);
ArtifactSummary decode_artifact_summary(fr_buf_reader &in);

struct ListArtifactsOk {
    std::vector<ArtifactSummary> items;
};
void encode_list_artifacts_ok(fr_buf &out, const ListArtifactsOk &ok);
ListArtifactsOk decode_list_artifacts_ok(const fr_frame &frame);

struct ShowArtifactOk {
    ArtifactInfo info; // 复用存储层元数据（含清单）
};
void encode_show_artifact_ok(fr_buf &out, const ShowArtifactOk &ok);
ShowArtifactOk decode_show_artifact_ok(const fr_frame &frame);

struct StatusOk {
    std::string version;
    uint64_t uptime_seconds = 0;
    uint32_t connections = 0;
    uint32_t active_sessions = 0;
    uint64_t artifacts = 0;
    uint64_t used_bytes = 0;
    uint64_t capacity_bytes = 0;
    uint8_t high_watermark_percent = 0;
};
void encode_status_ok(fr_buf &out, const StatusOk &ok);
StatusOk decode_status_ok(const fr_frame &frame);

struct DeleteArtifactOk {
    int64_t marked_chunks = 0;
};
void encode_delete_artifact_ok(fr_buf &out, const DeleteArtifactOk &ok);
DeleteArtifactOk decode_delete_artifact_ok(const fr_frame &frame);

struct RunGcReq {
    bool dry_run = false;
};
void encode_run_gc_req(fr_buf &out, const RunGcReq &req);
RunGcReq decode_run_gc_req(const fr_frame &frame);

struct RunGcOk {
    int64_t scanned = 0;
    int64_t deleted_count = 0;
    int64_t deleted_bytes = 0;
};
void encode_run_gc_ok(fr_buf &out, const RunGcOk &ok);
RunGcOk decode_run_gc_ok(const fr_frame &frame);

struct UserAddReq {
    std::string username;
    uint8_t role = 0;
};
void encode_user_add_req(fr_buf &out, const UserAddReq &req);
UserAddReq decode_user_add_req(const fr_frame &frame);

struct UserItem {
    std::string username;
    uint8_t role = 0;
    bool enabled = true;
    int64_t created_at = 0;
};
void encode_user_item(fr_buf &out, const UserItem &item);
UserItem decode_user_item(fr_buf_reader &in);

struct UserListOk {
    std::vector<UserItem> items;
};
void encode_user_list_ok(fr_buf &out, const UserListOk &ok);
UserListOk decode_user_list_ok(const fr_frame &frame);

struct TokenCreateReq {
    std::string username;
    uint32_t ttl_hours = 0; // 0 = 永不过期
};
void encode_token_create_req(fr_buf &out, const TokenCreateReq &req);
TokenCreateReq decode_token_create_req(const fr_frame &frame);

struct TokenCreateOk {
    std::string token; // 仅创建响应返回一次（FR-AUTH-03）
};
void encode_token_create_ok(fr_buf &out, const TokenCreateOk &ok);
TokenCreateOk decode_token_create_ok(const fr_frame &frame);

struct TokenItem {
    uint64_t id = 0;
    std::string username;
    int64_t created_at = 0;
    int64_t expires_at = 0; // 0 = 永不过期
    bool revoked = false;
};
void encode_token_item(fr_buf &out, const TokenItem &item);
TokenItem decode_token_item(fr_buf_reader &in);

struct TokenListOk {
    std::vector<TokenItem> items;
};
void encode_token_list_ok(fr_buf &out, const TokenListOk &ok);
TokenListOk decode_token_list_ok(const fr_frame &frame);

struct SessionItem {
    std::string session_id;
    std::string owner;
    SessionState state = SessionState::OPEN;
    std::string ns;
    std::string name;
    std::string version;
    uint64_t expected_size = 0;
    int64_t created_at = 0;
};
void encode_session_item(fr_buf &out, const SessionItem &item);
SessionItem decode_session_item(fr_buf_reader &in);

struct SessionListOk {
    std::vector<SessionItem> items;
};
void encode_session_list_ok(fr_buf &out, const SessionListOk &ok);
SessionListOk decode_session_list_ok(const fr_frame &frame);

// ---- 通用响应封装 ------------------------------------------------------

/** OK 响应负载 = i32be 0 + 业务负载。 */
void encode_ok(fr_buf &out, const void *payload, size_t payload_len);

/** ERROR 响应负载 = i32be code + 说明文本（PROTO-06）。 */
void encode_error_payload(fr_buf &out, fr_status code, const std::string &message);

/** 解析 ERROR 响应负载。 */
struct ErrorPayload {
    fr_status code = FR_OK;
    std::string message;
};
ErrorPayload decode_error_payload(const fr_frame &frame);

} // namespace fr::msg

#endif /* FR_SERVICE_MESSAGES_HPP */
