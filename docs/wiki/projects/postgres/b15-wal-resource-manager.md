---
project: postgres
feature: wal-resource-manager
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/rmgr.c
    symbols: [RmgrStartup, RmgrCleanup, RmgrNotFound]
---

# WAL rmgr 分派

## 一句话职责

`RmgrTable` 把 WAL record 头部的 `xl_rmid` 映射到一组资源管理器回调，供启动/清理阶段遍历，并在运行期遇到未知 ID 时将问题升级为错误。

## 机制是什么

每条 WAL record 的 `xl_rmid` 指出它由哪个 resource manager（rmgr）定义解释。`rmgr.c` 通过对 `access/rmgrlist.h` 做宏展开建立全局 `RmgrTable[RM_MAX_ID + 1]`：列表每项的 `rm_name`、redo、desc、identify、startup、cleanup、mask、decode 函数指针填入同一槽位。`RmgrData` 在 `xlog_internal.h` 里定义；`rm_name == NULL` 是该槽未注册的判据。表项索引就是 `RmgrId`，内建 ID 的数值来自列表顺序，扩展 ID 是另一段范围。

本页只讲这个**回调分派接口层**：静态内建表构造、startup/cleanup 全表遍历、未知 ID 错误。它不讲 rmgr 的 record 描述输出（b16）、各具体 rmgr redo 如何修改页面（b03）、WAL 记录的组装/写入过程（b01），也不展开逻辑解码回调细节。特别是 `RmgrStartup` / `RmgrCleanup` 不参与每条 WAL record 的 redo：它们只在模块级生命周期被显式调用。

扩展注册发生在 preload 阶段。`RegisterCustomRmgr` 检查名字非空、ID 位于 `RM_MIN_CUSTOM_ID..RM_MAX_CUSTOM_ID`、调用时正处于 `shared_preload_libraries` 初始化窗口，并且 ID 与名称都没有冲突；检查通过才把 `RmgrData` 整体复制入表。注册时序不是实现偶然，而是确保 postmaster 与所有 backend 看到一致函数表的接口约束。

## 怎么实现的

1. **声明表项形状与 ID 域。** `rmgr.h` 定义 `RmgrId` 为 `uint8`，并通过 `PG_RMGR` 再次 include `rmgrlist.h` 生成内建 `enum RmgrIds`。`RM_MAX_ID` 必须能装入 8 位；头文件注释说明改变宽度会影响 WAL 文件格式。`RM_MIN_CUSTOM_ID` 从 128 起，`RM_MAX_CUSTOM_ID` 是 `UINT8_MAX`。
2. **在一个位置展开内建表。** `rmgr.c` 定义自己的 `PG_RMGR` 宏，使列表每一行转成 `{ name, redo, desc, identify, startup, cleanup, mask, decode }` 初始化项，初始化全局 `RmgrTable[RM_MAX_ID + 1]`。必要的 rmgr 头文件在该编译单元集中 include，使各回调声明可见。
3. **启动/结束时按 ID 遍历。** `RmgrStartup` 从 0 到 `RM_MAX_ID` 扫描，先 `RmgrIdExists` 跳过空槽，再在 `rm_startup != NULL` 时调用；`RmgrCleanup` 同样跳空槽、仅对非空 cleanup pointer 调用。它不对单个 manager 做排序或异步并发，回调在遍历者的调用线程内同步执行。
4. **访问表项时统一检查注册状态。** `xlog_internal.h` 的 `GetRmgr(rmid)` 在 `RmgrIdExists` 为 false 时调 `RmgrNotFound`，否则返回该槽结构体副本。`RmgrNotFound` 发 `ERROR`，提示用户在 `shared_preload_libraries` 加入实现该 rmgr 的扩展。
5. **注册扩展项。** `RegisterCustomRmgr` 首先排除空名、范围外 ID 与错误调用时机，再扫表以大小写不敏感方式拒绝重复名称；所有检查过后以 `RmgrTable[rmid] = *rmgr` 一次性安装整份 vtable，最后记 LOG。`pg_get_wal_resource_managers` 也从同一表迭代，只返回已注册项，并标出 built-in/custom，说明目录视图与分派共用同一事实来源。

