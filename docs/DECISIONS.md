# ForgeRelay 技术决策记录（DECISIONS.md）

> 记录重要接口与设计选择及其原因。遇到设计冲突时在此更新，不静默改变需求。
> 格式：决策 / 背景 / 备选 / 影响。

## D-01 错误模型：稳定枚举 + 可选详情出参

**决策**：全部 C API 返回 `fr_status`（稳定错误码枚举，`FR_OK = 0`），可传入 `fr_error *err` 出参
获取「错误码 + 分类 + 详情文本」。错误详情格式化接口 `fr_error_set()` 截断安全。

**背景**：ERR-01 要求库代码返回结构化错误而非 `exit/abort`；ERR-02 要求稳定错误码、类别与不含敏感信息的说明。

**备选**：(a) 仅返回枚举，无详情——错误定位困难；(b) 每线程全局错误槽（errno 风格）——隐式全局状态，
并发下易误用；(c) C++ 异常跨 C API——违反 ARCH-01。

**影响**：调用方对错误路径显式检查；`fr_error` 可传 `NULL` 表示不关心详情。

## D-02 帧解析器自持有 payload 缓冲

**决策**：`fr_frame_parser` 内部持有 `fr_buf` payload 缓冲；`feed()` 产出完整帧时，`fr_frame.payload`
指向解析器自有缓冲，**有效期到下一次 `feed()` 或 `destroy()`**。

**背景**：PROTO-05 禁止解析结果长期持有指向可移动接收缓冲区的裸指针；传输层接收缓冲会被复用/移动。

**备选**：(a) payload 指向调用方输入缓冲——跨 feed 边界时帧数据会被切断，无法满足 PROTO-05；
(b) 调用方提供拷贝回调——接口复杂化。

**影响**：每帧最多一次 8 MiB 内拷贝（上限 FR_FRAME_MAX_PAYLOAD）；M3 下载大流量场景（DATA 帧）
若成为瓶颈，可在 M3 为单帧流式读取增加专用接口，不改变本接口语义。

## D-03 CRC32 自实现（非密码学）

**决策**：帧头校验字段使用 IEEE CRC32，按位无表实现（每帧仅校验 16 字节固定头，性能不敏感）。

**背景**：SEC-01 仅禁止自研密码算法（TLS/SHA-256/随机数须用 OpenSSL）；CRC 是传输校验，不属密码学。

**备选**：引入第三方 CRC 库——为 16 字节/帧的校验引入依赖不值得。

**影响**：无外部依赖；无静态查找表；运行时惰性表方案因线程安全复杂化被否决。

## D-04 标识与路径校验为手写 allowlist

**决策**：`fr_ident_valid_component/version` 用显式字符集白名单逐字符校验（无正则库）；
拒绝 `NUL`（由 C 字符串边界保证）、控制字符、`.`/`..` 段；块文件相对路径仅由摘要 hex 生成
（`<hex[0:2]>/<hex[2:]>`），用户输入永不参与路径拼接（FR-STO-02）。

**备选**：POSIX regex / C++ std::regex——引入编码区域与性能问题，且 `std::regex` 不可用于 C 层。

**影响**：规则与 FR-ART-02/03 一致：ns/name `[A-Za-z0-9._-]{1,64}`；version `[A-Za-z0-9._+-]{1,96}`
（允许 semver 的 `+build` 段）；两端点值（1、64/96）在单元测试中锁定。

## D-05 Request ID = 0 视为协议错误

**决策**：编码器拒绝发出 `req_id = 0`；解析器收到 `req_id = 0` 的帧返回 `FR_E_PROTOCOL`。

**背景**：§5.2 将 Request ID 0 标注为「保留值」。文档未明说收到 0 是否报错。

**备选**：接受 0——保留值被静默使用，违反保守解析原则，与 PROTO-04 的精神冲突。

**影响**：客户端/服务端请求 ID 从 1 起分配；响应帧回显请求 ID，不会引入 0。如后续规范明确 0 有系统
用途，仅需放宽两处校验并补测试。

## D-06 Flags 字段首版必须为 0

**决策**：编码器只接受 `flags = 0`；解析器对非 0 flags 返回 `FR_E_PROTOCOL`。

**背景**：§5.2 Flags 字段为 2 字节「保留位必须为 0」，首版未定义任何 flag 位。

**影响**：未来新增 flag 时按位定义白名单，同时更新编码/解码两侧与 PROTOCOL.md。

## D-07 消息类型数值分配

**决策**：为 §5.3 全部消息分配具体数值并集中为 `fr_msg_type` 枚举 + `fr_msg_type_valid()` 白名单：
连接 1–4（HELLO/AUTH/PING/CLOSE）；上传 16–20；下载 32–33（GET_ARTIFACT/DATA）；
查询 48–50；管理 64–68（DELETE_ARTIFACT/RUN_GC/USER_*）、80–82（TOKEN_*）、96–97（SESSION_*）；
通用 200–201（OK/ERROR）。其余值一律 `FR_E_PROTOCOL`。

