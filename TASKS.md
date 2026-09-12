# ForgeRelay 任务跟踪（TASKS.md）

> 本文件按《ForgeRelay_Requirements_Design.md》V2.0 维护，建立需求编号与开发任务的对应关系，
> 并按里程碑记录完成项、未完成项与后续工作。每阶段固定动作（15.1）结束后更新本文件。

## 1. 里程碑概览

| 里程碑 | 范围 | 状态 |
|---|---|---|
| M0 工程骨架 | 仓库、CMake、目录、规范、示例配置、基础测试 | **完成**（本轮） |
| M1 核心库 | buffer、checked arithmetic、codec（帧编解码）、path、fd、error | **完成**（本轮） |
| M2 存储 | SQLite、chunk、manifest、session、recovery、GC | **完成**（m2 分支） |
| M3 协议服务 | epoll、TLS、frame、连接状态、请求分发 | **完成**（m3-m4 分支；TLS 后端待 Linux 复核，D-20） |
| M4 CLI 与权限 | frctl、用户、令牌、角色授权、审计、状态查询 | **完成**（m3-m4 分支） |
| M5 完善交付 | 并发、错误场景、资源限制、文档、安装示例 | 未开始 |

## 2. 需求编号 → 任务映射

状态列含义：`—` 未排期；`排队` 已排期待开发；`进行中`；`完成`；`部分` 部分完成（备注说明）。

### 2.1 概述与架构（1、3 章）

| 需求 | 内容摘要 | 任务 | 所属里程碑 | 状态 |
|---|---|---|---|---|
| OBJ-01 | 上传/下载/查询/删除 | SRV-CMD、CLI-CMD | M2–M4 | — |
| OBJ-02 | 分块传输、断点续传 | SRV-UP、CLI-UP | M2–M4 | — |
| OBJ-03 | 内容寻址、块复用 | STO-CHUNK | M2 | — |
| OBJ-04 | 元数据一致性、恢复 | STO-RECV | M2 | — |
| OBJ-05 | 认证、授权、配额、审计 | AUTH-*、AUDIT | M2–M4 | — |
| OBJ-06 | 单节点、无外部依赖 | —（架构约束） | 全程 | — |
| ARCH-01 | C 接口不暴露 C++ 类型 | CORE-C-API | M1 | 完成 |
| ARCH-02 | 网络线程不做哈希/长事务 | SRV-IO | M3 | — |
| ARCH-03 | 明确所有权 | CORE-C-API + 各模块文档 | M1 起 | 进行中（M1 模块已文档化） |
| ARCH-04 | 存储根/时钟/随机可替换 | STO-CLOCK | M2 | — |
| ARCH-05 | 公共头不含实现细节、无循环依赖 | CORE-C-API、BUILD | M1 起 | 进行中 |
| §3.3 目录结构 | 目录骨架 | BUILD-TREE | M0 | 完成 |

### 2.2 功能需求（4 章）

