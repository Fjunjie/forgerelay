// fr_client.cpp - 阻塞式客户端实现。
#include "forgerelay/client.hpp"
#include "forgerelay/tls.hpp"

#include <chrono>
#include <cstring>

#include "forgerelay/fr_version.h"
#include "forgerelay/storage/error.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using fr_socket_t = SOCKET;
constexpr fr_socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using fr_socket_t = int;
constexpr fr_socket_t kInvalidSocket = -1;
#endif

namespace fr {

namespace {

void net_init_once()
{
#if defined(_WIN32)
    static bool done = false;
    if (!done) {
        WSADATA data;
        (void)WSAStartup(MAKEWORD(2, 2), &data);
        done = true;
    }
#endif
}

void close_sock(fr_socket_t fd)
{
    if (fd == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    (void)closesocket(fd);
#else
    (void)::close(fd);
#endif
}

void send_all(fr_socket_t fd, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
#if defined(_WIN32)
        const int n =
            ::send(fd, reinterpret_cast<const char *>(data + off), static_cast<int>(len - off), 0);
#else
        const ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            throw_error(FR_E_IO, "connection lost while sending");
        }
        off += static_cast<size_t>(n);
    }
}

void set_timeout(fr_socket_t fd, int seconds)
{
#if defined(_WIN32)
    DWORD ms = static_cast<DWORD>(seconds) * 1000u;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
#else
    timeval tv{};
    tv.tv_sec = seconds;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

/** 读取至少 1 字节；0/错误 → 网络异常。 */
size_t recv_some(fr_socket_t fd, uint8_t *buf, size_t len)
{
#if defined(_WIN32)
    const int n = ::recv(fd, reinterpret_cast<char *>(buf), static_cast<int>(len), 0);
#else
    const ssize_t n = ::recv(fd, buf, len, 0);
#endif
    if (n <= 0) {
        throw_error(FR_E_IO, "connection lost while receiving");
    }
    return static_cast<size_t>(n);
}

} // namespace

struct Client::Impl {
    ClientOptions options;
    fr_socket_t fd = kInvalidSocket;
    fr_frame_parser parser;
    fr_buf inbuf; // 接收缓冲（保留跨帧遗留字节，PROTO-01）
    size_t inpos = 0;
    bool connected = false;
#if defined(FR_HAVE_OPENSSL)
    std::unique_ptr<tls::ClientCtx> tls_ctx;
    std::unique_ptr<tls::Session> tls; // use_tls 时存在（D-20）
#endif

    Impl() { fr_buf_init(&inbuf); }
    ~Impl() { fr_buf_destroy(&inbuf); }

    /** 统一发送路由：TLS 会话存在时走 SSL。 */
    void send_bytes(const uint8_t *data, size_t len)
    {
#if defined(FR_HAVE_OPENSSL)
        if (tls) {
            size_t off = 0;
            while (off < len) {
                const int n = tls->write(data + off, static_cast<int>(len - off));
                if (n <= 0) {
                    throw_error(FR_E_IO, "connection lost while sending (tls)");
                }
                off += static_cast<size_t>(n);
            }
            return;
        }
#endif
        send_all(fd, data, len);
    }

