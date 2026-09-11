# ForgeRelay 传输协议（PROTOCOL.md）

> 依据需求 §5。当前 M1 已实现帧层（`libfrcore` 的 `fr_frame.h`）；消息 payload 结构随 M3 实现，
> 将以「消息规范」小节补充。

## 1. 协议概述

- 面向 TCP 的有界二进制协议；数据面与管理面共用。
- 远程连接使用 TLS 1.3；仅监听回环地址时允许配置关闭 TLS（M3 实现）。
- 所有请求/响应通过 4 字节 Request ID 关联；帧内多字节整数字段为**网络字节序（大端）**。

## 2. 帧格式（§5.2）

```text
偏移  大小  字段            说明
0     4    Magic           固定 'F''R''L''Y' (0x46 0x52 0x4C 0x59)
4     1    Version         首版为 1
5     1    Type            消息类型（见 §3 白名单）
6     2    Flags           保留，必须为 0（大端）
8     4    Request ID      关联请求与响应；0 为保留值，禁止使用（大端）
12    4    Payload Length  负载长度，最大 8 MiB = 8388608（大端）
16    4    Header CRC      前 16 字节的 IEEE CRC32（大端）
20    N    Payload         消息负载
```

- 固定头 20 字节；最大帧长 = 20 + 8 MiB。
- 校验失败、未知版本、未知消息类型、非零 Flags、Request ID 为 0、Payload 超限均返回
  `FR_E_PROTOCOL`（PROTO-04），不进行部分分配（PROTO-02）。
- 解析器支持任意分片到达与多帧连续到达（PROTO-01）；解析结果不持有指向接收缓冲的指针（PROTO-05）。

## 3. 消息类型（§5.3）

| 数值 | 名称 | 方向 | 分类 | 说明 |
|---:|---|---|---|---|
| 1 | HELLO | 双向 | 连接 | 版本协商（M3） |
| 2 | AUTH | C→S | 连接 | bearer token 认证（M3/M4） |
| 3 | PING | 双向 | 连接 | 保活/探活 |
| 4 | CLOSE | 双向 | 连接 | 优雅关闭 |
| 16 | CREATE_UPLOAD | C→S | 上传 | 创建上传会话 |
| 17 | PUT_CHUNK | C→S | 上传 | 上传数据块 |
| 18 | QUERY_UPLOAD | C→S | 上传 | 查询会话缺失块 |
| 19 | COMMIT_UPLOAD | C→S | 上传 | 提交会话 |
| 20 | ABORT_UPLOAD | C→S | 上传 | 终止会话 |
| 32 | GET_ARTIFACT | C→S | 下载 | 请求制品/范围 |
| 33 | DATA | S→C | 下载 | 制品数据流 |
| 48 | LIST_ARTIFACTS | C→S | 查询 | 分页列出 |
| 49 | SHOW_ARTIFACT | C→S | 查询 | 查看元数据 |
| 50 | STATUS | C→S | 查询 | 服务状态 |
| 64 | DELETE_ARTIFACT | C→S | 管理 | 删除制品 |
| 65 | RUN_GC | C→S | 管理 | 执行清理 |
| 66 | USER_ADD | C→S | 管理 | 添加用户 |
| 67 | USER_DISABLE | C→S | 管理 | 禁用用户 |
| 68 | USER_LIST | C→S | 管理 | 列出用户 |
| 80 | TOKEN_CREATE | C→S | 管理 | 创建令牌 |
| 81 | TOKEN_REVOKE | C→S | 管理 | 撤销令牌 |
| 82 | TOKEN_LIST | C→S | 管理 | 列出令牌 |
| 96 | SESSION_LIST | C→S | 管理 | 列出上传会话 |
| 97 | SESSION_ABORT | C→S | 管理 | 终止会话 |
| 200 | OK | S→C | 通用 | 成功响应 |
| 201 | ERROR | S→C | 通用 | 错误响应 |

未列出的类型值一律拒绝（PROTO-04）。新增消息在对应区段续接，并同步本表。

## 4. 错误与状态码

### 4.1 协议层错误码（`libfrcore` `fr_status`）

| 码 | 名称 | 含义 |
|---|---|---|
| 0 | FR_OK | 成功 |
| -1 | FR_E_ARG | 无效参数 |
| -2 | FR_E_RANGE | 取值超出允许范围 |
| -3 | FR_E_OVERFLOW | 算术/容量溢出 |
| -4 | FR_E_NOMEM | 内存分配失败 |
| -5 | FR_E_IO | 文件/系统 I/O 失败 |
| -6 | FR_E_EOF | 意外的输入结束（读满语义） |
| -7 | FR_E_PROTOCOL | 帧格式/字段违规 |
| -8 | FR_E_TRUNCATED | 帧被截断（流关闭时不完整） |
| -9 | FR_E_STATE | 状态机非法转换 |
| -10 | FR_E_NOTFOUND | 对象不存在 |
| -11 | FR_E_EXISTS | 对象已存在 |
| -12 | FR_E_LIMIT | 超出配额/上限（M2+） |
| -13 | FR_E_BUSY | 资源忙碌/背压（M3+） |
| -14 | FR_E_INTERNAL | 内部不变量被破坏 |
| -15 | FR_E_CONFLICT | 内容冲突（如同编号块内容不同） |
| -16 | FR_E_EXPIRED | 对象已过期（如上传会话） |
| -17 | FR_E_UNAUTHENTICATED | 未认证（FR-AUTH-04） |
| -18 | FR_E_FORBIDDEN | 已认证但权限不足（§2.1） |