| 需求 | 内容摘要 | 任务 | 所属里程碑 | 状态 |
|---|---|---|---|---|
| FR-ART-01 | ns/name/version 唯一标识 | STO-ART、CORE-IDENT | M1（验证规则）/M2（持久化） | M1 完成 |
| FR-ART-02 | ns/name 字符集与长度 | CORE-IDENT | M1 | 完成 |
| FR-ART-03 | version 长度与格式 | CORE-IDENT | M1 | 完成 |
| FR-ART-04 | 元数据字段 | STO-ART | M2 | — |
| FR-ART-05 | 默认不可覆盖 | SRV-DEL | M2/M3 | — |
| FR-ART-06 | 查询与分页列出 | SRV-QUERY | M2/M3 | — |
| FR-UP-01 | 创建会话 | SRV-UP | M2/M3 | — |
| FR-UP-02 | 块大小 4 MiB，1–8 MiB | SRV-UP | M2/M3 | — |
| FR-UP-03 | 返回会话 ID/块大小/缺失块/过期 | SRV-UP | M2/M3 | — |
| FR-UP-04 | 任意顺序、最多 4 并发块 | SRV-UP | M2/M3 | — |
| FR-UP-05 | 块编号/长度/摘要校验 | SRV-UP、CORE-CRC | M2/M3 | — |
| FR-UP-06 | 相同块成功、不同块冲突 | SRV-UP | M2/M3 | — |
| FR-UP-07 | 会话五状态机 | STO-SESSION | M2 | — |
| FR-UP-08 | 提交校验块数/顺序/长度/摘要 | SRV-UP | M2/M3 | — |
| FR-UP-09 | 会话 24h，1–168h 可配 | SRV-UP | M2/M3 | — |
| FR-DL-01 | 全量与字节范围下载 | SRV-DL、CLI-DL | M2–M4 | — |
| FR-DL-02 | 临时文件 + 原子替换 | CORE-FD、CLI-DL | M1（fd 原语）/M4 | 进行中 |
| FR-DL-03 | 下载限内存 | SRV-DL、CLI-DL | M3/M4 | — |
| FR-DL-04 | 空文件、边界、越界、起>止 | SRV-DL | M2/M3 | — |
| FR-DL-05 | 断点续传按临时文件长度 | CLI-DL | M4 | — |
| FR-STO-01 | SHA-256 内容寻址、去重 | STO-CHUNK | M2 | — |
| FR-STO-02 | 块路径仅由摘要生成 | CORE-PATH | M1 | 完成 |
| FR-STO-03 | 块文件固定头部 | STO-CHUNK | M2 | — |
| FR-STO-04 | 临时写入 + 同步 + 原子重命名 | CORE-FD、STO-CHUNK | M1（原语）/M2 | 进行中 |
| FR-STO-05 | 提交一致性与恢复 | STO-RECV | M2 | — |
| FR-STO-06 | 启动检查 + 后台清理 | STO-RECV、SRV-GC | M2/M3 | — |
| FR-STO-07 | 首版不压缩 | —（范围约束） | — | — |
| FR-GC-01 | 删元数据→标记无引用块 | SRV-DEL、STO-GC | M2/M3 | — |
| FR-GC-02 | 保护期后才清理 | STO-GC | M2 | — |
| FR-GC-03 | 容量上限、高水位 85% | STO-GC、CFG | M2/M3 | — |
| FR-GC-04 | 高水位拒新会话 | SRV-UP | M2/M3 | — |
| FR-GC-05 | `frctl gc --dry-run` | CLI-GC | M4 | — |
| FR-AUTH-01 | 用户模型 | STO-USER | M2/M4 | — |
| FR-AUTH-02 | bearer token、只存摘要 | AUTH-TOKEN | M2/M4 | — |
| FR-AUTH-03 | 令牌只显示一次、可撤销 | AUTH-TOKEN | M2/M4 | — |
| FR-AUTH-04 | 请求前鉴权 | SRV-AUTH | M3/M4 | — |
| FR-AUTH-05 | 认证失败速率限制 | SRV-AUTH | M4 | — |
| FR-ADM-01 | `frctl status` | CLI-STATUS | M4 | — |
| FR-ADM-02 | `frctl artifact list/show/delete` | CLI-ART | M4 | — |
| FR-ADM-03 | `frctl user/token` 仅 Admin | CLI-USER | M4 | — |
| FR-ADM-04 | `frctl session list/abort` | CLI-SESS | M4 | — |
| FR-ADM-05 | 结构化审计日志 | AUDIT | M2/M4 | — |

### 2.3 协议（5 章）

| 需求 | 内容摘要 | 任务 | 所属里程碑 | 状态 |
|---|---|---|---|---|
| §5.1 | TCP 有界二进制协议；TLS 1.3；回环可关 | NET-TLS | M3 | — |
| §5.2 帧格式 | Magic/Version/Type/Flags/ReqID/Length/CRC | CORE-FRAME | M1 | 完成 |
| §5.3 消息类型 | 全部消息枚举与白名单 | CORE-FRAME | M1 | 完成 |
| PROTO-01 | 分片到达、多帧连续 | CORE-FRAME | M1 | 完成 |
| PROTO-02 | 分配前校验长度/类型上限 | CORE-FRAME | M1 | 完成 |
| PROTO-03 | 溢出检查 | CORE-CKD | M1 | 完成 |
| PROTO-04 | 未知版本/类型/保留位/格式错误 | CORE-FRAME | M1 | 完成 |
| PROTO-05 | 不长期持有接收缓冲指针 | CORE-FRAME | M1 | 完成（解析器自持有 payload 缓冲） |
| PROTO-06 | 错误不含敏感信息 | CORE-ERR、SRV | M1 起 | 进行中 |

