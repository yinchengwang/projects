---
project: sqlite
feature: hash-table
page_type: feature
schema_version: 1
refs:
  - path: src/hash.c
    symbols: [sqlite3HashInit, sqlite3HashClear, sqlite3HashInsert, sqlite3HashFind]
---

# 哈希表

## 一句话职责

给 SQLite 全库提供一套按字符串键索引的通用符号表：负责插入、查找、删除与桶数调整，让模式对象（表 / 索引 / 触发器 / 外键）、连接级函数、排序规则与虚拟表模块都能按名字 O(1) 命中。

## 机制是什么

`src/hash.c` 实现的是 **open addressing 之外的链地址法**（separate chaining）：哈希表本体是一个 `struct _ht` 桶数组，每个桶挂一条链表；但所有元素同时串在**一条全局双向链表**上（`Hash.first` 指向表头），桶只保存"链首指针 + 元素个数"。

这套"链表为主、桶数组为辅"的结构有两个直接后果，源码注释 `src/hash.h` 里写明了：

- **小表不建桶数组**。`Hash.htsize` 与 `Hash.ht` 可以都是 0，此时查找退化为对全局链表的**线性扫描**。`src/hash.h` 的注释给出的理由是：元素很少时线性搜索比维护哈希表更快。
- **桶链是全局链的子链**。`src/hash.h` 注释描述为：桶的内容是"桶指向的那个元素，加上全局链上紧随其后的 `_ht.count-1` 个元素"。`insertElement` 正是按这个约定把元素接到桶链与全局链上的。

键的语义是**不区分 ASCII 大小写**的：`findElementWithHash` 先比较哈希值 `h==elem->h`，再调 `sqlite3StrICmp` 比较键字符串。哈希函数 `strHash` 本身也做了大小写折叠——它对每个字节取 `0xdf` 掩码后再累乘（EBCDIC 构建下是 `0xbf`），掩码 `0xdf` 清掉了 ASCII 的 0x20 位，因此 `'A'` 与 `'a'` 折叠成同一个值。

边界上，本模块只负责**一张哈希表内部**的插入 / 查找 / 删除 / 扩容；不负责模式对象（`Table` / `Index` / `Trigger` / `FKey`）本身的生命周期管理，也不负责 B-tree 存储。

## 怎么实现的

### 数据结构

`src/hash.h` 定义：

- `struct Hash`：`htsize`（桶数）、`count`（元素总数）、`first`（全局双向链表头）、`ht`（`struct _ht` 桶数组指针）。
- `struct _ht`：`count`（落在本桶的元素数）、`chain`（本桶链首，指向全局链上的某个元素）。
- `struct HashElem`：`next` / `prev`（全局双向链表指针）、`data`（用户数据）、`pKey`（键指针，**不拷贝字符串**）、`h`（键的哈希值）。

遍历用的宏在 `src/hash.h`：`sqliteHashFirst(H)` 取链表头、`sqliteHashNext(E)` 取下一个、`sqliteHashData(E)` 取数据、`sqliteHashCount(H)` 取元素总数。

### 初始化与清空

`sqlite3HashInit` 把 `first`、`count`、`htsize` 全置 0，`ht` 置 0——即"空表且无桶数组"的初始态。

`sqlite3HashClear` 释放桶数组（`sqlite3_free(pH->ht)` 后置 `ht=0`、`htsize=0`），然后沿全局链逐个 `sqlite3_free` 掉 `HashElem`，最后 `count=0`。

### 插入

`sqlite3HashInsert(pH, pKey, data)` 的返回值语义有三种，源码注释写得很明确：

1. **键不存在且 `data!=0`**：新建 `HashElem`（`sqlite3Malloc(sizeof(HashElem))`），填入 `pKey` / `h` / `data`，`pH->count++`，然后调 `insertElement` 挂链，返回 0。
2. **键已存在且 `data!=0`**：把 `elem->data` 换成新 `data`，`elem->pKey` 换成新 `pKey`，返回**旧数据指针**。
3. **`data==0`**：这是**删除语义**。键不存在时直接返回 0；键存在时调 `removeElement` 摘链，返回**被删元素的旧数据指针**。

其中"键已存在"的判定不是"找到同名"，而是 `elem->data` 非空——`findElementWithHash` 找不到时返回的是函数内 `static HashElem nullElement = { 0, 0, 0, 0, 0 }`，其 `data` 字段为 0。

