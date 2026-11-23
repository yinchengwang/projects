---
project: postgres
feature: relation-options
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/common/reloptions.c
    symbols: [add_reloption_kind, transformRelOptions, parseRelOptions]
---

# Relation options

## 一句话职责

Relation options 注册各类 relation 可接受的存储参数，将 SQL/catalog 中的文本键值安全解析成访问方法可直接读取的结构化配置。

## 机制是什么

Reloptions 的声明包括名字、类型、适用 kind、默认值/范围、字符串校验器与所需锁模式。内建和扩展 AM 用 kind 区分 heap、TOAST、各索引 AM 等参数族；扩展可调用 `add_reloption_kind` 取得新的 kind，再注册针对该 kind 的 bool、int、real、enum 或 string 参数。解析结果通常写入 `StdRdOptions` 或 AM 自己定义的字节结构，heap 的 `StdRdOptions` 包含 fillfactor、TOAST 配置以及 `AutoVacOpts` 一类 autovacuum 覆盖值。

`transformRelOptions` 处理 DDL/ALTER 的 `DefElem` 列表，生成保存在 catalog 的 text array 字符串形式，并按 namespace 替换旧值或保留未改的键。`parseRelOptions` 则按目标 kind 找到已注册选项，解析键值、应用默认值并可验证重复、未知选项、类型和值域。该层不规定 tuple 物理编码；fillfactor 等参数的消费者才在页空间策略、vacuum 调度等处解释配置。

## 怎么实现的

1. 注册时，`add_reloption_kind` 将下一个可用 bit 左移分配给扩展 kind，并在达到 `RELOPT_KIND_MAX` 时报告超限；`add_reloption` 将 option 元数据放入 TopMemoryContext 中的动态表，并标记需要重建解析索引。
2. 每个 `relopt_gen` 带 option 名、kind mask、类型、锁模式和名字长度；对应类型结构还持有 default/min/max、enum 成员或 string validator。kind mask 允许参数仅对特定关系类型有效，而同名参数可分别属于 heap、btree 等不同 kind。
3. DDL 侧 `transformRelOptions` 遍历已有 text array 与新 `DefElem`：同 namespace 同名旧项被替换，其他旧项保留；检查 namespace 是否被允许，RESET 禁止携带值；新选项序列化为 `name=value`，不写 namespace 到 catalog array。无显式参数值时按布尔 true 解释。
4. 读配置时 `parseRelOptions` 根据 `kind` 选出候选定义，先把每个 option 置为未显式提供，再由 `parseRelOptionsInternal` 匹配 text array 项。启用 validate 时对重复项、未知名称、bool/int 范围、enum 成员、字符串回调执行错误检查；不验证模式用于读已存 catalog 值，避免把历史合法配置按新输入流程处理。
5. `build_reloptions` 按选项表和结构体字段偏移分配解析结果、填入默认值/已设置值；local reloptions 可额外执行整组 validator。字符串值采用独立或尾随分配，调用方需按返回结构生命周期管理。
6. `heap_reloptions` 按 relation kind 选 HEAP 或 TOAST kind 的参数集合；TOAST 关系还调整默认 fillfactor 与 analyze 阈值。实际打开关系时解析结果挂在 `rd_options`，消费者通过 relation option accessor 或结构字段读取，而不是每次重新解析 text array。

## 关键取舍

- 源码事实：catalog 存储形式是可变文本数组，而运行时按 kind 解析为字段结构；`parseRelOptions` 支持 `validate` 开关。架构推论：灵活可扩展的 catalog 表示与运行期高效读取分离，代价是每次构建 relcache 配置要做一次解析；若直接在 catalog 增加每种 option 专用列，查询快但扩展 AM/新参数需要系统目录迁移。
- 源码事实：`add_reloption_kind` 用 bit mask 分配 kind，选项按 kind 筛选；同名 `fillfactor` 可因 AM kind 而有不同定义。架构推论：一套 parser 可服务多 AM 而保留参数语义边界；若用全局名字唯一表，索引与 heap 的同名参数将冲突或必须人为加前缀。
- 源码事实：`transformRelOptions` 保留 namespace 区分并按 namespace 替换，namespace 不写进最终 `name=value` 文本；未知或无效 namespace 在转换时拒绝。架构推论：namespace 是 SQL 输入路由与验证边界，不是持久化 option key 的组成部分；若省略 namespace 校验，不同扩展的选项可能被静默接收或覆盖。
- 源码事实：每个选项元数据记录 lockmode，整数、实数和字符串有各自验证路径，整组本地选项可注册 validator。架构推论：参数可声明修改时所需锁强度，并兼顾单字段与跨字段约束；统一只做字符串解析会把非法组合推迟到实际消费处，错误更难定位。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/common/reloptions.c` | `add_reloption_kind` | 为扩展访问方法发放新的 kind bit，供之后注册与解析其私有选项族。 |
| `src/backend/access/common/reloptions.c` | `transformRelOptions` | 将 DDL `DefElem` 按 namespace 与旧 text array 合并、校验并序列化为 catalog 形式。 |
| `src/backend/access/common/reloptions.c` | `parseRelOptions` | 按目标 kind 筛选注册表并从存储文本恢复带默认值/设置标志的结构化选项。 |

## 与主核 / AI 的关系

主核使用自身配置结构控制对象与索引行为，并无 PostgreSQL `reloptions` text-array + AM kind parser 的直接对应。可参考集中注册、类型校验和运行期结构化配置的模式；不要将 PostgreSQL fillfactor 或 autovacuum 参数名视作主核已有配置。

## 参考

- [PostgreSQL 文档：CREATE TABLE 存储参数](https://www.postgresql.org/docs/current/sql-createtable.html#SQL-CREATETABLE-STORAGE-PARAMETERS) —— 说明 relation storage parameters 的 SQL 设置入口。
- [PostgreSQL 官方源码：relation options parser](https://github.com/postgres/postgres/blob/master/src/backend/access/common/reloptions.c) —— 支撑 kind 注册、选项变换与解析/验证流程。