### 2.4 数据模型与数据库（6 章）

| 需求 | 内容摘要 | 任务 | 所属里程碑 | 状态 |
|---|---|---|---|---|
| §6 实体表 | 8 个实体 | STO-DB | M2 | — |
| DB-01 | SQLite WAL、busy timeout | STO-DB | M2 | — |
| DB-02 | 绑定参数、排序白名单 | STO-DB | M2 | — |
| DB-03 | 单调版本迁移、单事务 | STO-DB | M2 | — |
| DB-04 | 只存相对路径/摘要 | CORE-PATH、STO-DB | M1（路径规则）/M2 | 进行中 |
| DB-05 | 短写事务 | SRV-DB | M2/M3 | — |

### 2.5 并发、安全、配置、日志（7–9 章）

| 需求 | 内容摘要 | 任务 | 所属里程碑 | 状态 |
|---|---|---|---|---|
| §7.1 并发模型 | epoll I/O 线程 + 工作池 + 维护线程 | SRV-IO | M3 | — |
| LIFE-01 | RAII / create-destroy | CORE-C-API | M1 | 完成 |
| LIFE-02 | 清理幂等 | CORE-FD、SRV | M1 起 | 进行中 |
| LIFE-03 | 异步任务不引用短生命周期对象 | SRV-IO | M3 | — |
| LIFE-04 | 错误路径释放资源 | 各模块 | M1 起 | 进行中 |
| LIFE-05 | SIGTERM 优雅退出 | SRV-LIFE | M3 | — |
| §7.3 资源上限表 | 连接/缓冲/队列/会话上限 | CFG、SRV | M3 | — |
| SEC-01 | OpenSSL 3.x，不自研密码算法 | AUTH-CRYPTO | M2 | — |
| SEC-02 | 客户端验证证书与主机名 | CLI-TLS | M3/M4 | — |
| SEC-03 | 最小文件权限 | CORE-FD、OPS | M1（fd 默认 0600/0700）/M5 | 进行中 |
| SEC-04 | 低权限用户运行 | OPS | M5 | — |
| SEC-05 | 拒绝绝对路径、`..`、NUL、控制字符 | CORE-PATH | M1 | 完成 |
| SEC-06 | 禁 system/popen | —（编码约束） | 全程 | — |
| SEC-07 | 日志不含凭据 | LOG、SRV | M3 起 | — |
| SEC-08 | 大小/连接/会话/磁盘/频率限制 | CFG、SRV | M3 | — |
| CFG-01 | 配置完整校验 | CFG | M3 | — |
| CFG-02 | 命令行覆盖参数 | CLI | M4 | — |
| CFG-03 | 不热更新 | —（范围约束） | — | — |
| LOG-01 | 五个日志级别 | LOG | M3 | — |
| LOG-02 | JSON 行 + request ID | LOG | M3 | — |
| LOG-03 | 块传输 debug 级 | LOG、SRV | M3 | — |
| LOG-04 | 按大小轮转 100 MiB ×5 | LOG | M3 | — |

### 2.6 CLI、错误、工程（10–12 章）

| 需求 | 内容摘要 | 任务 | 所属里程碑 | 状态 |
|---|---|---|---|---|
| §10 命令行 | frctl 全部子命令 | CLI-* | M4 | — |
| CLI-01 | 通用参数 | CLI | M4 | — |
| CLI-02 | `--confirm` | CLI | M4 | — |
| CLI-03 | 稳定退出码 | CLI、CORE-ERR | M4 | — |
| CLI-04 | 进度显示 | CLI | M4 | — |
| ERR-01 | 库不 exit/abort | —（编码约束） | 全程 | — |
| ERR-02 | 稳定错误码+类别+说明 | CORE-ERR | M1 | 完成 |
| ERR-03 | 不忽略系统调用失败 | —（编码约束） | 全程 | — |
| ERR-04 | 可重试/永久错误区分 | CORE-ERR、CLI | M1（分类基础）/M4 | 进行中 |
| ERR-05 | 指数退避重试 | CLI | M4 | — |
| §12.1 编码规范 | C17/C++20、clang-format、警告即错误 | BUILD | M0 | 完成 |
| §12.2 第三方依赖 | OpenSSL、SQLite、toml、GTest | BUILD-DEPS | M1（GTest）/M2 起 | 进行中 |
| §12.3 构建预设 | gcc/clang/asan presets | BUILD | M0 | 完成 |

