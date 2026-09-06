# ForgeRelay 架构说明（ARCHITECTURE.md）

> 依据《ForgeRelay_Requirements_Design.md》V2.0 §3。随里程碑更新实现细节。

## 1. 组件总览

| 组件 | 语言 | 职责 | 状态 |
|---|---|---|---|
| `forgerelayd` | C++20 | 服务端守护进程：连接管理、业务编排、存储、鉴权、维护任务 | M3+ |
| `frctl` | C++20 | 命令行客户端：上传、下载、查询、删除、管理 | M4+ |
| `libfrcore` | C17 | 缓冲区、字节序、帧编解码、摘要、路径、fd 封装 | **M1 交付** |
| `fr-check` | C++20 | 离线一致性检查与修复 | M5+ |

## 2. 分层与依赖规则（§3.2）

```text
Application(forgerelayd/frctl)
    └── Service  ──► Storage ──► Core(libfrcore)
    └── Protocol ──► Core
    └── Transport ─► Core
```

- **Core**：纯 C17，不依赖业务层与 C++ 类型（ARCH-01）；不含网络与数据库。
- **Transport**：连接、帧收发、超时；不直接访问数据库。
- **Protocol**：请求/响应对象与字段校验；不直接操作磁盘。
- **Storage**：块文件、清单、SQLite、恢复；通过接口向业务层提供能力。
- **Service**：编排，不重复实现底层能力。
- **Application**：仅配置、组装、启动、退出。
- 禁止循环依赖（ARCH-05）：依赖箭头单向向下。

## 3. libfrcore 模块结构（M1）

| 模块 | 头文件 | 职责 | 依赖 |
|---|---|---|---|
| error | `fr_error.h` | `fr_status` 稳定错误码、分类、详情格式化 | 无 |
| byteorder | `fr_byteorder.h` | 大端 load/store（网络字节序） | 无 |
| checked | `fr_checked.h` | 加/减/乘溢出检查（PROTO-03） | 无 |
| buffer | `fr_buffer.h` | 可增长字节缓冲 + 边界检查只读读取器 | 无 |
| crc32 | `fr_crc32.h` | IEEE CRC32（帧头校验，D-03） | 无 |
| hex | `fr_hex.h` | hex 编解码/校验（摘要表示） | 无 |
| frame | `fr_frame.h` | 帧编码、增量解析状态机、消息类型白名单 | buffer/byteorder/crc32/checked |
| path | `fr_path.h` | 标识校验（FR-ART-02/03、SEC-05）、摘要路径、安全拼接 | 无（返回 `fr_status`） |
| fd | `fr_fd.h` | fd 打开/读写/同步、原子 rename、临时文件、目录创建 | 无（`fr_status`/`fr_error`） |

关键流程（帧解析）见 `docs/PROTOCOL.md`；错误模型见 `docs/DECISIONS.md` D-01/D-02。

## 4. 后续里程碑预留（未实现，仅为结构占位）

- M2 Storage：`src/storage/`（SQLite、chunk、manifest、session、recovery、GC）。
- M3 Transport：`src/transport/`（epoll 单 I/O 线程 + 固定工作线程池 + 后台维护线程，§7.1）。
- M4 CLI：`src/apps/frctl/`（命令解析、进度、退出码 CLI-03）。

## 5. 并发模型（设计约束，M3 落地）

单 epoll I/O 线程（监听 + 客户端连接 + 非阻塞读写）；固定大小工作线程池执行文件读写、
哈希与数据库操作（ARCH-02）；工作队列有界，满时背压；后台维护线程处理过期与清理。
首版允许互斥锁 + 条件变量，不要求无锁（§7.1）。
