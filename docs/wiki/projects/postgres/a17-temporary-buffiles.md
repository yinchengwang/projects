---
project: postgres
feature: temporary-buffiles
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/file/buffile.c
    symbols: [BufFileCreateTemp, BufFileCreateFileSet, BufFileOpenFileSet]
---

# 临时 BufFile

## 一句话职责

BufFile 为后端提供带缓冲、可跨多个物理临时文件扩展的逻辑文件接口。

## 机制是什么

BufFile 是建立在 PostgreSQL 虚拟文件层之上的临时文件抽象，主要供需要大量中间数据的算法使用。它把一个逻辑字节流映射到一个或多个物理段文件，提供缓冲读写、定位和 FileSet 命名等能力。普通临时 BufFile 由当前后端及 ResourceOwner 管理；FileSet/SharedFileSet 变体只有在参与者附接同一共享文件集并遵守导出/重开协议时，才能发现并读取共享段。两类用途共享分段 I/O 实现，但生命周期和访问主体不同。

它不是通用持久化文件系统，也不提供事务提交意义上的耐久性。普通临时文件由临时文件生命周期和资源所有者管理；FileSet/SharedFileSet 只解决受控场景中的共享与重开，不代表任意后端都能访问。调用方仍负责数据格式、并发协议和何时关闭/删除。底层文件描述符缓存与操作系统 I/O 位于 BufFile 之下。

## 怎么实现的

`BufFile` 保存分段文件句柄数组、当前段号和偏移、用户缓冲区位置、有效字节数、dirty/readOnly 状态以及 FileSet 名称等。普通 `BufFileCreateTemp` 通过 `OpenTemporaryFile` 建立首段，并记住创建时的 `ResourceOwner`；结构体由当前内存上下文分配，底层临时文件由资源所有者回收，错误退出时也能清理资源。

每个物理段上限为 1 GiB（与关系段大小概念不同）。写入缓冲区满或显式刷新时，`BufFileDumpBuffer` 将数据分段写出；跨段时通过 `extendBufFile` 创建下一文件，并将连续逻辑位置映射到新段。读取路径由 `BufFileLoadBuffer` 装入缓冲区，`BufFileReadCommon` 支持普通短读及要求精确字节数的变体。逻辑位置通过段号、段内偏移和缓冲区位置组合表达，缓冲区内回退定位也需在刷出时恢复正确位置。

FileSet 变体通过 `BufFileCreateFileSet` 以调用方命名创建首段；`BufFileOpenFileSet` 按递增段号探测现存文件，恢复逻辑文件的段集合。创建者必须先关闭或 `BufFileExportFileSet`，后者刷新缓存并令句柄只读，之后参与者再以只读方式打开。删除可逐段进行，通常由 FileSet 清理兜底。普通临时文件与文件集文件虽共用缓冲与分段机制，生命周期管理并不相同。

读写通过 `FileRead` / `FileWrite` 落到虚拟文件层，并记录临时块读写统计和可选 I/O 时延。关闭会刷新待写缓冲、关闭底层句柄并释放 BufFile 自身数组和结构；上层还需遵守对应文件集的所有权与共享约定。

## 关键取舍

BufFile 以单个逻辑流和固定大小缓冲减少虚拟文件层调用，同时将大文件拆段以绕过单文件大小限制并支持临时表空间分布。若直接使用单个普通文件，接口更简单但会受文件大小限制，且虚拟文件频繁打开关闭的成本更高；若改用关系存储，虽可获得数据库页与 WAL 管理，却会把可丢弃的中间结果引入不必要的事务存储路径。BufFile 牺牲通用随机访问和持久性，换取轻量临时 I/O。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/file/buffile.c` | `BufFileCreateTemp` | 创建由资源所有者管理的普通临时 BufFile |
| `src/backend/storage/file/buffile.c` | `BufFileCreateFileSet` | 创建可按 FileSet 名称发现的分段文件 |
| `src/backend/storage/file/buffile.c` | `BufFileOpenFileSet` | 探测并打开 FileSet 中既有段 |
| `src/backend/storage/file/buffile.c` | `BufFileDumpBuffer`、`BufFileLoadBuffer` | 在逻辑缓冲与物理段之间搬运数据 |
| `src/backend/storage/file/buffile.c` | `BufFileExportFileSet`、`BufFileClose` | 导出为只读及关闭回收 |

## 参考

- [PostgreSQL 官方文档：临时文件与排序溢写（work_mem、temp_file_limit）](https://www.postgresql.org/docs/current/runtime-config-resource.html#GUC-TEMP-FILE-LIMIT)
- [PostgreSQL 官方源码：BufFile 分段与 FileSet 实现](https://github.com/postgres/postgres/blob/master/src/backend/storage/file/buffile.c)

## 与主核 / AI 的关系

BufFile 是主核临时 I/O 的底层抽象之一；上层排序、哈希或并行执行组件决定数据语义与共享时序。解释文件存在不应推导出事务持久化，或推导出任意 worker 可读写。
