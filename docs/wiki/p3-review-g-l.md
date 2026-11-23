# G–L 抽检报告

## 范围与方法

本报告记录 G–L 范围内抽检的四页：G01、I10、J12、L09。逐条对照页面描述与 manifest 中声明的源码文件/符号；页面行号仅用于定位被审页面，源码对照使用相对仓根路径与符号名。未对整个 G–L 范围逐页审计，不将抽样结论外推为全组通过。

## 抽检结果

| 页面 | 结论 | 行号与源码对照 |
|---|---|---|
| `g01-catalog-storage-bootstrap.md` | 抽检段落与当前源码相符。 | 页面第 23 行称 `IsCatalogRelation` 以 pinned OID 边界判定且不访问目录；对应 `src/backend/catalog/catalog.c` 的 `IsCatalogRelation` / `IsCatalogRelationOid`，后者按 `FirstUnpinnedObjectId` 比较。页面第 36 行描述 `GetNewOidWithIndex` 在 bootstrap 模式直接分配、常规模式用 `SnapshotAny` 查重；对应同文件 `GetNewOidWithIndex`。页面第 35 行关于目录 heap 插入后同步维护索引，对应 `src/backend/catalog/indexing.c` 的 `CatalogTupleInsert`。 |
| `i10-archive-and-base-backup.md` | 抽检段落与当前源码相符。 | 页面第 35 行描述 `SendBaseBackup` 解析选项并设置备份状态，对应 `src/backend/backup/basebackup.c` 的 `SendBaseBackup`。页面第 36–37 行描述 `perform_base_backup` 的备份模式、tablespace 打包及可选 WAL 追加，对应同文件 `perform_base_backup`。页面第 31–33 行归档状态与失败重试说明，按 refs 应继续核对 `src/backend/postmaster/pgarch.c` 的 `pgarch_archiveXlog` / `PgArchiverMain`；本次抽检未对这段做逐分支完整复核。 |
| `j12-session-and-guc.md` | 复核期间发现的并发版本问题已修复；当前版本复核未再确认该问题。 | 页面第 34–40 行覆盖初始化、来源优先级、事务栈和自定义变量替换；与 `src/backend/utils/misc/guc.c` 的 `InitializeGUCOptions`、`SetConfigOption`、`define_custom_variable` 及 `set_config_with_handle` 相对照，SQL 接线对应 `src/backend/utils/misc/guc_funcs.c` 的 `ExecSetVariableStmt`。**说明：并发版本问题在复核期间已由页面作者修复；此报告记录修复状态，不把已修订内容作为当前缺陷。** |
| `l09-regex-and-fulltext.md` | 核心调用链抽检与当前源码相符；缓存策略细节未逐分支完整复核。 | 页面第 29–31 行关于 `RE_compile_and_execute` 与 `textregexeq`，对应 `src/backend/utils/adt/regexp.c` 的同名符号；页面第 34 行 `TS_execute` 的三值结果语义，对应 `src/backend/utils/adt/tsvector_op.c` 的 `TS_execute`；页面第 35 行文本匹配入口，对应同文件 `ts_match_tt`；页面第 33 行查询解析，对应 `src/backend/utils/adt/tsquery.c` 的 `parse_tsquery`。 |

## 复核说明

- `g01`、`i10`、`l09` 行号用于记录页面与源码的对照点；源码位置以文件与符号名标识，不将源码行号当作稳定引用。
- G01：当前 `IsCatalogRelationOid` 实际定义以 `relid < FirstUnpinnedObjectId` 判定；`GetNewOidWithIndex` 的 bootstrap 分支直接返回新对象 OID，常规分支以索引扫描并使用 `SnapshotAny`。
- I10：页面对基础备份调用顺序的描述与 `SendBaseBackup` → `perform_base_backup` 入口相符；归档状态机属于另一源码文件，本轮仅作有限抽检，不能据此宣称已完整验证其全部重试和错误分支。
- L09：`RE_compile_and_execute` 在 `nmatch < 2` 时增加 `REG_NOSUB`；`TS_execute` 将递归结果不等于 `TS_NO` 视为匹配；`ts_match_tt` 先调用 `to_tsvector` 与 `plainto_tsquery` 再进入向量查询匹配。
- J12：将复核期间的并发版本问题记为**已修复**；如需审计修复本身的前后差异，应以版本历史/diff 为准，本报告不声称已取得该历史差异。

## 抽检边界

本报告是四页抽样记录，不是 G–L 全量验收；未抽到页面、未逐分支复核的行为仍需另行检查。未修改任何被审页面。