**扩容判定**在插入路径里：`if( pH->count>=5 && pH->count > 2*pH->htsize )` 才调 `rehash(pH, pH->count*3)`。两个条件缺一不可——`count>=5` 是"表足够大才值得建桶"的门槛，`count > 2*htsize` 是"负载因子超过 2 才扩容"。

**内存不足时**：`sqlite3Malloc` 返回 0，`sqlite3HashInsert` 直接 `return data`，**哈希表保持不变**。调用方靠"返回值等于自己刚传进去的 data"来识别失败。

### 查找

`sqlite3HashFind(pH, pKey)` 是 `findElementWithHash(pH, pKey, 0)->data` 的薄封装——`pHash` 传 0 表示不需要回填哈希值。

`findElementWithHash` 的查找范围按桶数组是否存在分两种：

- **有桶数组**（`pH->ht` 非 0）：定位桶 `&pH->ht[h % pH->htsize]`，从 `pEntry->chain` 开始，沿 `elem->next` 走 `pEntry->count` 步。
- **无桶数组**（`pH->ht` 为 0）：从全局链表头 `pH->first` 开始，走 `pH->count` 步。

两条路径的循环上限都来自结构体里记录的 `count` 字段（桶内计数或表内总数），而不是走到 NULL 为止。

### 删除

`removeElement(pH, elem)` 做四件事：从全局双向链摘除（`elem->prev->next = elem->next` / `pH->first = elem->next`，同时维护对侧的 `prev`）；若有桶数组，把 `elem->h % pH->htsize` 对应桶的 `chain` 指针推进到 `elem->next`（仅当 `pEntry->chain==elem`），并 `pEntry->count--`；`sqlite3_free(elem)` 释放元素；`pH->count--`。

**表被删空时会连带释放桶数组**：`if( pH->count==0 )` 则调 `sqlite3HashClear(pH)`，把 `ht` 与 `htsize` 一并清零，回到"无桶数组"的初始态。

### 扩容（rehash）

`rehash(pH, new_size)` 的流程：

1. **软上限裁剪**：`#if SQLITE_MALLOC_SOFT_LIMIT>0` 分支下，若 `new_size*sizeof(struct _ht) > SQLITE_MALLOC_SOFT_LIMIT`，就把 `new_size` 收敛为 `SQLITE_MALLOC_SOFT_LIMIT/sizeof(struct _ht)`。`SQLITE_MALLOC_SOFT_LIMIT` 定义在 `src/sqliteInt.h`，未显式定义时**默认值为 1024**。裁剪后若 `new_size==pH->htsize` 则直接返回 0，不做任何事。
2. **分配新桶数组**：用 `sqlite3BeginBenignMalloc()` / `sqlite3EndBenignMalloc()` 包住 `sqlite3Malloc(new_size*sizeof(struct _ht))`。源码注释说明了两点：分配失败只是性能损失而非致命错误，所以标记为 benign；用 `sqlite3Malloc` + `memset(0)` 而不是 `sqlite3MallocZero`，是因为 `sqlite3MallocZero` 只清零请求的字节数，而本模块要用的是实际分配到的空间。
3. **分配失败**：返回 0，**旧桶数组保持不变**，调用方 `sqlite3HashInsert` 不检查这个返回值，继续把新元素插进旧桶数组。
4. **换桶并重挂**：`sqlite3_free(pH->ht)` 释放旧桶数组，`pH->ht = new_ht`；`pH->htsize = new_size = sqlite3MallocSize(new_ht)/sizeof(struct _ht)`——桶数按**实际分配到的字节数**回算，而不是沿用请求值。`memset` 清零后，遍历全局链表，把每个元素按 `elem->h % new_size` 重新 `insertElement` 到新桶。
5. 全部成功返回 1。

`rehash` 是 `src/hash.c` 里的 `static` 函数，唯一调用点是 `sqlite3HashInsert` 里的扩容判定。

### 真实调用方与它们的失败处理

以下调用方都按 `sqlite3HashInsert` 的三态返回值处理，代码可查：

