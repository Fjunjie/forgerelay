# ForgeRelay 依赖清单（DEPENDENCIES.md）

> 依据需求 §12.2：除下表所列依赖外，新增第三方库必须在此说明用途、许可证和不可替代原因。
> 仓库不得提交第三方源码副本。

## 运行/构建依赖

| 依赖 | 版本 | 用途 | 引入方式 | 许可证 | 可替代性说明 |
|---|---|---|---|---|---|
| OpenSSL | 3.x | TLS 1.3、SHA-256、安全随机数（SEC-01） | 系统依赖 | Apache-2.0 | 不可替代：SEC-01 明确要求，不自研密码算法 |
| SQLite | 3.x（≥3.35 推荐，DB-03 迁移用 `user_version`） | 元数据数据库 | 系统依赖 | Public Domain | 不可替代：DB-01 指定；单文件、零管理满足 OBJ-06 |
| GCC | 13+ | 编译器（目标环境） | 系统依赖 | GPL | 二选一 |
| Clang | 18+ | 编译器（目标环境） | 系统依赖 | Apache-2.0 (LLVM) | 二选一 |
| CMake | 3.25+ | 构建体系 | 系统依赖 | BSD-3 | 不可替代：需求 §1 指定 |
| Ninja / Unix Makefiles | 任意近期版本 | CMake 生成器 | 系统依赖 | Apache-2.0 / GPL | 可替换：任何 CMake 支持的生成器 |

## 测试与开发依赖

| 依赖 | 版本 | 用途 | 引入方式 | 许可证 | 可替代性说明 |
|---|---|---|---|---|---|
| GoogleTest | v1.16.x | C++ 单元/协议测试（仅测试目标） | `find_package` 回退 FetchContent（D-10） | BSD-3 | 需求 §12.2 指定 |
| clang-format | 18+ | 代码格式化 | 系统依赖 | Apache-2.0 (LLVM) | 开发工具，不参与构建 |
| cloc / scc | 任意 | 生产代码行数统计（SIZE-03，`scripts/count_loc.sh`） | 系统依赖（可选） | GPL / MIT | 二选一，脚本自动探测 |

## 各里程碑引入计划

- **M1（当前）**：GoogleTest（测试）。核心库自身零第三方依赖。
- **M2**：SQLite。
- **M3**：OpenSSL。
- **M4**：tomlc99 或 toml++（TOML 配置解析，§12.2 许可：tomlc99 MIT / toml++ MIT）。

## 新增依赖登记（追加于此）

（暂无）
