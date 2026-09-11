// fr_server.cpp - TCP 服务器实现（§5/§7）。
//
// 内含：socket 原语、Poller（Linux epoll / Windows WSAPoll，D-19）、
// 连接管理、I/O 线程、有界工作队列线程池、完成队列与唤醒通道。
#include "forgerelay/transport.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>

#include "forgerelay/log.hpp"
#include "forgerelay/service.hpp"
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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using fr_socket_t = int;
constexpr fr_socket_t kInvalidSocket = -1;
#endif

namespace fr {

namespace net {

/* 平台无关的 socket 封装：>0 字节、0 = 暂无数据/写满、-1 错误。 */

void init()
{
#if defined(_WIN32)
    WSADATA data;
    (void)WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

void shutdown_net()
{
#if defined(_WIN32)
    (void)WSACleanup();
#endif
}

void set_nonblocking(fr_socket_t fd)
{
#if defined(_WIN32)
    u_long mode = 1;
    (void)ioctlsocket(fd, static_cast<long>(FIONBIO), &mode);
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    (void)fcntl(fd, F_SETFL, static_cast<int>(flags | O_NONBLOCK));
#endif
}

void set_nodelay(fr_socket_t fd)
{
#if defined(_WIN32)
    BOOL on = TRUE;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
#else
    int on = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
#endif
}

void close_socket(fr_socket_t fd)
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

/** 监听 host:port（port 0 = 自动分配）；失败抛 fr::Error。 */
fr_socket_t listen_on(const std::string &host, uint16_t port)
{
    fr_socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        throw_error(FR_E_IO, "socket() failed");
    }
#if !defined(_WIN32)
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host == "0.0.0.0" || host == "*") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close_socket(fd);
        throw_error(FR_E_ARG, "listen host is not a valid IPv4 address: " + host);
    }
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close_socket(fd);
        throw_error(FR_E_IO, "bind failed on " + host + ":" + std::to_string(port));
    }
    if (::listen(fd, 64) != 0) {
        close_socket(fd);
        throw_error(FR_E_IO, "listen failed");
    }
    set_nonblocking(fd);
    return fd;
}

std::string peer_of(fr_socket_t fd)
{
    sockaddr_in addr{};
#if defined(_WIN32)
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (getpeername(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        return "unknown";
    }
    char buf[32] = {};
    if (inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)) == nullptr) {
        return "unknown";
    }
    return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
}