- `src/callback.c` 的 `findCollSeqEntry`：查 `db->aCollSeq`（排序规则表）。创建新项时插入，注释明写"若 `sqlite3HashInsert` 发生 malloc 失败，它会返回那个没被加进哈希表的 `pColl` 指针"，于是 `if( pDel!=0 )` 时调 `sqlite3OomFault(db)` 并 `sqlite3DbFree(db, pDel)`。
- `src/callback.c` 的 `sqlite3FindFunction`（查找应用自定义函数的路径）：查 `db->aFunc`（连接级函数表）。注册新函数时 `sqlite3HashInsert(&db->aFunc, pBest->zName, pBest)`，若返回 `pOther==pBest` 说明 malloc 失败，就 `sqlite3DbFree(db, pBest)` + `sqlite3OomFault(db)`。
- `src/build.c` 的 `sqlite3UnlinkAndDeleteIndex`：`sqlite3HashInsert(pHash, zIdxName, 0)` 用"传 0 即删除"的语义取出 `Index`，再 `sqlite3FreeIndex`。
- `src/build.c` 的 `sqlite3UnlinkAndDeleteTable`：`sqlite3HashInsert(&pDb->pSchema->tblHash, zTabName, 0)` 取出 `Table`，再 `sqlite3DeleteTable`。
- `src/trigger.c` 的 `sqlite3CreateTrigger`：`sqlite3HashInsert(pHash, zName, pTrig)` 写入 `pSchema->trigHash`，返回非 0 则 `sqlite3OomFault(db)`。
- `src/select.c` 的 `sqlite3ColumnsFromExprList`：`sqlite3HashInit(&ht)` 起一张局部表，用 `sqlite3HashFind` 检测结果列名冲突并追加后缀去重，用 `sqlite3HashInsert(&ht, zName, pX)` 登记；若插入返回值 `==pX` 则 `sqlite3OomFault(db)`；函数末尾 `sqlite3HashClear(&ht)` 释放。
- `src/callback.c` 的 `sqlite3SchemaClear`：把 `pSchema->tblHash` / `trigHash` 拷到临时 `Hash` 变量，`sqlite3HashInit` 重置原表，再遍历临时表逐个 `sqlite3DeleteTrigger` / `sqlite3DeleteTable`，最后 `sqlite3HashClear` 两张临时表。
- `src/main.c` 的连接关闭路径：`sqlite3HashClear(&db->aFunc)` / `sqlite3HashClear(&db->aCollSeq)` / `sqlite3HashClear(&db->aModule)`。
- `src/vtab.c` 的 `sqlite3_create_module`：`sqlite3HashInsert(&db->aModule, zCopy, (void*)pMod)` 登记虚拟表模块。

## 关键取舍