    /** 统一接收路由：读取至少 1 字节。 */
    size_t recv_bytes(uint8_t *buf, size_t len)
    {
#if defined(FR_HAVE_OPENSSL)
        if (tls) {
            const int n = tls->read(buf, static_cast<int>(len));
            if (n <= 0) {
                throw_error(FR_E_IO, "connection lost while receiving (tls)");
            }
            return static_cast<size_t>(n);
        }
#endif
        return recv_some(fd, buf, len);
    }
};

Client::Client(const ClientOptions &options) : impl_(new Impl())
{
    impl_->options = options;
    fr_frame_parser_init(&impl_->parser);
}

Client::~Client()
{
    if (impl_->connected) {
        try {
            fr_buf payload;
            fr_buf_init(&payload);
            fr_frame_encode(&payload, FR_MSG_CLOSE, 0, 1, nullptr, 0);
            impl_->send_bytes(payload.data, payload.len);
            fr_buf_destroy(&payload);
        } catch (...) {
            // 析构尽力而为
        }
    }
    fr_frame_parser_destroy(&impl_->parser);
    close_sock(impl_->fd);
}

void Client::connect()
{
    net_init_once();
    Impl &impl = *impl_;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    const std::string port = std::to_string(impl.options.port);
    const int gai_rc = getaddrinfo(impl.options.host.c_str(), port.c_str(), &hints, &result);
    if (gai_rc != 0 || result == nullptr) {
        throw_error(
            FR_E_IO,
            "cannot resolve server address: " + impl.options.host +
                " (gai=" + std::to_string(gai_rc) + ")");
    }
    impl.fd = kInvalidSocket;
    for (addrinfo *it = result; it != nullptr; it = it->ai_next) {
        impl.fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (impl.fd == kInvalidSocket) {
            continue;
        }
        if (::connect(impl.fd, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            break;
        }
        close_sock(impl.fd);
        impl.fd = kInvalidSocket;
    }
    freeaddrinfo(result);
    if (impl.fd == kInvalidSocket) {
        throw_error(FR_E_IO, "cannot connect to " + impl.options.host + ":" + port);
    }
    set_timeout(impl.fd, impl.options.timeout_seconds);

    /* TLS 握手（SEC-02；D-20：无 OpenSSL 构建请求 TLS 时明确报错）。 */
#if defined(FR_HAVE_OPENSSL)
    if (impl.options.use_tls) {
        impl.tls_ctx = std::make_unique<tls::ClientCtx>();
        impl.tls_ctx->init();
        impl.tls = std::make_unique<tls::Session>();
        impl.tls->connect_client(
            *impl.tls_ctx, impl.fd,
            impl.options.tls_hostname.empty() ? impl.options.host : impl.options.tls_hostname);
    }
#else
    if (impl.options.use_tls) {
        throw_error(
            FR_E_IO,
            "TLS unavailable in this build (no OpenSSL); use a "
            "loopback/plain connection or rebuild with OpenSSL");
    }
#endif
    impl.connected = true;

    /* HELLO 握手。 */
    msg::HelloReq hello;
    hello.client = "frctl " + std::string(fr_core_version_string());
    fr_buf payload;
    fr_buf_init(&payload);
    msg::encode_hello_req(payload, hello);
    (void)request(FR_MSG_HELLO, payload);
    fr_buf_destroy(&payload);

    if (!impl.options.token.empty()) {
        msg::AuthReq auth;
        auth.token = impl.options.token;
        fr_buf auth_payload;
        fr_buf_init(&auth_payload);
        msg::encode_auth_req(auth_payload, auth);
        (void)request(FR_MSG_AUTH, auth_payload);
        fr_buf_destroy(&auth_payload);
    }
}

fr_frame Client::read_frame()
{
    Impl &impl = *impl_;
    for (;;) {
        /* 先消化缓冲中的遗留字节（PROTO-01：多帧连续到达）。 */
        if (impl.inpos < impl.inbuf.len) {
            size_t consumed = 0;
            fr_frame frame{};
            bool ready = false;
            const fr_status fs = fr_frame_parser_feed(
                &impl.parser, impl.inbuf.data + impl.inpos, impl.inbuf.len - impl.inpos, &consumed,
                &frame, &ready);
            if (fs != FR_OK) {
                throw_error(FR_E_PROTOCOL, "server sent invalid frame");
            }
            impl.inpos += consumed;
            if (ready) {
                if (impl.inpos == impl.inbuf.len) {
                    impl.inpos = 0;
                    fr_buf_clear(&impl.inbuf);
                }
                return frame; // payload 指向解析器内部缓冲，下次 feed 前有效（D-02）
            }
        }
        uint8_t chunk[65536];
        const size_t n = impl.recv_bytes(chunk, sizeof(chunk));
        fr_buf_append(&impl.inbuf, chunk, n);
    }
}

void Client::send_raw_frame(uint8_t type, const void *payload, size_t len)
{
    Impl &impl = *impl_;
    fr_buf out;
    fr_buf_init(&out);
    const fr_status st = fr_frame_encode(
        &out, type, 0,
        static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count() & 0x7fffffffu) |
            1u,
        payload, len);
    if (st != FR_OK) {
        fr_buf_destroy(&out);
        throw_error(st, "encode raw frame failed");
    }
    impl.send_bytes(out.data, out.len); // 统一路由：TLS 会话存在时同样加密
    fr_buf_destroy(&out);
}

void Client::send_garbage(const std::string &bytes)
{
    Impl &impl = *impl_;
    if (!bytes.empty()) {
        impl.send_bytes(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
    }
}

fr_frame Client::request(uint8_t type, const fr_buf &payload)
{
    Impl &impl = *impl_;
    fr_buf out;
    fr_buf_init(&out);
    const uint32_t req_id =
        static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count() & 0x7fffffffu) |
        1u;
    const fr_status st = fr_frame_encode(&out, type, 0, req_id, payload.data, payload.len);
    if (st != FR_OK) {
        fr_buf_destroy(&out);
        throw_error(st, "encode request failed");
    }
    impl.send_bytes(out.data, out.len);
    fr_buf_destroy(&out);

    fr_frame frame = read_frame();
    if (frame.type == FR_MSG_ERROR) {
        const msg::ErrorPayload err = msg::decode_error_payload(frame);
        throw_error(err.code, err.message);
    }
    if (frame.type != FR_MSG_OK && frame.type != FR_MSG_DATA) {
        throw_error(FR_E_PROTOCOL, "unexpected response frame type");
    }
    return frame;
}

msg::StatusOk Client::status()
{
    fr_buf payload;
    fr_buf_init(&payload);
    fr_frame frame = request(FR_MSG_STATUS, payload);
    fr_buf_destroy(&payload);
    return msg::decode_status_ok(frame);
}

std::vector<msg::ArtifactSummary> Client::list_artifacts(
    const std::optional<std::string> &ns, const std::optional<std::string> &name, uint32_t limit,
    uint32_t offset)
{
    msg::ListArtifactsReq req;
    req.ns = ns;
    req.name = name;
    req.limit = limit;
    req.offset = offset;
    fr_buf payload;
    fr_buf_init(&payload);
    msg::encode_list_artifacts_req(payload, req);
    fr_frame frame = request(FR_MSG_LIST_ARTIFACTS, payload);
    fr_buf_destroy(&payload);
    return msg::decode_list_artifacts_ok(frame).items;
}

msg::ShowArtifactOk
Client::show_artifact(const std::string &ns, const std::string &name, const std::string &version)
{
    fr_buf payload;
    fr_buf_init(&payload);
    msg::ArtifactRef ref{ns, name, version};
    msg::put_ref(payload, ref);
    fr_frame frame = request(FR_MSG_SHOW_ARTIFACT, payload);
    fr_buf_destroy(&payload);
    return msg::decode_show_artifact_ok(frame);
}

uint64_t Client::download(
    const std::string &ns, const std::string &name, const std::string &version, uint64_t offset,
    uint64_t length, const Sink &sink)
{
    msg::GetArtifactReq req;
    req.ref = {ns, name, version};
    req.offset = offset;
    req.length = length;
    fr_buf payload;
    fr_buf_init(&payload);
    msg::encode_get_artifact_req(payload, req);
    fr_frame frame = request(FR_MSG_GET_ARTIFACT, payload);
    fr_buf_destroy(&payload);

    uint64_t total = 0;
    while (frame.type == FR_MSG_DATA) {
        const msg::DataPayload data = msg::decode_data(frame);
        if (!data.data.empty() && sink) {
            sink(data.data.data(), data.data.size());
        }
        total += data.data.size();
        frame = read_frame();
        if (frame.type == FR_MSG_ERROR) {
            const msg::ErrorPayload err = msg::decode_error_payload(frame);
            throw_error(err.code, err.message);
        }
    }
    if (frame.type != FR_MSG_OK) {
        throw_error(FR_E_PROTOCOL, "download ended unexpectedly");
    }
    return total;
}

msg::CreateUploadOk Client::create_upload(
    const std::string &ns, const std::string &name, const std::string &version,
    uint64_t expected_size, const std::string &expected_digest)
{
    msg::CreateUploadReq req;
    req.ref = {ns, name, version};
    req.expected_size = expected_size;
    req.expected_digest = expected_digest;
    fr_buf payload;
    fr_buf_init(&payload);
    msg::encode_create_upload_req(payload, req);
    fr_frame frame = request(FR_MSG_CREATE_UPLOAD, payload);
    fr_buf_destroy(&payload);
    return msg::decode_create_upload_ok(frame);
}

msg::PutChunkOk
Client::put_chunk(const std::string &session_id, uint64_t ordinal, const void *data, size_t len)
{
    msg::PutChunkReq req;
    req.session_id = session_id;
    req.ordinal = ordinal;
    req.data.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + len);
    fr_buf payload;
    fr_buf_init(&payload);
    msg::encode_put_chunk_req(payload, req);
    fr_frame frame = request(FR_MSG_PUT_CHUNK, payload);
    fr_buf_destroy(&payload);
    return msg::decode_put_chunk_ok(frame);
}

