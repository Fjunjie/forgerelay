// fr/transport.hpp - TCP 服务器门面（M3，§5/§7）。
//
// 并发模型（§7.1）：
//   - 单 I/O 线程：Poller（Linux epoll / Windows WSAPoll，D-19）管理监听与连接，
//     仅做帧组装与出站缓冲，不做哈希/长事务（ARCH-02）；
//   - 固定大小工作线程池：有界任务队列（§7.3），满时回 busy 错误；
//   - 后台维护线程：周期调用 Storage::run_maintenance（§7.1）。
//   - 完成队列 + 唤醒通道把工作结果送回 I/O 线程写出（背压经出站缓冲）。
//
// 生命周期（LIFE-01..05）：全部线程 RAII 启停；stop() 幂等；SIGTERM 经
// app 层信号标志转 stop()（LIFE-05）。
//
// 线程安全：start/stop 可跨线程；连接回调由服务层实现（service.hpp Dispatcher）。
#ifndef FR_SERVICE_TRANSPORT_HPP
#define FR_SERVICE_TRANSPORT_HPP

#include <memory>

#include "forgerelay/config.hpp"
#include "forgerelay/storage/storage.hpp"

namespace fr {

struct StatusHub; // 定义于 service.hpp（前向声明，避免循环依赖）

class Logger;
class Dispatcher;

/** 已启动的服务器。析构即优雅停止。 */
class Server {
public:
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    /**
     * 启动监听并拉起全部线程。
     *
     * @param[in] settings 已校验配置（CFG-01）。
     * @param[in] storage 存储门面（调用方保证生命周期长于 Server）。
     * @param[in] dispatcher 请求分发器（调用方保证生命周期长于 Server）。
     * @param[in] logger 日志。
     * @throws fr::Error 绑定失败、TLS 不可用等。
     */
    static std::unique_ptr<Server> start(const ServerSettings &settings, Storage &storage,
                                         Dispatcher &dispatcher, Logger &logger,
                                         StatusHub *status_hub = nullptr);

    /** 实际监听端口（配置端口为 0 时用于测试）。 */
    uint16_t port() const;

    /** 当前活动连接数。 */
    uint32_t connection_count() const;

    /** 停止：停止接受新连接、等待在途任务完成（有界等待）、关闭全部连接。 */
    void stop();

private:
    Server() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fr

#endif /* FR_SERVICE_TRANSPORT_HPP */
