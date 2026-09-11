// fr/storage.hpp - M2 存储层门面（需求 §4.1/4.2/4.4/4.5、§6、§7.3）。
//
// 编排以下能力，不重复实现底层原语（§3.2）：
//   - 内容寻址块存储（chunk_store）、SQLite 元数据（db/schema）；
//   - 上传会话状态机 OPEN→COMMITTING→COMPLETED/ABORTED/EXPIRED（FR-UP-07）；
//   - 发布原子性：元数据+清单单事务，不存在缺块的已发布制品（FR-STO-05）；
//   - GC：无引用 + 超保护期才删除；高水位拒绝新会话（FR-GC-01..04）；
//   - 启动恢复：临时文件清理、过期会话处理、COMMITTING 回滚（FR-STO-06）。
//
// 时钟与存储根目录可替换（ARCH-04）：StorageConfig 注入。
// 线程安全：单个 Storage 实例的方法非线程安全（M3 由服务层串行化/加锁）。
#ifndef FR_STORAGE_STORAGE_HPP
#define FR_STORAGE_STORAGE_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "forgerelay/fr_buffer.h"

namespace fr {

/** 会话状态（FR-UP-07）。 */
enum class SessionState {
    OPEN,
    COMMITTING,
    COMPLETED,
    ABORTED,
    EXPIRED,
};

/** 会话状态转文本（DB 存储/日志用）。 */
const char *session_state_name(SessionState state);

/** 文本转会话状态；未知值抛 FR_E_ARG。 */
SessionState session_state_from(const std::string &name);

/** 上传会话信息。 */
struct SessionInfo {
    std::string id;
    std::string owner;
    SessionState state = SessionState::OPEN;
    std::string ns;
    std::string name;
    std::string version;
    uint64_t expected_size = 0;
    std::string expected_digest;
    uint64_t chunk_size = 0;
    int64_t created_at = 0;
    int64_t expires_at = 0;
};

/** 制品清单项。 */
struct ManifestEntry {
    int64_t ordinal = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
    std::string digest;
};

/** 制品元数据（FR-ART-04）。 */
struct ArtifactInfo {
    std::string ns;
    std::string name;
    std::string version;
    uint64_t size = 0;
    std::string digest;
    uint64_t chunk_size = 0;
    std::string creator;
    int64_t created_at = 0;
    std::string note;
    std::vector<ManifestEntry> chunks; // 仅 show 时填充
};

/** 块接收结果。 */
struct ChunkAccept {
    std::string digest;  ///< 块摘要（小写 hex）。
    bool reused = false; ///< 同编号同内容幂等复用（FR-UP-06）。
};

/** GC 结果统计（FR-GC-05）。 */
struct GcReport {
    int64_t scanned = 0;       ///< 检查的待清理块数。
    int64_t deleted_count = 0; ///< 删除/将删除的块数（dry-run 为预计值）。
    int64_t deleted_bytes = 0; ///< 删除/将释放的字节（dry-run 为预计值）。
};

/** 启动恢复报告（FR-STO-06）。 */
struct RecoveryReport {
    int temp_files_removed = 0;       ///< 清理的遗留临时文件。
    int64_t sessions_expired = 0;     ///< 超期置为 EXPIRED 的会话。
    int64_t sessions_rolled_back = 0; ///< COMMITTING 回滚到 OPEN 的会话。
};

/** 存储配置；clock 可注入（ARCH-04），默认取系统 UTC 秒。 */
struct StorageConfig {
    std::filesystem::path root;                            ///< 存储根目录（必填）。
    uint64_t chunk_size = 4ull * 1024 * 1024;              ///< 上传块大小（FR-UP-02，1-8 MiB）。
    uint64_t capacity_bytes = 100ull * 1024 * 1024 * 1024; ///< 容量上限（FR-GC-03）。
    int high_watermark_percent = 85;                       ///< 高水位百分比（FR-GC-03）。
    int session_hours = 24;                                ///< 会话保留时长（FR-UP-09，1-168）。
    int gc_grace_seconds = 3600;                           ///< 无引用块保护期（FR-GC-02）。
    int max_sessions_per_owner = 8;                        ///< 单用户活动会话上限（§7.3）。
    int busy_timeout_ms = 5000;                            ///< SQLite 忙等超时（DB-01）。
    std::function<int64_t()> clock;                        ///< 时钟源（秒）。
};

/** 读取数据接收器（流式，不整文件载入内存，FR-DL-03）。 */
using Sink = std::function<void(const void *data, size_t len)>;

/** M2 存储门面。 */
class Storage {
public:
    ~Storage();
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