### 2.7 测试、规模（13–14 章）

| 需求 | 内容摘要 | 任务 | 所属里程碑 | 状态 |
|---|---|---|---|---|
| 单元测试 | buffer/编解码/路径/状态机/数据访问 | TEST-UNIT | M1（核心库）/M2 起 | 进行中 |
| 协议测试 | 全部消息与帧解析、分片/连续帧/截断/超长 | TEST-PROTO | M1（帧层）/M3 | 进行中 |
| 集成测试 | 上传提交下载删除清理恢复 | TEST-INT | M2 起 | — |
| 并发测试 | 重复提交、断开、超时、关闭、清理 | TEST-CONC | M5 | — |
| 动态检查 | ASan/UBSan | TEST-SAN | M1 起 | 进行中 |
| TEST-01 | 断言结果与持久化后果 | —（测试约束） | 全程 | — |
| TEST-02 | 8 个端到端场景 | TEST-E2E | M2–M5 | — |
| TEST-03 | 测试数据运行时生成 | —（测试约束） | 全程 | — |
| TEST-04 | 不得删测试降断言 | —（测试约束） | 全程 | — |
| SIZE-01 | 不人为凑行数 | —（约束） | — | — |
| SIZE-02 | 超 20k 行优先简化 | —（约束） | — | — |
| SIZE-03 | `scripts/count_loc.sh` | BUILD-LOC | M0 | 完成 |

### 2.8 验收标准（17 章）

| 编号 | 验收项 | 所属里程碑 | 状态 |
|---|---|---|---|
| AC-01 | GCC 与 Clang 可配置、编译、测试 | M0–M5 | 进行中（见 4.3 环境说明） |
| AC-02 | 服务可启动、接受连接、正常停止 | M3/M5 | — |
| AC-03 | 上传/续传/提交/查询/下载/删除/清理 | M2–M5 | — |
| AC-04 | 异常退出数据一致 | M2/M5 | — |
| AC-05 | 三角色正反向权限 | M4/M5 | — |
| AC-06 | 大文件不整载内存、上限齐全 | M3/M5 | — |
| AC-07 | 客户端可区分错误类别 | M4 | — |
| AC-08 | 各类测试可重复运行 | M1–M5 | 进行中 |
| AC-09 | 文档完整 | M5 | 进行中 |
| AC-10 | ≤20,000 行生产代码 | M5 | 进行中 |

## 3. M0/M1 文件计划与执行顺序（本轮实施记录）

### 3.1 文件计划

```text
forgerelay/
├── CMakeLists.txt                 # 根构建：项目、选项、子目录、测试注册
├── CMakePresets.json              # gcc/clang debug+release、clang-asan、clang-ubsan + testPresets
├── .clang-format                  # 统一格式
├── .gitignore
├── README.md                      # 项目介绍与快速开始（随里程碑演进）
├── TASKS.md                       # 本文件
├── DEPENDENCIES.md                # 第三方依赖清单
├── cmake/
│   ├── warnings.cmake             # -Wall -Wextra -Wpedantic -Wconversion -Wshadow + 自研目标 -Werror
│   └── sanitizers.cmake           # FR_SANITIZE=address,undefined 注入
├── config/forgerelay.example.toml # 9.1 示例配置
├── include/forgerelay/
│   ├── fr_version.h               # 版本常量
│   ├── fr_error.h                 # fr_status/fr_category/fr_error
│   ├── fr_byteorder.h             # 大端 load/store
│   ├── fr_checked.h               # 溢出检查算术
│   ├── fr_buffer.h                # fr_buf、fr_buf_reader
│   ├── fr_crc32.h                 # CRC32（帧头校验）
│   ├── fr_hex.h                   # hex 编解码与校验
│   ├── fr_frame.h                 # 帧编码 + 增量解析器 + 消息类型
│   ├── fr_path.h                  # 标识验证、摘要路径、安全拼接
│   └── fr_fd.h                    # fd 封装、原子写、临时文件、rename
├── src/core_c/
│   ├── CMakeLists.txt
│   ├── fr_error.c fr_byteorder.c fr_checked.c fr_buffer.c
│   ├── fr_crc32.c fr_hex.c fr_frame.c fr_path.c
│   └── fr_fd.c                    # POSIX 主分支 + 受控 _WIN32 适配（见 DECISIONS D-08）
├── tests/
│   ├── unit/                      # GTest：error/byteorder/checked/buffer/crc32/hex/path/fd
│   ├── protocol/                  # GTest：帧编解码、分片、连续帧、截断、超长、非法字段
│   └── integration/               # M2 起填充
├── tools/                         # M2 起（fr-check 等）
└── scripts/
    └── count_loc.sh               # 生产代码行数统计（SIZE-03）
```

