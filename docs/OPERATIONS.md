# ForgeRelay 运维手册（OPERATIONS.md，M5 交付，需求 §16）

面向运维人员：安装、配置、启动、备份、恢复与故障排查。

## 1. 安装

依赖：OpenSSL 3.x、SQLite 3、CMake ≥ 3.25、GCC ≥ 13 或 Clang ≥ 18、Ninja。

```bash
cmake --preset gcc-release
cmake --build --preset gcc-release -j
sudo cmake --install build/gcc16-release   # 或手动复制 forgerelayd/frctl
```

（构建预设与依赖清单见 README.md 与 DEPENDENCIES.md。）

## 2. 配置

- 示例：`config/forgerelay.example.toml`；部署：`/etc/forgerelay/server.toml`。
- 关键字段与校验（CFG-01，无效即拒绝启动）：
  - `[server]`：listen（host:port）、max_connections（16–1024）、worker_threads（1–32）。
  - `[tls]`：enabled/certificate/private_key；远程监听建议开启（SEC-01/02）。
  - `[storage]`：root、capacity_bytes、high_watermark_percent、upload_session_hours（1–168）。
  - `[database]`：path（SQLite，WAL 自动启用，DB-01）。
  - `[logging]`：level（trace–error）、format=json、file、轮转（LOG-04）。
- 覆盖：`--listen host:port`、`--log-level`（CFG-02）。
- 数据库初始化（首个 Admin + 一次性令牌）：

```bash
forgerelayd --config /etc/forgerelay/server.toml --create-admin admin
```

## 3. 启动与停止

- 前台调试：`forgerelayd --config ...`（Ctrl-C 触发优雅停止）。
- 生产：systemd 单元见 `deploy/forgerelayd.service`；`systemctl stop forgerelayd`
  发送 SIGTERM——服务停止接受新连接、等待在途任务、30 秒强杀保护（LIFE-05）。
- 会话/连接状态在进程内，重启后由启动恢复流程处理（FR-STO-06）：
  遗留临时文件清理、过期上传会话置 EXPIRED、COMMITTING 回滚 OPEN。

## 4. 备份与恢复

- 元数据：SQLite 单文件（默认 `<storage.root>/metadata.db`）。在线备份：

```bash
sqlite3 /var/lib/forgerelay/metadata.db ".backup /backup/metadata-$(date +%F).db"
```

- 块数据：`<storage.root>/chunks/` 为内容寻址（文件名即 SHA-256），可 rsync 增量备份；
  备份先于元数据快照或同时进行（恢复时以元数据为准，孤儿块由 GC 回收）。
- 恢复：停服 → 恢复 metadata.db + chunks/ → 校验属主与权限（0700/0600，SEC-03）→ 启动。
  启动恢复流程会清理不一致状态（FR-STO-06）。

## 5. 监控与日常操作

- `frctl status`：版本、运行时长、连接数、活动会话、制品数、存储占用（FR-ADM-01）。
- `frctl session list` / `session abort <id> --confirm`（FR-ADM-04）。
- `frctl gc --dry-run` 预览，`frctl gc --confirm` 执行清理（FR-GC-05）。
- 日志：JSON 行（LOG-02），`/var/log/forgerelay/server.log`，按 100 MiB×5 轮转；
  认证失败/请求失败为 warn，块传输为 debug（LOG-03）。

## 6. 故障排查

| 现象 | 排查 |
|---|---|
| 启动即退出 "config: ..." | CFG-01 校验失败——按消息修正对应字段 |
| 启动即退出 "TLS backend unavailable" | 构建未启用 OpenSSL（D-20）或证书/私钥不可读 |
| 客户端 "connection lost" | 服务端空闲扫描（request_timeout）已关闭连接；重连即可 |
| 客户端 FR_E_LIMIT | 存储高水位（FR-GC-04）——执行 GC 或扩容 capacity_bytes |
| 客户端 FR_E_CONFLICT（上传） | 同编号不同内容；重传正确块（FR-UP-06） |
| 提交失败 "overall digest mismatch" | 上传数据与声明摘要不符；重建会话重传（FR-UP-08） |
| 客户端 -17/-18 | 认证失败 / 权限不足（CLI-03：退出码 3/4） |

## 7. 已知限制

- Windows 开发分支：TLS 关闭（D-20）、epoll 为 WSAPoll 替代（D-19）。
- TLS(OpenSSL) 与 POSIX fd 的运行级行为需在 Linux 目标环境复核（D-08/D-13/D-19/D-20）。
