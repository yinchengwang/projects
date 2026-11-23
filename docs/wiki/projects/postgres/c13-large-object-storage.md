---
project: postgres
feature: large-object-storage
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/large_object/inv_api.c
    symbols: [inv_create, inv_open, inv_write]
---

# 大对象存储

## 一句话职责

大对象接口把一个逻辑 OID 映射为 `pg_largeobject` 中按页号分片的事务性字节流，并提供带权限与游标状态的读写描述符。

## 机制是什么

`inv_create` 建立大对象 metadata 记录，不立即分配数据页；数据按 `(loid, pageno)` 存在 `pg_largeobject`，页数据是 `bytea`。`LargeObjectDesc` 保存对象 OID、当前 offset、访问标志和读取快照，`inv_open` 做存在性与 ACL 检查后构造描述符，后续读写通过 catalog heap 和其 B-tree 索引访问这些分片。

大对象接口适合需要独立 OID、随机 seek/read/write 的内容；它不等同于普通行内字段，也不由 TOAST 的自动行内值外置机制代替。虽然大对象分片的 `bytea` 数据可能被压缩，但逻辑对象的页号、offset、权限与事务接口由 large-object API 管理。本页不讲 TOAST 内部 tuple 压缩/外置，也不讲 SQL 客户端协议包装。

## 怎么实现的

1. `inv_create` 调 `LargeObjectCreate` 在 `pg_largeobject_metadata` 插入 metadata 与 owner/ACL，再记录依赖、调用对象创建 hook 并推进 command counter；数据页保持空，因此对象可存在且长度为零。
2. `inv_open` 将读写请求转换为 descriptor flags：写标志同时允许读取描述符，写请求使用瞬时快照，纯读使用当前活动快照；随后检查对象存在与 SELECT/UPDATE 权限，按调用方指定 memory context 分配 `LargeObjectDesc` 并将 offset 置零。
3. `open_lo_relation` 在事务内懒打开 `pg_largeobject` 与 `(loid,pageno)` 索引，并把 relation 引用归属到 top transaction resource owner。由此多个 API 调用共享 relation 引用，事务提交时关闭，abort 清理由资源所有权路径处理。
4. `inv_write` 检查写权限、请求长度与最大偏移，按当前 offset 得到 pageno 和页内 offset；有序扫描从 `(loid, pageno >= 当前页)` 取后续分片。覆盖已有页时读出并填补中间空洞，再更新该 tuple；没有对应页则零填空洞并插入新 tuple。
5. 每次循环最多写当前页剩余空间，然后推进 offset 与页号继续；写完关闭系统扫描与索引状态并 `CommandCounterIncrement`，使本事务后续大对象操作看到刚刚写入/更新的数据。
6. `inv_read` 同样按页号有序读取；遇到缺页时向输出缓冲补零，遇到有数据页时按页内 offset 拷贝可用部分并前移描述符游标。读取/写入底层 tuple 使用 heap/catalog 更新，因此受事务提交回滚语义管理。

## 关键取舍

- **稀疏页表表示大对象，而非一条连续、必须完整分配的存储对象。** `inv_read` 将缺失页区读作零，`inv_write` 可跨空洞写入并只创建实际涉及的分片。放弃连续文件式直接寻址的简单性，以利用 catalog 行、索引与事务更新；缺点是随机定位依赖索引扫描，且每个页块有 tuple/index 元数据开销。
- **写描述符用瞬时快照，读描述符保留活动快照。** 源码事实：`inv_open` 对写请求将 snapshot 设为 NULL；读请求取 `GetActiveSnapshot()`，API 注释还要求 snapshot 若需跨调用存活必须注册/延长生命周期。此区分避免写操作用旧快照来定位要更新的数据，同时让读语义由调用者的活动快照决定。
- **每个大对象数据页的逻辑块大小取 `LOBLKSIZE`，而非尽量填满堆页。** `large_object.h` 定义其为 `BLCKSZ / 4`，源码注释说明此选择有利于部分页写入减少 tuple 更新，并且页 tuple 足以触发 inline compression 机会；代价是页内留白和较多分片条目，换来更新局部化与客户端常用幂次块传输的适配。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/storage/large_object/inv_api.c` | `inv_create` | 创建逻辑对象 metadata、owner 依赖并推进 command counter |
| `src/backend/storage/large_object/inv_api.c` | `inv_open` | 选择读/写 snapshot、验证存在与 ACL，并初始化游标描述符 |
| `src/backend/storage/large_object/inv_api.c` | `inv_write` | 按页号更新或插入分片，处理页内位置与间隙零填充 |
| `src/backend/storage/large_object/inv_api.c` | `inv_read` | 有序读取页分片、对稀疏缺页输出零并推进游标 |
| `src/backend/catalog/pg_largeobject.c` | `LargeObjectCreate` | 在 metadata catalog 中建立无数据页的逻辑对象记录 |
| `src/include/catalog/pg_largeobject.h` | `FormData_pg_largeobject` | 定义数据页 tuple 的 OID、页号与变长 bytea 字段及唯一索引键 |

## 与主核 / AI 的关系

暂无直接对应。大型内容若选择显式对象 API，调用方必须管理 OID、ACL 与事务游标；不要把它和自动 TOAST 外置混称为同一存储路径。

## 参考

- [PostgreSQL 官方源码：大对象 API](https://github.com/postgres/postgres/blob/master/src/backend/storage/large_object/inv_api.c) —— 支撑打开权限、游标读写、稀疏页与事务 catalog 更新
- [PostgreSQL 官方源码：大对象 catalog 结构](https://github.com/postgres/postgres/blob/master/src/include/catalog/pg_largeobject.h) —— 支撑分片 tuple 字段与 `(loid,pageno)` 索引
- [PostgreSQL 官方源码：大对象 metadata 创建](https://github.com/postgres/postgres/blob/master/src/backend/catalog/pg_largeobject.c) —— 支撑逻辑对象从 metadata 记录起始
