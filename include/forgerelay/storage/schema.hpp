// fr_schema.hpp - 元数据数据库迁移（DB-03：user_version 单调递增，单事务执行）。
//
// V1（M2）：按需求 §6 建立全部 8 个实体表；user/token/audit 的业务 API 在
// M4 落地，但 schema 一次成形，后续演进走新版本号。
#ifndef FR_STORAGE_SCHEMA_HPP
#define FR_STORAGE_SCHEMA_HPP

#include "forgerelay/storage/db.hpp"

namespace fr {

/** 当前 schema 最新版本号。 */
constexpr int64_t kSchemaVersion = 1;

/**
 * 将数据库迁移到 kSchemaVersion；已是最新则为无害操作。
 *
 * @param[in,out] db 已打开的数据库。
 * @throws fr::Error 任何 SQL/事务失败（迁移原子回滚）。
 */
void migrate(Database &db);

} // namespace fr

#endif /* FR_STORAGE_SCHEMA_HPP */