**影响**：PROTOCOL.md 同步记录；新增消息在对应区段续接。

## D-08 平台策略：POSIX 主分支 + Windows 适配分支（开发环境差异）

**决策**：目标平台保持 Linux x86_64（需求 §1）。`fr_fd.c` 以 POSIX API 为主分支实现；
同一文件内以受控的 `#ifdef _WIN32` 分支适配开发验证环境（`open/close/read/write`、
`fsync→_commit`、`rename→MoveFileExW(REPLACE_EXISTING)`、`mkdir→_mkdir`、目录同步为受控 no-op）。
平台差异集中在该单文件，其余核心库代码完全平台无关。

**背景**：本轮开发验证机为 Windows（LLVM-MinGW Clang 22），无 WSL 发行版与 Docker；不引入平台适配
则 M1 的 fd 单测在本机无法运行、无法验证。

**备选**：(a) fd 模块仅 POSIX、本机排除编译——fd 模块完全未验证；(b) 拆分 `fr_fd_posix.c/fr_fd_win32.c`
两文件——POSIX 文件在本机不参与编译，语法错误无法发现，反而更不可验证。

**影响**：
- Windows 分支（含 LLVM-MinGW Clang 22 与 WinLibs GCC 16.2 两个编译器）已在本机完整运行验证；
  ASan/UBSan 在 LLVM-MinGW 下实测可用，全套测试通过。
- **POSIX 主分支在本机不参与编译，其运行行为（fsync、rename 原子性、目录同步）须在 Linux 上复核**，
  列为 M2 前置任务（TASKS 4.4）。
- 绝不在业务代码中出现第二处 `_WIN32`；若 M2+ 再遇平台差异，优先考虑构建期探测而非源码分支。
- MinGW 下 `format(printf)` 属性按 MS 语义校验，拒绝 C99 转换；故统一使用
  `format(gnu_printf)`（POSIX 上等价，UCRT 运行时支持 `%zu` 等）。

## D-09 临时文件命名：唯一性而非机密性

**决策**：`fr_tmpfile_create()` 生成 `tmp-<pid>-<tick>-<counter>` 形式名称，`O_CREAT|O_EXCL`
冲突时重试（有限次）；计数器为进程级原子计数。不调用 OpenSSL 随机数。

**背景**：M1 尚无 OpenSSL 依赖；临时名位于受保护的存储目录（0700），命名不可预测性不是安全边界，
`O_EXCL` 才是（防符号链接竞争与并发冲突）。

**备选**：`mkstemp`——POSIX 可用但 Windows CRT 行为有差异，且无法定制目录前缀一致性；
OpenSSL `RAND_bytes`——M2 引入 OpenSSL 后如需可切换，接口不变。

**影响**：令牌生成等密码学随机数在 M2 使用 OpenSSL（SEC-01），与本模块无关。

## D-10 GTest 经 FetchContent 引入，保留系统包回退

**决策**：`cmake/GetGoogleTest.cmake` 优先 `find_package(GTest)`，失败则 `FetchContent` 拉取
官方 release tag（v1.16.x）；GTest 头以 SYSTEM 方式引入，警告矩阵只作用于自研目标。

**背景**：§12.2 允许「包管理或 FetchContent」；开发机无系统 GTest。

**影响**：离线构建需预装系统 GTest 或预置 FetchContent 缓存；仓库不提交第三方源码（§12.2）。

## D-11 构建预设命名与编译器解析

**决策**：`CMakePresets.json` 中 `gcc-*`/`clang-*` 预设以裸编译器名（`gcc/g++/clang/clang++`）
交给 PATH 解析，不在预设中写绝对路径；本地验证如需固定编译器，用命令行
`-DCMAKE_<LANG>_COMPILER=<path>` 覆盖。

**背景**：预设硬编码机器相关绝对路径会破坏「干净环境可用」（AC-01）。

**影响**：Linux 目标环境开箱即用；本轮本地验证时通过 PATH 前置将 `gcc` 解析到
WinLibs GCC 16.2（真实 GNU GCC），`gcc-*` 预设本体在 Linux 上可直接使用。

## D-12 C 层缓冲不使用隐式全局分配器钩子

**决策**：`fr_buf` 直接使用 `malloc/realloc/free`，分配失败返回 `FR_E_NOMEM`；不做自定义分配器注入。

**背景**：ARCH-04 要求「存储根目录、时钟、随机数来源」可替换——未包含内存分配器；SQLite 本身在
M2 提供分配器配置点，足够覆盖测试需要。

**影响**：无。如未来需要泄漏追踪，通过 ASan/Valgrind 外部工具完成。