msg::QueryUploadOk Client::query_upload(const std::string &session_id)
{
    fr_buf payload;
    fr_buf_init(&payload);
    msg::put_str(payload, session_id);
    fr_frame frame = request(FR_MSG_QUERY_UPLOAD, payload);
    fr_buf_destroy(&payload);
    return msg::decode_query_upload_ok(frame);
}

msg::CommitUploadOk Client::commit_upload(const std::string &session_id)
{
    fr_buf payload;
    fr_buf_init(&payload);
    msg::put_str(payload, session_id);
    fr_frame frame = request(FR_MSG_COMMIT_UPLOAD, payload);
    fr_buf_destroy(&payload);
    return msg::decode_commit_upload_ok(frame);
}

void Client::abort_upload(const std::string &session_id)
{
    fr_buf payload;
    fr_buf_init(&payload);
    msg::put_str(payload, session_id);
    (void)request(FR_MSG_ABORT_UPLOAD, payload);
    fr_buf_destroy(&payload);
}

int64_t
Client::delete_artifact(const std::string &ns, const std::string &name, const std::string &version)
{
    fr_buf payload;
    fr_buf_init(&payload);
    msg::ArtifactRef ref{ns, name, version};
    msg::put_ref(payload, ref);
    fr_frame frame = request(FR_MSG_DELETE_ARTIFACT, payload);
    fr_buf_destroy(&payload);
    return msg::decode_delete_artifact_ok(frame).marked_chunks;
}