### 3.2 接口计划（M1 核心库公开 API 摘要）

所有 C API 遵循：错误码 `fr_status` 返回，可选 `fr_error *err` 出参携带详情；
资源一律 `init/destroy` 成对；单实例非线程安全、无全局可变状态；详见各头文件 Doxygen 注释。

| 模块 | 关键接口 |
|---|---|
| error | `fr_status_name/message/category`、`fr_error_set(_errno)` |
| byteorder | `fr_load_be16/32/64`、`fr_store_be16/32/64` |
| checked | `fr_checked_add/sub/mul_{u32,u64,size}`（成功才写出参） |
| buffer | `fr_buf_{init,destroy,reserve,append*,consume,clear,view}`、`fr_buf_reader_*` |
| crc32 | `fr_crc32` |
| hex | `fr_hex_encode`、`fr_hex_decode`、`fr_hex_valid` |
| frame | `fr_msg_type_valid`、`fr_frame_encode`、`fr_frame_parser_{init,destroy,feed}` |
| path | `fr_ident_valid_{component,version}`、`fr_digest_hex_valid`、`fr_chunk_rel_path`、`fr_path_{validate_rel,join}` |
| fd | `fr_fd_{open_read,open_write,close,read,write_all,read_full,fsync}`、`fr_file_{rename,unlink,size,read_limited}`、`fr_dir_{create,create_all,sync}`、`fr_tmpfile_create` |

### 3.3 关键技术决策

见 `docs/DECISIONS.md`（D-01 … D-11），要点：错误模型、解析器自持有 payload、
CRC32 自实现（非密码学）、POSIX 主分支 + Windows 适配分支的平台策略、GTest FetchContent。

### 3.4 执行顺序

1. M0：构建骨架 + 规范文件 + 示例配置 + 目录树，双编译器冒烟。
2. M1 按依赖序实现：error → byteorder → checked → buffer → crc32/hex → frame → path → fd。
3. 每模块：实现 → 单测 → 格式化 → 构建测试；全部完成后跑 sanitizer 与双编译器全套。
4. 统计代码量、更新文档、提交。

## 4. 里程碑记录

### 4.1 M0 工程骨架

完成项（本轮）：

- 仓库目录骨架、CMake 3.25+ 配置、`CMakePresets.json`（gcc-debug/gcc-release/clang-debug/clang-release/clang-asan/clang-ubsan 及对应 testPresets）。
- `.clang-format`、`.gitignore`、示例配置 `config/forgerelay.example.toml`。
- 警告矩阵 `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`，自研目标警告视为错误。
- `scripts/count_loc.sh`（SIZE-03）。
- TASKS/DECISIONS/ARCHITECTURE 文档与依赖清单。

### 4.2 M1 核心库

完成项（本轮）：

- `libfrcore`（C17）：error、byteorder、checked、buffer、crc32、hex、frame、path、fd 九个模块，全部含 Doxygen 风格注释（参数、返回值、所有权、线程安全）。
- GTest 单元测试：error/byteorder/checked/buffer/crc32/hex/path/fd。
- 协议测试（帧层）：编码往返、分片逐字节喂入、多帧连续、截断、超长 payload（分配前拒绝）、未知版本/类型、非零 flags、req_id=0、头部 CRC 破坏、payload 生命周期契约。
- 动态检查：ASan+UBSan 全套测试通过。

M1 退出条件核对：核心库单元测试全部通过 ✔；ASan/UBSan 通过 ✔。
生产代码量（scc，include+src，不含空行注释）：1,776 行（C 1,468 + 头 308）。

### 4.3 构建与测试结果记录（本轮）

