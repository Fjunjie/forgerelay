# ForgeRelay

面向中小型研发团队的单节点制品缓存与分发服务。CI 产出的安装包、压缩包、符号文件等
通过 ForgeRelay 上传、缓存与分发：支持大文件分块传输、断点续传、SHA-256 内容寻址与
块级去重、元数据一致性、存储配额与过期清理。

- 服务端 `forgerelayd`（C++20）与命令行客户端 `frctl`（C++20）基于同一 TCP 二进制协议。
- 核心库 `libfrcore`（C17）提供缓冲区、字节序、帧编解码、路径与 fd 封装。
- 离线一致性检查工具 `fr-check`（C++20）。

状态：**开发中**（当前完成 M0 骨架、M1 核心库、M2 存储层、M3 协议服务、M4 CLI 与权限，见 `TASKS.md`）。

## 当前能力（M1 核心库 + M2 存储层 + M3/M4 协议服务与 CLI）

- 稳定错误码与结构化错误（`fr_status` / `fr_error`）。
- 大端字节序读写、溢出检查算术。
- 可增长字节缓冲与边界检查读取器。
- `FRLY` 二进制帧编解码：增量解析、分片容忍、长度/CRC/保留位校验（见 `docs/PROTOCOL.md`）。
- 制品标识校验、摘要路径映射、安全路径拼接。
- fd 封装：整写/整读、fsync、原子 rename、临时文件、目录创建。
- **M3/M4 协议服务与 CLI**：`forgerelayd` 服务端（单 I/O 线程 Poller + 工作线程池、26 种消息分发）、`frctl` 客户端（全部 §10 命令）、bearer 令牌认证 + 角色矩阵 + 速率限制 + 审计、TOML 配置、JSON 行日志。
- **M2 存储层**：SQLite 元数据（WAL + 单调迁移）；SHA-256 内容寻址块存储（去重、临时写入 + 原子落位、块头 CRC 校验）；上传会话状态机（提交校验块数/顺序/总长/整体摘要）；制品发布/查询/分页/范围读/删除；GC（保护期 + 高水位 + dry-run）；启动恢复。

## 快速开始（构建与测试）

要求：CMake ≥ 3.25，GCC ≥ 13 或 Clang ≥ 18，Ninja（推荐），网络（首次拉取 GoogleTest）。

```bash
# 配置 + 构建 + 测试（GCC）
cmake --preset gcc-debug
cmake --build --preset gcc-debug -j
ctest --preset gcc-debug --output-on-failure

# Clang
cmake --preset clang-debug
cmake --build --preset clang-debug -j
ctest --preset clang-debug --output-on-failure

# 动态检查（ASan/UBSan）
cmake --preset clang-asan
cmake --build --preset clang-asan -j
ctest --preset clang-asan --output-on-failure
```

预设清单：`gcc-debug`、`gcc-release`、`clang-debug`、`clang-release`、`clang-asan`
（address+undefined）、`clang-ubsan`（undefined）。编译器由 PATH 解析（`docs/DECISIONS.md` D-11）。

```bash
# 生产代码行数统计（SIZE-03，需安装 scc 或 cloc）
scripts/count_loc.sh
```

## 文档

| 文档 | 内容 |
|---|---|
| `TASKS.md` | 需求编号 → 任务映射、里程碑记录、构建测试结果 |
| `docs/ARCHITECTURE.md` | 模块职责、依赖规则、并发模型 |
| `docs/PROTOCOL.md` | 帧格式、消息类型、状态与错误码 |
| `docs/DECISIONS.md` | 技术决策记录 |
| `DEPENDENCIES.md` | 第三方依赖清单 |
| `config/forgerelay.example.toml` | 服务端示例配置（M3 起生效） |

## 已知限制（当前阶段）

- 存储层、网络服务与 CLI 尚未实现（M2+）。
- `fr_fd.c` 的 POSIX 主分支需在 Linux 上完成运行级验证（本开发机为 Windows，
  编译并验证的是受控 `_WIN32` 适配分支；见 `docs/DECISIONS.md` D-08 与 `TASKS.md` §4.4）。