- **小表不建桶，用线性扫描**。**源码事实：**`findElementWithHash` 在 `pH->ht` 为 0 时从 `pH->first` 线性扫；`sqlite3HashInsert` 的扩容判定要求 `count>=5` 才可能建桶；`src/hash.h` 注释写明"元素很少时线性搜索比维护哈希表更快"。**架构推论：**代价是表小的时候查找是 O(n) 而非 O(1)；换来的是省掉一次桶数组的 malloc 与一整轮 rehash 重挂——SQLite 的模式表在绝大多数连接上只有个位数条目。
- **桶数组软上限 1024 字节**。**源码事实：**`rehash` 里 `#if SQLITE_MALLOC_SOFT_LIMIT>0` 分支把 `new_size*sizeof(struct _ht)` 裁到 `SQLITE_MALLOC_SOFT_LIMIT`，该常量在 `src/sqliteInt.h` 未定义时取 `1024`。**架构推论：**等价于给桶数封顶——`sizeof(struct _ht)` 是两个 32 位指针域，按 `struct _ht` 的实际布局算，1024 字节能装下约 128 个桶。哈希表永远长不大，冲突链可以很长，但内存占用有硬天花板。
- **扩容失败不算错误**。**源码事实：**`rehash` 里 `sqlite3Malloc` 失败时返回 0，`sqlite3HashInsert` 不检查这个返回值，继续用旧桶数组插入；`sqlite3BeginBenignMalloc` / `sqlite3EndBenignMalloc` 的包裹明确把这标记为良性分配失败。**架构推论：**语义上把"哈希性能退化"与"操作失败"解耦——调用方只需处理元素节点本身分配失败（`sqlite3HashInsert` 返回 `data` 那一路），不必区分是桶数组没扩起来还是整个插入失败。
- **键不拷贝，生命周期由调用方负责**。**源码事实：**`sqlite3HashInsert` 的注释明写"The key is not copied in this instance"，`HashElem.pKey` 只是存指针；`sqlite3HashInsert` 在覆盖已有键时还会把 `elem->pKey` 一起换成新指针。**架构推论：**省掉每次插入一次字符串拷贝，代价是键字符串必须活得比哈希表长——`src/callback.c` 的 `sqlite3FindFunction` 注册时把函数名转成小写后连同 `FuncDef` 一起分配，靠 `FuncDef` 的存活来保证键有效。
- **查找失败返回一个静态空元素而不是 NULL**。**源码事实：**`findElementWithHash` 找不到时返回 `&nullElement`（`static HashElem nullElement = { 0, 0, 0, 0, 0 }`），其 `data` 为 0；`sqlite3HashFind` 因此返回 NULL，而 `sqlite3HashInsert` 用 `elem->data` 是否非空来区分"找到"与"没找到"。**架构推论：**调用方（`src/callback.c`、`src/build.c`、`src/trigger.c`、`src/vtab.c`）统一用"返回指针是否为 0"判断命中，不需要额外的 found 输出参数。
- **删除操作复用插入接口**。**源码事实：**`sqlite3HashInsert` 传 `data==0` 即删除；`src/build.c` 的 `sqlite3UnlinkAndDeleteIndex` / `sqlite3UnlinkAndDeleteTable`、`src/trigger.c` 的 `sqlite3CreateTrigger` 都用这一形态。**架构推论：**接口面收窄成三个动作（init / insert-or-delete / find），但代价是"删除"与"插入"共用一条代码路径，语义要靠参数取值来区分。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/hash.c | sqlite3HashInit | 把 `Hash` 置为空表初始态：`first` / `count` / `htsize` / `ht` 全为 0。 |
| src/hash.c | sqlite3HashClear | 释放桶数组并沿全局链逐个释放 `HashElem`，把表重置为空。 |
| src/hash.c | sqlite3HashInsert | 插入 / 覆盖 / 删除三合一入口；返回旧数据指针，分配失败时返回传入的 `data`。 |
| src/hash.c | sqlite3HashFind | 按键查找并返回 `data`；内部走 `findElementWithHash`，键比较用 `sqlite3StrICmp`。 |
| src/hash.c | strHash | 哈希函数；逐字节取 `0xdf`（EBCDIC 下 `0xbf`）掩码后累乘 `0x9e3779b1`，对 ASCII 大小写不敏感。 |
| src/hash.c | insertElement | 同时把元素挂到全局双向链表与指定桶链上。 |
| src/hash.c | rehash | 重新分配桶数组并重挂全部元素；受 `SQLITE_MALLOC_SOFT_LIMIT` 裁剪，分配失败返回 0 且保留旧桶。 |
| src/hash.c | findElementWithHash | 按桶链或全局链查找匹配元素；找不到时返回静态 `nullElement`。 |
| src/hash.c | removeElement | 从全局链与桶链摘除元素并释放；表被删空时调 `sqlite3HashClear` 释放桶数组。 |
| src/hash.h | struct Hash / struct _ht / struct HashElem | 哈希表、桶、元素三个结构体的定义与遍历宏 `sqliteHashFirst` / `sqliteHashNext` / `sqliteHashData` / `sqliteHashCount`。 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 里的目录/元数据映射与 SQLite 这一层不是同一套机制：SQLite 的哈希表是**单进程内的名字 → 指针**映射，键不拷贝、生命周期与模式对象绑定、扩容失败可容忍，主核若要抄这一层，值得抄的是"小表不建桶 + 键不拷贝"这两条省内存的工程决定，而不是它的桶数软上限 1024 字节——那个上限来自 SQLite 单连接内存预算的现实，不是通用设计。

**架构推论：** 本页可作为 AI 检索 SQLite 内部机制时的落点：官方架构文档 `arch.html` 把 "Symbol tables used by the parser are maintained by hash tables found in hash.c" 明确写在 Utilities 一节，因此凡是涉及"按名字找模式对象 / 连接级函数 / 排序规则 / 虚拟表模块"的问题，检索落点都应该指向 `src/hash.c` 与 `src/hash.h`，而不是 B-tree（F 组）或模式对象管理（B05）。

## 参考

- SQLite 架构文档（Utilities 一节明确指出符号表由 `hash.c` 的哈希表维护）：<https://sqlite.org/arch.html>
- SQLite 官方 Fossil 源码 `src/hash.c`（trunk）：<https://sqlite.org/src/file?name=src/hash.c&ci=trunk>
- SQLite 官方 Fossil 源码 `src/hash.h`（结构体与遍历宏定义）：<https://sqlite.org/src/file?name=src/hash.h&ci=trunk>
- GitHub 源码镜像 `src/hash.c`：<https://github.com/sqlite/sqlite/blob/master/src/hash.c>
- GitHub 源码镜像 `src/hash.h`：<https://github.com/sqlite/sqlite/blob/master/src/hash.h>