| 预设/环境 | 编译器 | 构建 | 测试 |
|---|---|---|---|
| clang-debug | Clang 22.1.8 (LLVM-MinGW) | 零警告（-Werror） | 106/106 |
| clang-release | Clang 22.1.8 | 零警告 | 106/106 |
| clang-ubsan | Clang 22.1.8 + UBSan | 通过 | 106/106 |
| clang-asan | Clang 22.1.8 + ASan/UBSan | 通过 | 106/106 |
| gcc-debug | **GNU GCC 16.2.0**（WinLibs UCRT） | 零警告（-Werror） | 106/106 |
| gcc-release | **GNU GCC 16.2.0** | 零警告 | 106/106 |

双编译器差异互检收获（已修复）：

- GCC 的 `-Wconversion` 比严格：对枚举范围外转换（测试中模拟未知错误码）告警 → 测试以局部诊断豁免收敛。
- MinGW 下 `format(printf)` 属性按 MS 语义校验拒绝 `%zu` → 属性改为 `format(gnu_printf)`（UCRT 运行时支持 C99 转换）。
- ASan 抓到测试代码一处栈越界读（固定长度笔误）→ 修复为 `sizeof(payload)-1`；此问题在无 ASan 构建下偶合通过，验证了动态检查的价值。

### 4.4 环境说明与未完成项（重要）

- 本开发机为 Windows（LLVM-MinGW Clang 22.1.8 + WinLibs GCC 16.2.0），无 WSL 发行版、无 Docker：
  - 双编译器验证在本机以真实 GCC 16.2 与 Clang 22 完成（均编译 `_WIN32` fd 分支）；ASan/UBSan 在 LLVM-MinGW 下实测可用。
  - `fr_fd.c` 的 **POSIX 主分支在本机不参与编译**（本机编译 `_WIN32` 适配分支）：其语法已尽力保证，但
    **运行级验证（fsync、O_EXCL、rename 原子性、目录同步、`_FILE_OFFSET_BITS=64`）必须在 Linux 目标环境执行**
    （`cmake --preset gcc-debug` / `clang-asan` 直接复跑），列为 M2 前置任务。
- 未完成项：M3 起的网络/CLI 功能（见第 2 节状态列）。

### 4.5 M2 存储层（m2 分支）

完成项：

- `frstorage`（C++20，`src/storage/` + `include/forgerelay/storage/`）：SQLite 元数据
  （WAL、user_version 单事务迁移、绑定参数、短事务，DB-01..05，V1 含 §6 全部 8 张表）、
  SHA-256 流式摘要双后端（Linux OpenSSL / Windows BCrypt，D-13）、内容寻址块存储
  （临时写入+fsync+原子落位+去重+块头校验，FR-STO-01..04，D-15）、上传会话状态机
  （FR-UP-01..09，含块大小 1–8 MiB 可配、单用户会话上限、过期处理）、原子发布
  （FR-STO-05）、范围读（FR-DL-01..04，D-17）、删除与 GC（FR-GC-01..05，D-18）、
  启动恢复（FR-STO-06：临时文件清理/过期会话/COMMITTING 回滚）。
- 时钟与存储根可注入（ARCH-04）；新增稳定错误码 FR_E_CONFLICT/-16 FR_E_EXPIRED。
- 测试：digest/db/chunk_store/会话/制品/维护 6 套 GTest + M2 集成场景
  （TEST-02 #1 小文件、#2 1GiB 流式、#3 中断恢复、#4 块复用、#5 摘要错、#7 删除+dry-run/实际 GC），
  合计 154 例（M1 106 + M2 48），数据全部运行时生成（TEST-03）。

构建与测试结果（全部 -Werror 零警告）：

| 配置 | 编译器 | 结果 |
|---|---|---|
| clang-debug / clang-ubsan / clang-asan / clang-release | Clang 22.1.8 | 154/154 |
| gcc-debug / release（本地实测） | GNU GCC 16.2.0 | 154/154 |

生产代码量（scc，include+src，不含空行注释）：3,632 行（C 1,468 + C 头 308 + C++ 1,523 + C++ 头 270 + CMake 51）。

M2 退出条件核对：本地上传提交、下载和恢复测试通过 ✔。

环境说明：SHA-256 的 OpenSSL 后端（`fr_digest_ossl.cpp`）与 `fr_fd.c` 的 POSIX 主分支
运行级验证需在 Linux 目标环境复核（同 D-08/D-13，M3 前置任务）。