    /**
     * 打开存储：创建目录与数据库、执行迁移、运行启动恢复（FR-STO-06）。
     *
     * @param[in] config 存储配置；root 不得为空。
     * @throws fr::Error 配置非法或初始化失败。
     */
    static std::unique_ptr<Storage> open(StorageConfig config);

    // ---- 上传会话（FR-UP-01..09） -------------------------------------

    /** 创建上传会话；受容量高水位与单用户会话上限约束（FR-GC-04、§7.3）。 */
    SessionInfo create_session(const std::string &ns, const std::string &name,
                               const std::string &version, uint64_t expected_size,
                               const std::string &expected_digest, const std::string &owner);

    /** 查询会话；不存在抛 FR_E_NOTFOUND。 */
    SessionInfo get_session(const std::string &session_id);

    /**
     * 上传一个数据块（FR-UP-05/06）：校验内容摘要、落块存储、登记分片。
     * 重复上传同编号同内容幂等；同编号不同内容抛 FR_E_CONFLICT。
     */
    ChunkAccept put_chunk(const std::string &session_id, uint64_t ordinal, const void *data,
                          size_t len);

    /**
     * 提交会话（FR-UP-08）：校验块数/顺序/总长/整体摘要后原子发布；
     * 校验失败会话回到 OPEN，不发布不完整制品（FR-STO-05）。
     */
    ArtifactInfo commit_session(const std::string &session_id);

    /** 终止会话：OPEN→ABORTED，删除分片登记；幂等（LIFE-02）。 */
    void abort_session(const std::string &session_id);

    /** 列出会话；state 为空时列出全部。 */
    std::vector<SessionInfo> list_sessions(const std::optional<SessionState> &state);

    // ---- 制品（FR-ART-01..06、FR-DL-01..04） --------------------------

    /** 查询制品（含清单）；不存在抛 FR_E_NOTFOUND。 */
    ArtifactInfo show_artifact(const std::string &ns, const std::string &name,
                               const std::string &version);

    /**
     * 流式读取制品范围 [offset, offset + length)（FR-DL-01/03）。
     * length 超出剩余长度时截断到文件尾；offset > size 抛 FR_E_RANGE；
     * offset == size 且 length == 0 合法（空读取）。
     */
    void read_artifact(const std::string &ns, const std::string &name, const std::string &version,
                       uint64_t offset, uint64_t length, const Sink &sink);

    /** 分页列出制品（FR-ART-06）；按 created_at 降序。 */
    std::vector<ArtifactInfo> list_artifacts(const std::optional<std::string> &ns,
                                             const std::optional<std::string> &name, int64_t limit,
                                             int64_t offset);

    /**
     * 删除制品（FR-GC-01）：先删元数据，再将为无引用的块标记待清理。
     *
     * @return 标记为待清理的块数。
     */
    int64_t delete_artifact(const std::string &ns, const std::string &name,
                            const std::string &version);

    // ---- 维护（FR-GC-02..05、FR-STO-06） ------------------------------

    /**
     * 执行块清理：仅删除「无制品引用、无活动会话引用且超保护期」的块。
     *
     * @param[in] dry_run true 时只统计不删除（FR-GC-05）。
     */
    GcReport collect_garbage(bool dry_run);

    /** 运行维护：过期会话清理 + 临时文件清理（供后台任务周期调用）。 */
    RecoveryReport run_maintenance();

    /** 当前活跃块字节数（容量计算口径）。 */
    uint64_t used_bytes();

private:
    Storage() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fr

#endif /* FR_STORAGE_STORAGE_HPP */