msg::RunGcOk Client::gc(bool dry_run)
{
    fr_buf payload;
    fr_buf_init(&payload);
    msg::RunGcReq req;
    req.dry_run = dry_run;
    msg::encode_run_gc_req(payload, req);
    fr_frame frame = request(FR_MSG_RUN_GC, payload);
    fr_buf_destroy(&payload);
    return msg::decode_run_gc_ok(frame);
}

void Client::user_add(const std::string &username, Role role)
{
    fr_buf payload;
    fr_buf_init(&payload);
    msg::UserAddReq req;
    req.username = username;
    req.role = static_cast<uint8_t>(role);
    msg::encode_user_add_req(payload, req);
    (void)request(FR_MSG_USER_ADD, payload);
    fr_buf_destroy(&payload);
}

void Client::user_disable(const std::string &username)
{
    fr_buf payload;
    fr_buf_init(&payload);
    msg::put_str(payload, username);
    (void)request(FR_MSG_USER_DISABLE, payload);
    fr_buf_destroy(&payload);
}

std::vector<msg::UserItem> Client::user_list()
{
    fr_buf payload;
    fr_buf_init(&payload);
    fr_frame frame = request(FR_MSG_USER_LIST, payload);
    fr_buf_destroy(&payload);
    return msg::decode_user_list_ok(frame).items;
}

std::string Client::token_create(const std::string &username, uint32_t ttl_hours)
{
    fr_buf payload;
    fr_buf_init(&payload);
    msg::TokenCreateReq req;
    req.username = username;
    req.ttl_hours = ttl_hours;
    msg::encode_token_create_req(payload, req);
    fr_frame frame = request(FR_MSG_TOKEN_CREATE, payload);
    fr_buf_destroy(&payload);
    return msg::decode_token_create_ok(frame).token;
}

void Client::token_revoke(uint64_t token_id)
{
    fr_buf payload;
    fr_buf_init(&payload);
    msg::put_u64(payload, token_id);
    (void)request(FR_MSG_TOKEN_REVOKE, payload);
    fr_buf_destroy(&payload);
}

std::vector<msg::TokenItem> Client::token_list(const std::string &username)
{
    fr_buf payload;
    fr_buf_init(&payload);
    msg::put_str(payload, username);
    fr_frame frame = request(FR_MSG_TOKEN_LIST, payload);
    fr_buf_destroy(&payload);
    return msg::decode_token_list_ok(frame).items;
}

std::vector<msg::SessionItem> Client::session_list()
{
    fr_buf payload;
    fr_buf_init(&payload);
    fr_frame frame = request(FR_MSG_SESSION_LIST, payload);
    fr_buf_destroy(&payload);
    return msg::decode_session_list_ok(frame).items;
}

void Client::session_abort(const std::string &session_id)
{
    fr_buf payload;
    fr_buf_init(&payload);
    msg::put_str(payload, session_id);
    (void)request(FR_MSG_SESSION_ABORT, payload);
    fr_buf_destroy(&payload);
}

ExitCode exit_code_for(const Error &err)
{
    switch (err.code()) {
    case FR_E_UNAUTHENTICATED:
        return ExitCode::AuthFailed;
    case FR_E_FORBIDDEN:
        return ExitCode::Permission;
    case FR_E_IO:
    case FR_E_PROTOCOL:
        return ExitCode::Network;
    default:
        return ExitCode::Server;
    }
}

} // namespace fr
