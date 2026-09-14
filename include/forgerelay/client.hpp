// fr/client.hpp - frctl 使用的阻塞式客户端（CLI-01..03；SEC-02 在 Linux 目标环境经
// OpenSSL TLS 后端生效，Windows 开发环境回环明文，见 DECISIONS D-20）。
#ifndef FR_SERVICE_CLIENT_HPP
#define FR_SERVICE_CLIENT_HPP

#include <functional>
#include <string>
#include <vector>

#include "forgerelay/messages.hpp"
#include "forgerelay/storage/error.hpp"

namespace fr {

/** 用户角色（§2.1）：定义见 messages.hpp。 */

/** 稳定退出码（CLI-03）。 */
enum class ExitCode : int {
    Ok = 0,
    Usage = 2,      // 参数错误
    AuthFailed = 3, // 认证失败
    Permission = 4, // 权限不足
    Network = 5,    // 网络错误
    Server = 6,     // 服务端错误
    LocalFile = 7,  // 本地文件错误
};

struct ClientOptions {
    std::string host = "127.0.0.1";
    uint16_t port = 7443;
    int timeout_seconds = 30;
    std::string token;        // bearer token（FR-AUTH-02）
    bool use_tls = false;     // §5.1 远程默认 TLS；无 OpenSSL 构建置 false（D-20）
    std::string tls_hostname; // 证书主机名校验；空则用 host（SEC-02）
};

/** 一条命令的连接会话；析构自动发送 CLOSE 并关闭。 */
class Client {
public:
    explicit Client(const ClientOptions &options);
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    /** 连接并完成 HELLO/AUTH；失败抛 fr::Error（映射 CLI-03 退出码）。 */
    void connect();

    // 查询
    msg::StatusOk status();
    std::vector<msg::ArtifactSummary> list_artifacts(const std::optional<std::string> &ns,
                                                     const std::optional<std::string> &name,
                                                     uint32_t limit, uint32_t offset);
    msg::ShowArtifactOk show_artifact(const std::string &ns, const std::string &name,
                                      const std::string &version);

    /** 下载（全量或范围）；sink 分段接收，返回总字节数（FR-DL-01/03）。 */
    uint64_t download(const std::string &ns, const std::string &name, const std::string &version,
                      uint64_t offset, uint64_t length, const Sink &sink);

    // 上传（Publisher）
    msg::CreateUploadOk create_upload(const std::string &ns, const std::string &name,
                                      const std::string &version, uint64_t expected_size,
                                      const std::string &expected_digest);
    msg::PutChunkOk put_chunk(const std::string &session_id, uint64_t ordinal, const void *data,
                              size_t len);
    msg::QueryUploadOk query_upload(const std::string &session_id);
    msg::CommitUploadOk commit_upload(const std::string &session_id);
    void abort_upload(const std::string &session_id);

    // 管理（Admin，FR-ADM-02..04）
    int64_t delete_artifact(const std::string &ns, const std::string &name,
                            const std::string &version);
    msg::RunGcOk gc(bool dry_run);
    void user_add(const std::string &username, Role role);
    void user_disable(const std::string &username);
    std::vector<msg::UserItem> user_list();
    std::string token_create(const std::string &username, uint32_t ttl_hours);
    void token_revoke(uint64_t token_id);
    std::vector<msg::TokenItem> token_list(const std::string &username);
    std::vector<msg::SessionItem> session_list();
    void session_abort(const std::string &session_id);

    /** 测试/错误场景专用：发送原始帧（不等待响应、不做合法性包装）。 */
    void send_raw_frame(uint8_t type, const void *payload, size_t len);

    /** 测试/错误场景专用：发送任意原始字节（协议破坏场景）。 */
    void send_garbage(const std::string &bytes);

private:
    /** 发送请求并读取一帧响应；ERROR 帧转换为 fr::Error（保留稳定码）。 */
    fr_frame request(uint8_t type, const fr_buf &payload);

    /** 连续读取帧直到流结束（下载用）；每帧经 on_frame。 */
    fr_frame read_frame();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** fr::Error → CLI 退出码（CLI-03）。 */
ExitCode exit_code_for(const Error &err);

} // namespace fr

#endif /* FR_SERVICE_CLIENT_HPP */