这里没有运行期对表加锁的路径：内建表编译时初始化，扩展只能在 preload 初始化阶段注册；此后表作为只读共享代码数据由每个 backend 使用。ID/名称冲突在注册时失败，缺失的 ID 在实际获取时失败，而非静默选择默认解释器。

## 关键取舍

**用固定宽度 ID 与稀疏数组，不用动态哈希表。** `RmgrId` 是 `uint8`，表直接按 ID 索引，空项用 `rm_name == NULL` 表示。放弃的是开放式任意数量的 rmgr 与运行期增删；换来的是 WAL record 只需携带紧凑 ID、读取回调时无需哈希计算或锁。代价是可用 ID 空间有限，且增加内建 ID 的顺序会影响协议值；这也是 `RM_MAX_ID` 必须落在 8 位范围内的原因。

**扩展只允许在 `shared_preload_libraries` 阶段注册。** 放弃的是会话级 `LOAD` 后立即安装 rmgr 的灵活性，因为 WAL 记录可能被任何 backend 或恢复进程读到，而 per-session 注册会让同一 ID 在同一集群各进程意义不同。替代方案是全局共享内存可变表并对每次查表加锁/版本控制，复杂度与启动恢复风险都更大。此限制不是推断：`RegisterCustomRmgr` 明确在 `!process_shared_preload_libraries_in_progress` 时报错，`RmgrNotFound` 又给出对应 preload hint。

**未知 ID 采取 hard error，不跳过或当作空记录。** 记录必须由正确的 rmgr 解码，否则 redo / 识别结果可能与磁盘状态不一致。放弃的是跨版本时对未知新记录的宽容读取；代价是旧二进制遇到新扩展产生的 ID 会停止，而不是带着不完整状态继续。真实 fail path 是 `GetRmgr` 调用 `RmgrNotFound`，后者 `ereport(ERROR)` 并要求加载实现该 rmgr 的扩展。

**启动/清理 callback 逐槽同步调用。** 不引入通用优先级、并发调度或依赖图，回调顺序就是 ID 顺序，表中空 callback 直接跳过。放弃的是复杂模块可能要求显式依赖排序的表达能力；替代方案会使每个 manager 还需声明依赖与失败回滚协议。当前源码没有这层抽象，调用者也没有等待/并发处理逻辑，因此文档不假定有。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/rmgr.c` | `RmgrStartup` | 启动生命周期入口：逐个跳过空槽，对有 `rm_startup` 回调的已注册 manager 同步调用初始化 |
| `src/backend/access/transam/rmgr.c` | `RmgrCleanup` | 清理生命周期入口：与启动遍历同样按 ID 检查注册项，只对非空 `rm_cleanup` 回调同步执行收尾 |
| `src/backend/access/transam/rmgr.c` | `RmgrNotFound` | 未知 `RmgrId` 的硬失败出口，抛出 ERROR 并提示 preload 对应扩展，避免以错误解释继续处理 WAL |

## 参考

- [PostgreSQL 官方源码：rmgr.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/rmgr.c) —— 内建表构造、startup/cleanup 遍历与 custom 注册校验
- [PostgreSQL 官方源码：rmgr.h](https://github.com/postgres/postgres/blob/master/src/include/access/rmgr.h) —— `RmgrId` 宽度与内建/扩展 ID 范围
- [PostgreSQL Wiki：Custom WAL Resource Managers](https://wiki.postgresql.org/wiki/CustomWALResourceManagers) —— 扩展如何申请稳定的全局唯一 rmgr ID

## 与主核 / AI 的关系

暂无直接对应。
