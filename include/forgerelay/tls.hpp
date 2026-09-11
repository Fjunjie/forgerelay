// fr_tls.hpp - TLS 1.3 会话封装（SEC-01/02；DECISIONS D-20）。
//
// 仅在定义 FR_HAVE_OPENSSL 时编译（构建系统在 UNIX 上找到 OpenSSL 3.x 后定义；
// Windows 开发分支不编译本文件，回环明文，D-20）。
//
// 服务端：ServerCtx 加载证书/私钥（TLS_server_method，最低 TLS 1.2，实际协商
// 1.3）；Session 持有每连接 SSL，握手以非阻塞状态机驱动（WANT_READ/WANT_WRITE）。
// 客户端：ClientCtx 校验对端证书与主机名（SEC-02），阻塞式连接。
//
// 线程安全：SSL_CTX 创建后只读可多线程共享；每个 SSL 会话仅归属单连接（单线程）。
#ifndef FR_SERVICE_TLS_HPP
#define FR_SERVICE_TLS_HPP

#include <string>

#if defined(FR_HAVE_OPENSSL)

#include <openssl/ssl.h>

namespace fr::tls {

/** 服务端上下文：加载证书与私钥，配置 TLS 1.3 优先。 */
class ServerCtx {
public:
    ServerCtx() = default;
    ~ServerCtx();
    ServerCtx(const ServerCtx &) = delete;
    ServerCtx &operator=(const ServerCtx &) = delete;

    /**
     * 初始化服务端 TLS 上下文。
     *
     * @param[in] certificate PEM 证书路径（含中间证书链）。
     * @param[in] private_key PEM 私钥路径（SEC-03：文件权限由部署方保证）。
     * @throws fr::Error 打开/解析失败（CFG-01 语义：拒绝启动）。
     */
    void init(const std::string &certificate, const std::string &private_key);

    SSL_CTX *raw() const noexcept { return ctx_; }

private:
    SSL_CTX *ctx_ = nullptr;
};

/** 客户端上下文：启用证书与主机名校验（SEC-02，不允许静默降级）。 */
class ClientCtx {
public:
    ClientCtx() = default;
    ~ClientCtx();
    ClientCtx(const ClientCtx &) = delete;
    ClientCtx &operator=(const ClientCtx &) = delete;

    /** 初始化客户端上下文（系统信任库校验服务端证书）。 */
    void init();

    SSL_CTX *raw() const noexcept { return ctx_; }

private:
    SSL_CTX *ctx_ = nullptr;
};

/** 每连接 TLS 会话（服务端 accept 后 / 客户端 connect 后创建）。 */
class Session {
public:
    Session() = default;
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    /**
     * 服务端：为已 accept 的 fd 创建 SSL 会话（随后由 I/O 线程驱动
     * 非阻塞握手：read/write 返回 want 状态时驱动 SSL_accept 推进）。
     *
     * @param[in] ctx 服务端上下文。
     * @param[in] fd 已 accept 的 socket 描述符。
     * @throws fr::Error 创建失败。
     */
    void accept_server(const ServerCtx &ctx, int fd);

    /**
     * 服务端非阻塞握手推进一步（I/O 线程在连接可读/可写时调用）。
     *
     * @param[out] want_read 握手需要等待可读。
     * @param[out] want_write 握手需要等待可写。
     * @return 1 握手完成；0 需要继续等待；-1 致命错误（连接应关闭）。
     */
    int drive_handshake(bool &want_read, bool &want_write);

    /**
     * 客户端：为已 connect 的 fd 建立会话并阻塞完成握手
     * （客户端 socket 为阻塞模式，受 SO_RCVTIMEO 约束）。
     *
     * @param[in] ctx 客户端上下文。
     * @param[in] fd 已连接的 socket 描述符。
     * @param[in] hostname 期望的服务端主机名（SNI + 证书校验，SEC-02）。
     * @throws fr::Error 握手失败或校验不通过。
     */
    void connect_client(const ClientCtx &ctx, int fd, const std::string &hostname);

    /** 是否已完成握手、可正常收发。 */
    bool established() const noexcept { return ssl_ != nullptr && handshake_done_; }

    /** 语义与 net::read_some 对齐：>0 数据；0 = 暂无数据（want 重试）；-1 = 对端关闭/错误。 */
    int read(unsigned char *buf, int len);

    /** 语义与 net::write_some 对齐：>0 已写；0 = 暂不可写；-1 = 错误。 */
    int write(const unsigned char *buf, int len);

    /** 尽力而为的关闭通知（不阻塞）。 */
    void shutdown() noexcept;

private:
    SSL *ssl_ = nullptr;
    bool handshake_done_ = false;
};

} // namespace fr::tls

#endif /* FR_HAVE_OPENSSL */

#endif /* FR_SERVICE_TLS_HPP */
