// fr/service.hpp - 请求分发与鉴权（M3/M4，FR-AUTH、FR-ADM-05）。
//
// Dispatcher 把协议消息映射到 Storage/AuthRegistry 调用：
//   - 每个请求先过鉴权（FR-AUTH-04）：HELLO/AUTH 除外；
//   - 角色矩阵（§2.1）：Reader 只读；Publisher 可上传并删本人制品；
//     Admin 全部（含 USER_*/TOKEN_*/SESSION_*/RUN_GC/任意删除）；
//   - 认证失败速率限制（FR-AUTH-05）：连接内连续失败 ≥5 次断开；
//     源 IP 滑动窗口限流（默认 60 秒内 20 次失败封禁 60 秒）；
//   - 管理操作写入审计（FR-ADM-05）。
//   - 存储调用经全局互斥串行化（M2 Storage 非线程安全；DB-05 写事务短，
//     阻塞点在块文件 I/O，仍在工作线程上，ARCH-02 满足）。
#ifndef FR_SERVICE_DISPATCHER_HPP
#define FR_SERVICE_DISPATCHER_HPP

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "forgerelay/config.hpp"
#include "forgerelay/messages.hpp"
#include "forgerelay/transport.hpp"

namespace fr {

/** 每连接会话状态（由传输层持有、分发器读写）。 */
struct ClientSession {
    bool hello_done = false;
    bool authenticated = false;
    std::string username;
    Role role = Role::Reader;
    int auth_failures = 0;
};

/** 分发结果：待发送字节、是否断开、下载续传语义。 */
struct HandleResult {
    fr_buf out;
    bool close_connection = false;
    /** GET_ARTIFACT 分片续传：true 时传输层须以 continuation_key 调
     * handle_get_continue() 追加剩余 DATA 分片（分片间让出工作线程，AC-06）。 */
    bool has_more = false;
    std::string continuation_key;
};

/** 用户/令牌注册表与审计（独立 SQLite 连接，WAL 多读并发，D-22）。 */
class AuthRegistry {
public:
    explicit AuthRegistry(const std::string &db_path);
    ~AuthRegistry();
    AuthRegistry(const AuthRegistry &) = delete;
    AuthRegistry &operator=(const AuthRegistry &) = delete;

    // 用户管理（Admin 语义，FR-AUTH-01）。
    void user_add(const std::string &username, Role role);
    void user_disable(const std::string &username);
    std::vector<msg::UserItem> user_list();

    // 令牌（FR-AUTH-02/03）：明文仅创建时返回一次；库中只存 SHA-256。
    std::string token_create(const std::string &username, uint32_t ttl_hours);
    std::vector<msg::TokenItem> token_list(const std::string &username);
    void token_revoke(uint64_t token_id);

    /** 校验 bearer token；失败抛 FR_E_UNAUTHENTICATED（含原因）。 */
    msg::AuthOk verify(const std::string &token);

    /** 审计追加（FR-ADM-05：时间/操作者/动作/目标/结果）。 */
    void audit(const std::string &actor, const std::string &action, const std::string &target,
               const std::string &result);

    /** 是否还没有任何用户（引导判断）。 */
    bool has_no_users();

    /** 统计活动会话数（STATUS 用）。 */
    int64_t active_session_count();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** STATUS 共享状态源：Server 更新，Dispatcher 读取（线程安全，ARCH-03）。 */
struct StatusHub {
    std::atomic<uint32_t> connections{0};
    std::atomic<uint64_t> start_steady_seconds{0};
};

/** 请求分发器。 */
class Dispatcher {
public:
    Dispatcher(Storage &storage, AuthRegistry &auth, const ServerSettings &settings, Logger &log,
               std::shared_ptr<StatusHub> status_hub);
    ~Dispatcher();
    Dispatcher(const Dispatcher &) = delete;
    Dispatcher &operator=(const Dispatcher &) = delete;

    /**
     * 处理一个完整帧（工作线程上调用）。
     *
     * @param[in] conn_id 连接标识（日志/寻址）。
     * @param[in] peer 对端地址（限流/日志）。
     * @param[in,out] session 每连接状态。
     * @param[in] frame 完整帧（payload 指向传输层缓冲，调用内有效）。
     * @return 待发送字节与是否断开。
     */
    HandleResult handle(uint64_t conn_id, const std::string &peer, ClientSession &session,
                        const fr_frame &frame);

    /**
     * 续传 GET_ARTIFACT 的下一段 DATA 分片（工作线程上调用）；
     * 状态键由 handle() 返回。连接已不存在时清理状态并返回空结果。
     */
    HandleResult handle_get_continue(uint64_t conn_id, const std::string &peer,
                                     const std::string &state_key);

    /** 连接建立时调用：IP 限流检查；被封禁返回 false（传输层直接断开）。 */
    bool allow_peer(const std::string &peer);

private:
    using MakeOk = std::function<void(const void *, size_t)>;
    void dispatch_business(uint64_t conn_id, ClientSession &session, const fr_frame &frame,
                           HandleResult &result, const MakeOk &make_ok, const MakeOk &ok_empty);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fr

#endif /* FR_SERVICE_DISPATCHER_HPP */