/** >0 收到字节数；0 = 暂无数据；-1 = 连接错误/对端关闭。 */
int64_t read_some(fr_socket_t fd, uint8_t *buf, size_t len)
{
#if defined(_WIN32)
    const int n = ::recv(fd, reinterpret_cast<char *>(buf), static_cast<int>(len), 0);
    if (n > 0) {
        return n;
    }
    if (n == 0) {
        return -1; // 对端关闭
    }
    const int err = WSAGetLastError();
    return (err == WSAEWOULDBLOCK) ? 0 : -1;
#else
    const ssize_t n = ::recv(fd, buf, len, 0);
    if (n > 0) {
        return static_cast<int64_t>(n);
    }
    if (n == 0) {
        return -1;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
#endif
}

int64_t write_some(fr_socket_t fd, const uint8_t *buf, size_t len)
{
#if defined(_WIN32)
    const int n = ::send(fd, reinterpret_cast<const char *>(buf), static_cast<int>(len), 0);
    if (n > 0) {
        return n;
    }
    const int err = WSAGetLastError();
    return (err == WSAEWOULDBLOCK) ? 0 : -1;
#else
    const ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL);
    if (n >= 0) {
        return static_cast<int64_t>(n);
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
#endif
}

/** 唤醒通道：一对本机互连的 TCP socket（跨平台可用的自管道技巧，D-19）。 */
struct Wakeup {
    fr_socket_t reader = kInvalidSocket;
    fr_socket_t writer = kInvalidSocket;

    void open()
    {
        fr_socket_t listener = net::listen_on("127.0.0.1", 0);
        sockaddr_in addr{};
#if defined(_WIN32)
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        if (getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
            close_socket(listener);
            throw_error(FR_E_IO, "wakeup: getsockname failed");
        }
        writer = ::socket(AF_INET, SOCK_STREAM, 0);
        reader = kInvalidSocket;
        if (::connect(writer, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(listener);
            throw_error(FR_E_IO, "wakeup: connect failed");
        }
        reader = ::accept(listener, nullptr, nullptr);
        close_socket(listener);
        if (reader == kInvalidSocket) {
            throw_error(FR_E_IO, "wakeup: accept failed");
        }
        set_nonblocking(reader);
        set_nonblocking(writer);
    }

    void notify()
    {
        if (writer != kInvalidSocket) {
            const char byte = 1;
            (void)::send(writer, &byte, 1, 0);
        }
    }

    void drain()
    {
        uint8_t buf[256];
        while (read_some(reader, buf, sizeof(buf)) > 0) {
        }
    }

    void close()
    {
        close_socket(reader);
        close_socket(writer);
        reader = kInvalidSocket;
        writer = kInvalidSocket;
    }
};

} // namespace net

// ---- Poller：Linux epoll / Windows WSAPoll（D-19） ----------------------

class Poller {
public:
    struct Event {
        fr_socket_t fd;
        bool readable = false;
        bool writable = false;
    };

    Poller()
    {
#if defined(_WIN32)
        // WSAPoll 直接使用 fd 数组，无需额外资源。
#else
        epoll_ = epoll_create1(0);
        if (epoll_ < 0) {
            throw_error(FR_E_IO, "epoll_create1 failed");
        }
#endif
    }

    ~Poller()
    {
#if !defined(_WIN32)
        if (epoll_ >= 0) {
            net::close_socket(epoll_);
        }
#endif
    }

    void add(fr_socket_t fd, bool readable, bool writable)
    {
#if defined(_WIN32)
        pollfd poll_entry{};
        poll_entry.fd = fd;
        poll_entry.events = static_cast<short>(
            (readable ? POLLRDNORM : 0) | (writable ? POLLWRNORM : 0));
        poll_entry.revents = 0;
        fds_.push_back(poll_entry);
#else
        epoll_event ev{};
        ev.events = static_cast<uint32_t>((readable ? EPOLLIN : 0) | (writable ? EPOLLOUT : 0));
        ev.data.fd = fd;
        if (epoll_ctl(epoll_, EPOLL_CTL_ADD, fd, &ev) != 0) {
            throw_error(FR_E_IO, "epoll_ctl ADD failed");
        }
#endif
    }

    void mod(fr_socket_t fd, bool readable, bool writable)
    {
#if defined(_WIN32)
        for (pollfd &poll_entry : fds_) {
            if (poll_entry.fd == fd) {
                poll_entry.events = static_cast<short>(
                    (readable ? POLLRDNORM : 0) | (writable ? POLLWRNORM : 0));
                return;
            }
        }
#else
        epoll_event ev{};
        ev.events = static_cast<uint32_t>((readable ? EPOLLIN : 0) | (writable ? EPOLLOUT : 0));
        ev.data.fd = fd;
        if (epoll_ctl(epoll_, EPOLL_CTL_MOD, fd, &ev) != 0) {
            throw_error(FR_E_IO, "epoll_ctl MOD failed");
        }
#endif
    }

    void remove(fr_socket_t fd)
    {
#if defined(_WIN32)
        for (size_t i = 0; i < fds_.size(); i++) {
            if (fds_[i].fd == fd) {
                fds_.erase(fds_.begin() + static_cast<long>(i));
                return;
            }
        }
#else
        epoll_ctl(epoll_, EPOLL_CTL_DEL, fd, nullptr);
#endif
    }

    /** 等待事件；timeout_ms < 0 表示无限等待。 */
    void wait(std::vector<Event> &out, int timeout_ms)
    {
        out.clear();
#if defined(_WIN32)
        const int n = ::WSAPoll(fds_.data(), static_cast<ULONG>(fds_.size()),
                                static_cast<INT>(timeout_ms));
        if (n <= 0) {
            return;
        }
        for (const pollfd &poll_entry : fds_) {
            if (poll_entry.revents == 0) {
                continue;
            }
            Event ev;
            ev.fd = poll_entry.fd;
            ev.readable = (poll_entry.revents & (POLLRDNORM | POLLHUP | POLLERR)) != 0;
            ev.writable = (poll_entry.revents & (POLLWRNORM | POLLHUP | POLLERR)) != 0;
            out.push_back(ev);
        }
#else
        epoll_event events[64];
        const int n = epoll_wait(epoll_, events, 64, timeout_ms);
        for (int i = 0; i < n; i++) {
            Event ev;
            ev.fd = events[i].data.fd;
            ev.readable = (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
            ev.writable = (events[i].events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0;
            out.push_back(ev);
        }
#endif
    }

private:
#if defined(_WIN32)
    std::vector<pollfd> fds_;
#else
    int epoll_ = -1;
#endif
};

// ---- Server 实现 --------------------------------------------------------

struct Server::Impl {
    const ServerSettings *settings = nullptr;
    Storage *storage = nullptr;
    Dispatcher *dispatcher = nullptr;
    StatusHub *hub = nullptr;
    Logger *log = nullptr;

    fr_socket_t listen_fd = kInvalidSocket;
    net::Wakeup wakeup;
    Poller poller;
    std::thread io_thread;
    std::vector<std::thread> workers;
    std::thread maintenance_thread;

    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> next_conn_id{1};
    std::atomic<uint64_t> start_clock{0};
    uint16_t port = 0;

    struct Connection {
        fr_socket_t fd = kInvalidSocket;
        std::string peer;
        uint64_t id = 0;
        fr_frame_parser parser;
        fr_buf outbox;
        int64_t last_active = 0;
        std::shared_ptr<ClientSession> session; // 任务与连接共享（生命周期，LIFE-03）
        bool close_after_flush = false;
    };

    std::mutex conns_mu; // 保护 connections（仅 I/O 线程读写，accept 回调外无竞争；保守加锁）
    std::map<uint64_t, Connection> connections;

    /* 工作任务：请求帧或 GET 续传指令。 */
    struct Task {
        uint64_t conn_id = 0;
        std::string peer;
        uint8_t type = 0;
        uint32_t req_id = 0;
        std::vector<uint8_t> payload;      // 请求负载拷贝
        bool is_get_continue = false;      // true 时续传下载分片
        std::string get_key;               // 续传状态键
    };

    std::mutex queue_mu;
    std::condition_variable queue_cv;
    std::deque<Task> queue; // 有界（§7.3）
    size_t queue_capacity = 256;

    struct Completion {
        uint64_t conn_id = 0;
        fr_buf bytes;
        bool close_conn = false;
    };
    std::mutex done_mu;
    std::condition_variable done_cv;
    std::deque<Completion> done_queue;
    static constexpr size_t kDoneQueueHighWater = 128;

    void enqueue(Task &&task)
    {
        std::unique_lock<std::mutex> lock(queue_mu);
        queue_cv.wait(lock, [&] { return stopping.load() || queue.size() < queue_capacity; });
        if (stopping.load()) {
            return;
        }
        queue.push_back(std::move(task));
        queue_cv.notify_one();
    }

    bool pop_task(Task &out)
    {
        std::lock_guard<std::mutex> lock(queue_mu);
        if (queue.empty()) {
            return false;
        }
        out = std::move(queue.front());
        queue.pop_front();
        return true;
    }

    void push_completion(Completion &&completion)
    {
        /* 背压（§7.1）：完成队列超过高水位时阻塞工作线程。 */
        std::unique_lock<std::mutex> lock(done_mu);
        done_cv.wait(lock, [&] { return stopping.load() || done_queue.size() < kDoneQueueHighWater; });
        done_queue.push_back(std::move(completion));
        done_cv.notify_one();
    }

    void drain_completions()
    {
        std::deque<Completion> batch;
        {
            std::lock_guard<std::mutex> lock(done_mu);
            batch.swap(done_queue);
        }
        for (Completion &completion : batch) {
            std::lock_guard<std::mutex> lock(conns_mu);
            auto it = connections.find(completion.conn_id);
            if (it == connections.end()) {
                fr_buf_destroy(&completion.bytes);
                continue;
            }
            Connection &conn = it->second;
            if (conn.close_after_flush) {
                fr_buf_destroy(&completion.bytes);
                continue;
            }
            fr_buf_append(&conn.outbox, completion.bytes.data, completion.bytes.len);
            fr_buf_destroy(&completion.bytes);
            if (completion.close_conn) {
                conn.close_after_flush = true;
            }
        }
    }

    void close_connection_locked(uint64_t conn_id, Poller &poller_ref)
    {
        auto it = connections.find(conn_id);
        if (it == connections.end()) {
            return;
        }
        poller_ref.remove(it->second.fd);
        net::close_socket(it->second.fd);
        fr_frame_parser_destroy(&it->second.parser);
        fr_buf_destroy(&it->second.outbox);
        connections.erase(it);
    }

    void worker_loop(int worker_index)
    {
        (void)worker_index;
        Logger &logger = *log;
        while (!stopping.load()) {
            Task task;
            if (!pop_task(task)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            HandleResult result;
            fr_buf_init(&result.out);
            try {
                fr_frame frame{};
                fr_frame_parser parser;
                fr_frame_parser_init(&parser);
                fr_buf payload_buf;
                fr_buf_init(&payload_buf);
                fr_buf_append(&payload_buf, task.payload.data(), task.payload.size());
                frame.type = task.type;
                frame.req_id = task.req_id;
                frame.payload_len = payload_buf.len;
                frame.payload = payload_buf.len != 0 ? payload_buf.data : nullptr;
                /* flags 由帧解析器保证为 0；此处仅重建用于分发的帧视图。 */

                if (task.is_get_continue) {
                    result = dispatcher->handle_get_continue(task.conn_id, task.peer, task.get_key);
                } else {
                    /* 每连接会话状态存于 connections（I/O 线程）；工作线程通过
                     * conn_id 取引用。为避免锁顺序问题，会话对象由连接持有，
                     * 此处以 conn_id 查找（conns_mu 短临界区）。 */
                    std::shared_ptr<ClientSession> session;
                    {
                        std::lock_guard<std::mutex> lock(conns_mu);
                        auto it = connections.find(task.conn_id);
                        if (it != connections.end()) {
                            session = it->second.session;
                        }
                    }
                    if (!session) {
                        fr_buf_destroy(&payload_buf);
                        fr_frame_parser_destroy(&parser);
                        continue; // 连接已关闭，丢弃任务
                    }
                    result = dispatcher->handle(task.conn_id, task.peer, *session, frame);
                }
                /* GET 分片续传：has_more 时把下一段入队（对续传任务同样生效）。 */
                if (result.has_more) {
                    Task continuation;
                    continuation.conn_id = task.conn_id;
                    continuation.peer = task.peer;
                    continuation.is_get_continue = true;
                    continuation.get_key = result.continuation_key;
                    enqueue(std::move(continuation));
                }
                fr_buf_destroy(&payload_buf);
                fr_frame_parser_destroy(&parser);
            } catch (const fr::Error &err) {
                fr_buf_clear(&result.out);
                fr_buf err_payload;
                fr_buf_init(&err_payload);
                fr::msg::encode_error_payload(err_payload, err.code(), err.what());
                fr_frame_encode(&result.out, FR_MSG_ERROR, 0,
                                task.req_id == 0 ? 1u : task.req_id, err_payload.data,
                                err_payload.len);
                fr_buf_destroy(&err_payload);
            } catch (const std::exception &err) {
                fr_buf_clear(&result.out);
                fr_buf err_payload;
                fr_buf_init(&err_payload);
                fr::msg::encode_error_payload(err_payload, FR_E_INTERNAL, err.what());
                fr_frame_encode(&result.out, FR_MSG_ERROR, 0,
                                task.req_id == 0 ? 1u : task.req_id, err_payload.data,
                                err_payload.len);
                fr_buf_destroy(&err_payload);
            }

            Completion completion;
            completion.conn_id = task.conn_id;
            fr_buf_init(&completion.bytes); // 必须初始化：fr_buf_move 会 destroy 旧内容
            fr_buf_move(&completion.bytes, &result.out);
            completion.close_conn = result.close_connection;
            push_completion(std::move(completion));
            wakeup.notify();
            (void)logger;
        }
    }

    void maintenance_loop()
    {
        const int64_t interval = settings->maintenance_interval_seconds;
        while (!stopping.load()) {
            for (int i = 0; i < interval * 10 && !stopping.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (stopping.load()) {
                break;
            }
            try {
                (void)storage->run_maintenance();
            } catch (const std::exception &err) {
                log->error(std::string("maintenance failed: ") + err.what());
            }
        }
    }

    void accept_new()
    {
        while (true) {
            fr_socket_t client = ::accept(listen_fd, nullptr, nullptr);
            if (client == kInvalidSocket) {
                break;
            }
            if (static_cast<int>(connections.size()) >= settings->max_connections) {
                net::close_socket(client); // §7.3：超限直接拒绝
                continue;
            }
            net::set_nonblocking(client);
            net::set_nodelay(client);
            Connection conn;
            conn.fd = client;
            conn.peer = net::peer_of(client);
            conn.id = next_conn_id.fetch_add(1);
            conn.last_active = now_monotonic();
            fr_frame_parser_init(&conn.parser);
            fr_buf_init(&conn.outbox);
            conn.session = std::make_shared<ClientSession>();
            poller.add(client, true, false);
            {
                std::lock_guard<std::mutex> lock(conns_mu);
                connections.emplace(conn.id, std::move(conn));
                if (hub != nullptr) {
                    hub->connections.store(static_cast<uint32_t>(connections.size()));
                }
            }
            log->info("connection accepted: " + net::peer_of(client));
        }
    }

    static int64_t now_monotonic()
    {
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    void io_loop()
    {
        std::vector<Poller::Event> events;
        poller.add(listen_fd, true, false);
        poller.add(wakeup.reader, true, false);

        while (!stopping.load()) {
            poller.wait(events, 500);
            for (const Poller::Event &ev : events) {
                if (ev.fd == wakeup.reader) {
                    wakeup.drain();
                    continue;
                }
                if (ev.fd == listen_fd) {
                    accept_new();
                    continue;
                }
                on_conn_event(ev);
            }
            drain_completions();
            flush_writable();
        }
    }

    void on_conn_event(const Poller::Event &ev)
    {
        uint64_t conn_id = 0;
        {
            std::lock_guard<std::mutex> lock(conns_mu);
            for (const auto &pair : connections) {
                if (pair.second.fd == ev.fd) {
                    conn_id = pair.first;
                    break;
                }
            }
        }
        if (conn_id == 0) {
            return;
        }

        if (ev.readable) {
            uint8_t buf[65536];
            bool keep = true;
            while (keep) {
                const int64_t n = net::read_some(ev.fd, buf, sizeof(buf));
                if (n < 0) {
                    std::lock_guard<std::mutex> lock(conns_mu);
                    close_connection_locked(conn_id, poller);
                    return;
                }
                if (n == 0) {
                    break; // 暂无数据
                }
                keep = n == sizeof(buf);
                std::lock_guard<std::mutex> lock(conns_mu);
                auto it = connections.find(conn_id);
                if (it == connections.end()) {
                    return;
                }
                Connection &conn = it->second;
                conn.last_active = now_monotonic();

                const uint8_t *cursor = buf;
                size_t left = static_cast<size_t>(n);
                while (left != 0) {
                    size_t consumed = 0;
                    fr_frame frame{};
                    bool ready = false;
                    const fr_status st = fr_frame_parser_feed(&conn.parser, cursor, left,
                                                              &consumed, &frame, &ready);
                    if (st != FR_OK) {
                        send_error_locked(conn, frame.req_id, st,
                                          "protocol violation, closing connection");
                        conn.close_after_flush = true;
                        keep = false;
                        break;
                    }
                    cursor += consumed;
                    left -= consumed;
                    if (!ready) {
                        break;
                    }
                    /* 入队（有界队列，满时回 busy，§7.1）。 */
                    bool enqueued = false;
                    {
                        std::lock_guard<std::mutex> qlock(queue_mu);
                        if (queue.size() < queue_capacity && !stopping.load()) {
                            Task task;
                            task.conn_id = conn.id;
                            task.peer = conn.peer;
                            task.type = frame.type;
                            task.req_id = frame.req_id;
                            if (frame.payload != nullptr) {
                                task.payload.assign(frame.payload,
                                                    frame.payload + frame.payload_len);
                            }
                            queue.push_back(std::move(task));
                            enqueued = true;
                        }
                    }
                    if (enqueued) {
                        queue_cv.notify_one();
                    } else {
                        send_error_locked(conn, frame.req_id, FR_E_BUSY, "server busy");
                    }
                }
            }
        }

        if (ev.writable) {
            flush_conn(conn_id);
        }

        {
            std::lock_guard<std::mutex> lock(conns_mu);
            auto it = connections.find(conn_id);
            if (it == connections.end()) {
                return;
            }
            Connection &conn = it->second;
            poller.mod(conn.fd, true, conn.outbox.len != 0);
            if (conn.close_after_flush && conn.outbox.len == 0) {
                close_connection_locked(conn_id, poller);
            }
        }
    }

    void send_error_locked(Connection &conn, uint32_t req_id, fr_status code, const char *message)
    {
        fr_buf payload;
        fr_buf_init(&payload);
        fr::msg::encode_error_payload(payload, code, message != nullptr ? message : "error");
        fr_buf out;
        fr_buf_init(&out);
        (void)fr_frame_encode(&out, FR_MSG_ERROR, 0, req_id == 0 ? 1 : req_id, payload.data,
                              payload.len);
        fr_buf_append(&conn.outbox, out.data, out.len);
        fr_buf_destroy(&out);
        fr_buf_destroy(&payload);
    }

    void flush_writable()
    {
        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lock(conns_mu);
            for (const auto &pair : connections) {
                if (pair.second.outbox.len != 0 || pair.second.close_after_flush) {
                    ids.push_back(pair.first);
                }
            }
        }
        for (uint64_t id : ids) {
            flush_conn(id);
        }
    }

    void flush_conn(uint64_t conn_id)
    {
        std::lock_guard<std::mutex> lock(conns_mu);
        auto it = connections.find(conn_id);
        if (it == connections.end()) {
            return;
        }
        Connection &conn = it->second;
        while (conn.outbox.len != 0) {
            const int64_t n = net::write_some(conn.fd, conn.outbox.data, conn.outbox.len);
            if (n < 0) {
                close_connection_locked(conn_id, poller);
                return;
            }
            if (n == 0) {
                break; // 写满，等 POLLOUT
            }
            (void)fr_buf_consume(&conn.outbox, static_cast<size_t>(n));
        }
        if (conn.close_after_flush && conn.outbox.len == 0) {
            close_connection_locked(conn_id, poller);
        }
    }

    void sweep_idle()
    {
        const int64_t now = now_monotonic();
        std::vector<uint64_t> expired;
        {
            std::lock_guard<std::mutex> lock(conns_mu);
            for (const auto &pair : connections) {
                if (now - pair.second.last_active > settings->request_timeout_seconds) {
                    expired.push_back(pair.first);
                }
            }
            for (uint64_t id : expired) {
                close_connection_locked(id, poller);
            }
        }
    }
};

Server::~Server()
{
    stop();
}

std::unique_ptr<Server> Server::start(const ServerSettings &settings, Storage &storage,
                                      Dispatcher &dispatcher, Logger &logger,
                                      StatusHub *status_hub)
{
    auto server = std::unique_ptr<Server>(new Server());
    server->impl_ = std::unique_ptr<Impl>(new Impl());
    Impl &impl = *server->impl_;
    impl.settings = &settings;
    impl.storage = &storage;
    impl.dispatcher = &dispatcher;
    impl.log = &logger;
    impl.hub = status_hub;
    impl.queue_capacity = 256; // §7.3 默认队列长度

    std::string host_out;
    uint16_t port = 0;
    if (!parse_listen_address(settings.listen, host_out, port)) {
        throw_error(FR_E_ARG, "listen address invalid: " + settings.listen);
    }
    net::init();
    impl.listen_fd = net::listen_on(host_out, port);
    impl.wakeup.open();
    impl.start_clock.store(static_cast<uint64_t>(Impl::now_monotonic()));
    if (impl.hub != nullptr) {
        impl.hub->start_steady_seconds.store(
            static_cast<uint64_t>(static_cast<uint64_t>(Impl::now_monotonic())));
    }

    /* 实际端口（测试用 port 0）。 */
    sockaddr_in addr{};
#if defined(_WIN32)
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (getsockname(impl.listen_fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0) {
        impl.port = ntohs(addr.sin_port);
    }

    logger.info("forgerelayd listening on " + host_out + ":" + std::to_string(impl.port));

    impl.io_thread = std::thread([&impl] { impl.io_loop(); });
    for (int i = 0; i < settings.worker_threads; i++) {
        impl.workers.emplace_back([&impl, i] { impl.worker_loop(i); });
    }
    impl.maintenance_thread = std::thread([&impl] { impl.maintenance_loop(); });
    return server;
}

uint16_t Server::port() const
{
    return impl_->port;
}

uint32_t Server::connection_count() const
{
    std::lock_guard<std::mutex> lock(impl_->conns_mu);
    return static_cast<uint32_t>(impl_->connections.size());
}

void Server::stop()
{
    if (impl_ == nullptr || impl_->stopping.exchange(true)) {
        return;
    }
    impl_->queue_cv.notify_all();
    impl_->done_cv.notify_all();
    net::close_socket(impl_->listen_fd);
    impl_->wakeup.notify();
    if (impl_->io_thread.joinable()) {
        impl_->io_thread.join();
    }
    for (std::thread &worker : impl_->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    if (impl_->maintenance_thread.joinable()) {
        impl_->maintenance_thread.join();
    }
    impl_->wakeup.close();
    {
        std::lock_guard<std::mutex> lock(impl_->conns_mu);
        for (auto &pair : impl_->connections) {
            net::close_socket(pair.second.fd);
            fr_frame_parser_destroy(&pair.second.parser);
            fr_buf_destroy(&pair.second.outbox);
        }
        impl_->connections.clear();
    }
    net::shutdown_net();
}

} // namespace fr