### 4.6 M3 协议服务 + M4 CLI 与权限（m3-m4 分支）

完成项：

- **M3 传输层**（`src/transport/` + `include/forgerelay/transport.hpp`）：单 I/O 线程
  Poller（Linux epoll / Windows WSAPoll，D-19）+ 固定工作线程池（有界队列、满时 busy，
  §7.1/§7.3）+ 完成队列与唤醒通道 + 空闲扫描 + 优雅停止（LIFE-05）。分发器直接产出
  完整帧，I/O 线程零哈希/零数据库操作（ARCH-02）。
- **M3/M4 服务层**（`src/service/` + `include/forgerelay/service.hpp`）：全部 26 种消息
  分发（§5.3，payload 规范见 PROTOCOL.md §6）；AuthRegistry 独立 SQLite 连接
  （D-22）实现用户/令牌（FR-AUTH-01..03）、角色矩阵（§2.1，FR-AUTH-04）、
  速率限制（FR-AUTH-05）、审计（FR-ADM-05）。
- **M4 应用**：`forgerelayd`（配置 CFG-01/02 + --create-admin 引导 + 信号优雅退出）、
  `frctl`（§10 全部命令，CLI-01..04：--server/--token/--json/--verbose/--confirm、
  稳定退出码 CLI-03、上传/下载进度 CLI-04）；阻塞客户端库 `frclient`。
- **依赖**：toml++ v3.4.0（D-21）。
- **测试**：E2E 7 场景（HELLO/STATUS、上传提交查询下载全链路、权限矩阵正反向
  TEST-02 #8、令牌生命周期、认证限流、审计、并发客户端冒烟）+ log/config/messages
  单元测试，合计 168 例。

构建与测试结果（全部 -Werror 零警告）：

| 配置 | 编译器 | 结果 |
|---|---|---|
| clang-debug / ubsan / asan / release | Clang 22.1.8 | 168/168 |
| gcc-debug / release（本地实测） | GNU GCC 16.2.0 | 168/168 |

生产代码量（scc，include+src）：8,211 行（C++ 5,532 + 头 1,082 + C 1,488 + C 头 312 +
CMake 109），合计 54 文件，未超 20,000 上限。

调试期间 ASan 捕获并修复的实际缺陷（价值记录）：

- worker 的 `Completion.bytes` 未初始化 + `HandleResult` 浅拷贝赋值 → **双重释放**
  （挂死根源，ASan double-free 报告定位）。
- GET 分片续传：续传任务的 has_more 被忽略、continuation_key 未随结果传递 → 下载链
  在第 2 片后停滞（连接 5s 超时暴露）。
- 客户端析构 WSACleanup 清空 Winsock 引用计数 → 后续连接随机失败（D-23）。

M3/M4 退出条件核对：协议测试（M1 帧层 + M3 E2E）与基本端到端流程通过 ✔；frctl 全部
命令与权限测试通过 ✔。

环境说明：epoll/TLS/OpenSSL 后端与 POSIX fd 主分支的运行级验证需在 Linux 目标环境
复核（D-08/D-13/D-19/D-20，M5 前置任务）。

### 4.8 冒烟测试发现并修复的真实二进制缺陷（AC-02 实证）

进程内 E2E 全部通过后，对真实二进制（forgerelayd + frctl）做了完整冒烟
（status/upload/list/download/delete/gc/优雅停止），暴露并修复：

1. **TOML 部分字段缺失时段错误**：toml++ `get(key)` 对缺失键返回 nullptr，
   原实现 `get("key")->value_or(...)` 直接解引用——[tls] 仅含 enabled 时
   certificate 缺失即崩溃。统一改为 node_view::value_or（缺失键取默认值）。
2. **frctl 上传缓冲区溢出**：读入固定 1 MiB 缓冲，但按服务端 chunk_size
   （4 MiB）读取——3 MiB 文件即堆溢出。改为按会话 chunk_size 调整缓冲。
3. **parse_listen_address 拒绝端口 0**：重写时回归引入，破坏测试的
   自动端口分配。已移除（端口 0 = OS 分配为合法语义）。

另修复：close_connection_locked 现同步刷新 StatusHub 连接数（原仅在
accept 时更新，空闲扫描/错误关闭后计数失真）。