应用层错误码（认证、权限、配额等业务语义）在 M3/M4 随 ERROR 消息的 payload 结构补充。

### 4.2 错误传播约定（PROTO-06）

ERROR 响应与日志不得包含：bearer token、私钥、完整认证头、内部绝对路径、数据库语句。
详情文本使用稳定短语 + 非敏感参数（如会话 ID、块编号、摘要前缀）。

## 5. 会话与传输语义（M3 实现，此处为协议承诺）

- 上传会话状态机：`OPEN → COMMITTING → COMPLETED / ABORTED / EXPIRED`（FR-UP-07）。
- 块可乱序上传，单会话并发 ≤ 4 块（FR-UP-04）；重复相同块幂等成功，同编号不同内容返回冲突（FR-UP-06）。
- 下载支持全量与单范围请求（FR-DL-01）；DATA 帧序列保持顺序直到流结束。

## 6. 消息负载规范（M3/M4 实现）

所有负载字段**大端**；字符串 = u16be 长度 + UTF-8 字节（≤512）；摘要 = 64 字节小写
hex ASCII；解码越界一律 `FR_E_PROTOCOL`（PROTO-02）。

| 消息 | 负载（请求 / OK 响应） |
|---|---|
| HELLO | u8 协议版本 + s 客户端标识 / s 服务端标识 |
| AUTH | s token / s username + u8 角色（0=Reader 1=Publisher 2=Admin） |
| PING / CLOSE | 空 / 空（CLOSE 后服务端关闭连接） |
| CREATE_UPLOAD | s ns + s name + s version + u64 size + 64B digest / s session_id + u64 chunk_size + i64 expires_at |
| PUT_CHUNK | s session_id + u64 ordinal + N 字节数据 / 64B digest + u8 reused |
| QUERY_UPLOAD | s session_id / u8 state + u64 chunk_count + u32 位图长度 + 位图（ordinal/8 低位在前） |
| COMMIT_UPLOAD | s session_id / s ns + s name + s version + u64 size + 64B digest |
| ABORT_UPLOAD | s session_id / 空 |
| GET_ARTIFACT | s ns + s name + s version + u64 offset + u64 length（UINT64_MAX=到尾）/ 若干 DATA 帧 + 最终 OK（u64 bytes_sent） |
| DATA | N 字节数据（≤1 MiB/帧，GET 响应流） |
| LIST_ARTIFACTS | u8 有ns + [s ns] + u8 有name + [s name] + u32 limit + u32 offset / u32 n + n×(s ns + s name + s version + u64 size + 64B digest + s creator + i64 created_at) |
| SHOW_ARTIFACT | s ns + s name + s version / s ns + s name + s version + u64 size + 64B digest + u64 chunk_size + s creator + i64 created_at + s note + u32 n + n×(u32 ordinal + u64 offset + u64 length + 64B digest) |
| STATUS | 空 / s version + u64 uptime + u32 connections + u32 active_sessions + u64 artifacts + u64 used + u64 capacity + u8 hw% |
| DELETE_ARTIFACT | s ns + s name + s version / i64 marked_chunks |
| RUN_GC | u8 dry_run / i64 scanned + i64 deleted_count + i64 deleted_bytes |
| USER_ADD | s username + u8 role / 空 |
| USER_DISABLE | s username / 空 |
| USER_LIST | 空 / u32 n + n×(s username + u8 role + u8 enabled + i64 created_at) |
| TOKEN_CREATE | s username + u32 ttl_hours（0=永久）/ s token（仅此一次，FR-AUTH-03） |
| TOKEN_REVOKE | u64 token_id / 空 |
| TOKEN_LIST | s username / u32 n + n×(u64 id + s username + i64 created_at + i64 expires_at + u8 revoked) |
| SESSION_LIST | 空 / u32 n + n×(s session_id + s owner + u8 state + s ns + s name + s version + u64 expected_size + i64 created_at) |
| SESSION_ABORT | s session_id / 空 |
| ERROR | i32 fr_status + s 说明（PROTO-06：无敏感信息） |

### 6.1 会话语义（FR-UP）

- 块可乱序上传；同编号同内容幂等成功，同编号不同内容 `FR_E_CONFLICT`（FR-UP-06）。
- 提交校验块数/顺序/总长/整体摘要；失败会话回 OPEN，不发布残缺制品（FR-UP-08/FR-STO-05）。
- QUERY_UPLOAD 位图支持客户端断点续传（FR-UP-03）。

### 6.2 角色矩阵（§2.1，FR-AUTH-04）

| 操作 | Reader | Publisher | Admin |
|---|---|---|---|
| GET/LIST/SHOW/STATUS | ✓ | ✓ | ✓ |
| CREATE/PUT/QUERY/COMMIT/ABORT_UPLOAD（本人会话） | ✗ | ✓ | ✓ |
| DELETE_ARTIFACT | ✗ | 仅本人制品 | ✓ |
| RUN_GC / USER_* / TOKEN_* / SESSION_* | ✗ | ✗ | ✓ |

### 6.3 速率限制（FR-AUTH-05）

连接内连续认证失败 ≥5 次断开连接；来源 IP 在 60 秒窗口内累计 ≥20 次失败封禁 60 秒。
管理操作写入审计（FR-ADM-05）；失败路径不上报审计。
