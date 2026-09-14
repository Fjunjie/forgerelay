// fr_tls.cpp - TLS 1.3 会话封装实现（OpenSSL 3.x；FR_HAVE_OPENSSL 门控，D-20）。
// 本文件仅在定义 FR_HAVE_OPENSSL 的构建中编译（Linux 目标平台）；
// 运行级行为的最终复核在 Linux 目标环境执行（TASKS 4.6 M5 前置任务）。
#include "forgerelay/tls.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "forgerelay/storage/error.hpp"

namespace fr::tls {

namespace {

std::string openssl_errors(const char *what)
{
    std::string msg(what);
    unsigned long code = ERR_peek_last_error();
    if (code != 0) {
        char buf[128];
        ERR_error_string_n(code, buf, sizeof(buf));
        msg += ": ";
        msg += buf;
    }
    ERR_clear_error();
    return msg;
}

} // namespace

ServerCtx::~ServerCtx()
{
    if (ctx_ != nullptr) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

void ServerCtx::init(const std::string &certificate, const std::string &private_key)
{
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (ctx_ == nullptr) {
        throw_error(FR_E_IO, openssl_errors("SSL_CTX_new(server) failed"));
    }
    /* TLS 1.3 优先（§5.1）；保留 1.2 以兼容过渡期客户端。 */
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION);
    if (SSL_CTX_use_certificate_chain_file(ctx_, certificate.c_str()) != 1) {
        std::string msg = openssl_errors("load certificate failed");
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        throw_error(FR_E_IO, msg + " (" + certificate + ")");
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, private_key.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx_) != 1) {
        std::string msg = openssl_errors("load private key failed");
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        throw_error(FR_E_IO, msg + " (" + private_key + ")");
    }
}

ClientCtx::~ClientCtx()
{
    if (ctx_ != nullptr) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

void ClientCtx::init()
{
    ctx_ = SSL_CTX_new(TLS_client_method());
    if (ctx_ == nullptr) {
        throw_error(FR_E_IO, openssl_errors("SSL_CTX_new(client) failed"));
    }
    /* SEC-02：校验对端证书与主机名，不允许静默降级。 */
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
        std::string msg = openssl_errors("load default verify paths failed");
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        throw_error(FR_E_IO, msg);
    }
}

Session::~Session()
{
    shutdown();
    if (ssl_ != nullptr) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
}

void Session::accept_server(const ServerCtx &ctx, int fd)
{
    ssl_ = SSL_new(ctx.raw());
    if (ssl_ == nullptr) {
        throw_error(FR_E_IO, openssl_errors("SSL_new(server) failed"));
    }
    if (SSL_set_fd(ssl_, fd) != 1) {
        std::string msg = openssl_errors("SSL_set_fd failed");
        SSL_free(ssl_);
        ssl_ = nullptr;
        throw_error(FR_E_IO, msg);
    }
}

void Session::connect_client(const ClientCtx &ctx, int fd, const std::string &hostname)
{
    ssl_ = SSL_new(ctx.raw());
    if (ssl_ == nullptr) {
        throw_error(FR_E_IO, openssl_errors("SSL_new(client) failed"));
    }
    /* SNI + 主机名校验（SEC-02）：任一设置失败即中止，不允许静默跳过校验
     * （否则 SSL_VERIFY_PEER 只验证证书链信任，任意可信 CA 签发的主机名均可通过）。 */
    if (SSL_set_tlsext_host_name(ssl_, hostname.c_str()) != 1 ||
        SSL_set1_host(ssl_, hostname.c_str()) != 1) {
        std::string msg = openssl_errors("set SNI/hostname verification failed");
        SSL_free(ssl_);
        ssl_ = nullptr;
        throw_error(FR_E_IO, msg + " (" + hostname + ")");
    }
    if (SSL_set_fd(ssl_, fd) != 1) {
        std::string msg = openssl_errors("SSL_set_fd failed");
        SSL_free(ssl_);
        ssl_ = nullptr;
        throw_error(FR_E_IO, msg);
    }
    if (SSL_connect(ssl_) != 1) {
        std::string msg = openssl_errors("TLS handshake failed");
        SSL_free(ssl_);
        ssl_ = nullptr;
        throw_error(FR_E_IO, msg);
    }
    if (SSL_get_verify_result(ssl_) != X509_V_OK) {
        std::string msg = openssl_errors("certificate verify failed");
        SSL_free(ssl_);
        ssl_ = nullptr;
        throw_error(FR_E_IO, msg);
    }
    handshake_done_ = true;
}

int Session::drive_handshake(bool &want_read, bool &want_write)
{
    want_read = false;
    want_write = false;
    if (ssl_ == nullptr || handshake_done_) {
        return 1;
    }
    const int rc = SSL_accept(ssl_);
    if (rc == 1) {
        handshake_done_ = true;
        return 1;
    }
    const int err = SSL_get_error(ssl_, rc);
    if (err == SSL_ERROR_WANT_READ) {
        want_read = true;
        return 0;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        want_write = true;
        return 0;
    }
    ERR_clear_error();
    return -1;
}

int Session::read(unsigned char *buf, int len)
{
    const int rc = SSL_read(ssl_, buf, len);
    if (rc > 0) {
        return rc;
    }
    const int err = SSL_get_error(ssl_, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return 0; // 暂无数据，等待下次就绪（与明文 read_some 语义一致，tls.hpp 契约）
    }
    ERR_clear_error();
    /* SSL_ERROR_ZERO_RETURN（对端 close_notify）与其余错误一律视为连接终止：
     * EOF 后 socket 永久可读，若返回 0 会让电平触发的轮询空转到空闲扫描。 */
    return -1;
}

int Session::write(const unsigned char *buf, int len)
{
    const int rc = SSL_write(ssl_, buf, len);
    if (rc > 0) {
        return rc;
    }
    const int err = SSL_get_error(ssl_, rc);
    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
        return 0; // 暂不可写，稍后重试
    }
    ERR_clear_error();
    return -1;
}

void Session::shutdown() noexcept
{
    if (ssl_ != nullptr) {
        (void)SSL_shutdown(ssl_);
    }
}

} // namespace fr::tls

namespace fr {
// 显式实例化占位：确保 include/forgerelay/tls.hpp 的 C++ 链接在无 TLS 构建下
// 也不会因未使用头文件告警（本翻译单元仅在后端启用时参与编译）。
} // namespace fr
