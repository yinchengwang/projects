---
project: sqlite
feature: btree-cell-access
page_type: feature
schema_version: 1
refs:
  - path: src/btree.c
    symbols: [sqlite3BtreePayload, sqlite3BtreePayloadChecked, sqlite3BtreePayloadSize, accessPayload]
---

# B-tree 单元与记录访问

## 一句话职责

把「游标当前指向的某个 B-tree 单元」翻译成「一段可以直接交给上层的字节」：先在页内定位并解析单元头（得到 payload 总长与本地留存长度），再按需把散落在溢出页上的字节拼接回连续缓冲区。

## 机制是什么

B-tree 的一个单元（cell）在磁盘上是变长的：一段单元头加上 payload 字节。表 B-tree 的单元头是「payload 长度 varint + rowid varint」，索引 B-tree 的单元头是「payload 长度 varint」（内部节点还要前置 4 字节子页指针）。payload 本身**不一定**全部落在这一页上：超长记录的尾部被切到一串溢出页里，页内只保留前一段（本地部分），单元头末尾追加 4 字节溢出页号。

因此「读取一个单元的 payload」实际是两件事的叠加：

1. **页内定位与解析**：`btreeParseCellPtr` / `btreeParseCellPtrIndex` 把单元头解码进 `CellInfo`，核心字段是 `nPayload`（payload 总字节数）、`nLocal`（本地留存字节数）、`pPayload`（payload 首字节指针）、`nSize`（单元总长）。
2. **跨页拼接**：`accessPayload` 按 `offset`/`amt` 从「本地部分 + 溢出链」中取字节；必要时把溢出链的页号缓存进 `BtCursor.aOverflow`，让后续的随机偏移读取不必每次重走链表。

这一层之上还有一层：**记录（record）格式**。表单元的 payload 本身是 SQLite 自有的记录格式——首字节是记录头长度，随后是一串 serial type（字段类型串），再往后是各字段的值体。`OP_Column` 走的是「本地 payload 直接零拷贝」路径（`sqlite3BtreePayloadFetch`），只有当记录大到溢出时，才回退到 `sqlite3BtreePayload` 把整条记录读进缓冲区再逐字段解析。

## 怎么实现的

### 1. 单元头的解析：`CellInfo` 的四个字段

`btreeParseCellPtr`（表 B-tree 叶节点）内联展开 varint 解码，先取 `nPayload`，再取 rowid（`nKey`），随后：

- 若 `nPayload <= pPage->maxLocal`：整条 payload 都在本地，`nLocal = nPayload`，`nSize = nPayload + (pIter - pCell)`，且 `nSize` 下限被夹到 4。
- 否则转入 `btreeParseCellAdjustSizeForOverflow`，按「尽量少浪费溢出页空间，同时让本地量落在 minLocal 与 maxLocal 之间」的策略算出 `nLocal`。

`btreeParseCellPtrIndex`（索引 B-tree 节点）结构相同，只是跳过 rowid，`pInfo->nKey = pInfo->nPayload`（索引里键长度就是 payload 长度）。

`minLocal` / `maxLocal` 是打开数据库时算出来的，写在 `BtShared` 上（`btree.c` 的 `sqlite3BtreeOpen` 初始化路径）：

```
pBt->maxLocal = (u16)((pBt->usableSize-12)*64/255 - 23);
pBt->minLocal = (u16)((pBt->usableSize-12)*32/255 - 23);
pBt->maxLeaf  = (u16)(pBt->usableSize - 35);
pBt->minLeaf  = (u16)((pBt->usableSize-12)*32/255 - 23);
```

其中 `usableSize = pageSize - 保留字节数`，且打开时有一条硬校验：`usableSize < 480` 直接判为损坏页。`maxLocal` 还被 `BT_MAX_LOCAL`（定义在 `src/btreeInt.h`，值 65501，即 65536 - 35）约束，解析单元时用 `assert(pPage->maxLocal <= BT_MAX_LOCAL)` 断言；`MX_CELL_SIZE(pBt)` 则是 `pageSize - 8`。

页类型决定用哪套常量：`btreeParseCellPtr` / `btreeParseCellPtrNoPayload` / `btreeParseCellPtrIndex` 三者在 `zeroPage` 时被挂到 `MemPage.xParseCell` 上，叶表页用 `minLeaf`/`maxLeaf`，其余用 `minLocal`/`maxLocal`。

### 2. `sqlite3BtreePayloadSize`：只取长度，不复制

函数体极短：断言游标处于 `CURSOR_VALID`，调 `getCellInfo(pCur)`，返回 `pCur->info.nPayload`。

`getCellInfo` 是一个缓存门闩：`if( pCur->info.nSize==0 )` 才真正调用 `btreeParseCell` 重新解析，否则直接复用。`btreeParseCell` 通过 `pPage->xParseCell(pPage, findCell(pPage, iCell), pInfo)` 定位单元——**单元在页内的位置是从页头的 cell index 数组读出来的 2 字节偏移**（`findCellPastPtr` 宏：`aDataOfst + (maskPage & get2byteAligned(&aCellIdx[2*I]))`），不是顺序扫描。

### 3. `accessPayload`：本地段 + 溢出链

这是整个机制的核心函数，签名 `accessPayload(BtCursor*, u32 offset, u32 amt, unsigned char *pBuf, int eOp)`，`eOp` 为 0 是读、非 0 是写。

执行顺序：

1. `if( pCur->ix >= pPage->nCell ) return SQLITE_CORRUPT_PAGE(pPage);` —— 游标索引越界直接判损坏。
2. `getCellInfo(pCur)` 拿到 `aPayload = pCur->info.pPayload`。
3. 边界检查：`if( (uptr)(aPayload - pPage->aData) > (pBt->usableSize - pCur->info.nLocal) ) return SQLITE_CORRUPT_PAGE(pPage);` —— 这一句等价于「payload 本地段不得越过 `aData[usableSize]`」，源码注释说明改写成这个形式是为了避免整数溢出。
4. **本地段**：`if( offset < pCur->info.nLocal )` 则先从 `&aPayload[offset]` 拷 `min(amt, nLocal-offset)` 字节（走 `copyPayload`），若 `amt` 还有剩余则 `offset -= nLocal` 后进入溢出链。
5. **溢出链**：每张溢出页的有效内容是 `ovflSize = pBt->usableSize - 4` 字节（前 4 字节是下一个溢出页号，由 `get4byte(&aPayload[pCur->info.nLocal])` 取出首个溢出页号）。循环体内：
   - `if( nextPage > pBt->nPage ) return SQLITE_CORRUPT_BKPT;`
   - 若 `offset >= ovflSize`，说明本次只需要页号不需要页内容，优先查 `aOverflow` 缓存，否则调 `getOverflowPage(pBt, nextPage, 0, &nextPage)` 取下一个页号。
   - 否则通过 `sqlite3PagerGet` 取页，拷 `&aPayload[offset+4]` 起的 `a` 字节。
6. 溢出链走完而 `amt` 仍大于 0：`return SQLITE_CORRUPT_PAGE(pPage);`（注释写的是 "Overflow chain ends prematurely"）。

**溢出链缓存**：当 `BTCF_ValidOvfl` 标志未置位时，先算出所需溢出页数 `nOvfl = (nPayload - nLocal + ovflSize - 1) / ovflSize`，然后 `sqlite3Realloc(pCur->aOverflow, nOvfl*2*sizeof(Pgno))` 分配（分配失败返回 `SQLITE_NOMEM_BKPT`；`sqlite3FaultSim(413)` 可模拟失败），并 `memset` 清零。缓存项为 0 表示「尚未知」，惰性填充。若缓存已有效且 `aOverflow[offset/ovflSize]` 非 0，直接跳到该页并把 `offset` 取模。

缓存会失效的场合由源码注释点明：其他游标写了同一张表、游标移到别的行、auto-vacuum 模式下的增量收缩 / `auto_vacuum="full"` 提交、建表。

**auto-vacuum 指针映射捷径**：`getOverflowPage` 在 `pBt->autoVacuum` 为真时，先猜下一个溢出页是 `ovfl+1`（跳过 `PTRMAP_ISPAGE` 的指针映射页和 `PENDING_BYTE_PAGE`），若 `ptrmapGet` 查到类型为 `PTRMAP_OVERFLOW2` 且 `pgno==ovfl` 就认定猜测成立（内部返回 `SQLITE_DONE`，对外折算成 `SQLITE_OK`）；猜不中才回退到读页内容取 `get4byte(pPage->aData)`。

**直接读盘捷径**：`SQLITE_DIRECT_OVERFLOW_READ` 编译开关打开且满足六个条件（读操作、`offset==0`、`sqlite3PagerDirectReadOk` 为真、输出缓冲区前面已有 4 字节可用空间）时，用 `sqlite3OsRead` 直接从数据库文件把数据读进输出缓冲区，绕过页缓存。

### 4. 两个公开入口的差别

`sqlite3BtreePayload` 只做断言（游标必须是 `CURSOR_VALID`），然后 `return accessPayload(pCur, offset, amt, (unsigned char*)pBuf, 0);`。

`sqlite3BtreePayloadChecked` 是增量 blob 的专用变体（源码注释写明 "It is only used by the sqlite3_blob_read() interface"）：

```c
int sqlite3BtreePayloadChecked(BtCursor *pCur, u32 offset, u32 amt, void *pBuf){
  if( pCur->eState==CURSOR_VALID ){
    assert( cursorOwnsBtShared(pCur) );
    return accessPayload(pCur, offset, amt, pBuf, 0);
  }else{
    return accessPayloadChecked(pCur);
  }
}
```

`accessPayloadChecked` 的分支：游标处于 `CURSOR_INVALID` 返回 `SQLITE_ABORT`；否则调 `btreeRestoreCursorPosition(pCur)`，若出错直接返回该错误；恢复后若 `eState != CURSOR_VALID`（行已消失，游标被落到邻行）返回 `SQLITE_ABORT`；否则返回 `SQLITE_MOVED`——**此时一个字节都还没读**。

### 5. 写路径：`sqlite3BtreePutData`

写走的是同一个 `accessPayload`，只是 `eOp=1`。`sqlite3BtreePutData` 在调用前做了一串前置检查：恢复游标位置（`restoreCursorPosition`，失败即返回错误码）、`eState != CURSOR_VALID` 返回 `SQLITE_ABORT`、`saveAllCursors` 保存同表其他游标位置（注释解释这是防止其他游标持有被 `accessPayload` 修改的页的 xFetch 版本）、检查 `BTCF_WriteFlag` 未置位则返回 `SQLITE_READONLY`，最后 `return accessPayload(pCsr, offset, amt, (unsigned char *)z, 1);`。

`eOp=1` 时 `copyPayload` 会先调 `sqlite3PagerWrite(pDbPage)` 再 memcpy；读溢出页时若 `eOp!=0` 且该页引用计数不为 1 或已初始化，会 `return SQLITE_CORRUPT_PAGE(pPage)`。

### 6. 上层怎么用

- **`sqlite3BtreePayloadFetch` → `fetchPayload`**：零拷贝路径，直接返回 `pCur->info.pPayload` 指针与本地可用字节数 `nLocal`（若页内剩余空间不足则夹到 `MAX(0, aDataEnd - pPayload)`）。注释明确说这个指针是 ephemeral 的：「The key/data may move or be destroyed on the next call to any Btree routine」。`OP_Column` 用它拿 `pC->aRow`。
- **`OP_Column`（`src/vdbe.c`）**：先 `pC->payloadSize = sqlite3BtreePayloadSize(pCrsr)`，再 `pC->aRow = sqlite3BtreePayloadFetch(pCrsr, &pC->szRow)`。然后解析记录头：首字节 `< 0x80` 时头长就是 1 字节，否则 `sqlite3GetVarint32`。头长超过 98307 判损坏——这个上限来自源码注释的推导：每列类型串 1~5 字节，4 字节与 5 字节类型分别最多 4096 个与 32 个，故最长头由 32768 列 × 3 字节 + 3 字节头长 = 98307 决定。字段类型串逐个用 `sqlite3GetVarint32` 读出 serial type，长度由 `sqlite3VdbeSerialTypeLen` 给出（`serial_type>=128` 时为 `(serial_type-12)/2`，否则查 `sqlite3SmallTypeSizes` 表），类型值本体由 `sqlite3VdbeSerialGet` 解出。
- **`sqlite3VdbeRecordCompare` 路径（`src/vdbeaux.c`）**：先用 `sqlite3BtreePayloadSize(pCur)` 取长度，若超过 `0x7fffffff` 直接 `return SQLITE_CORRUPT_BKPT`；分配 `nRec+5` 字节（多分配的 5 字节是给 `getVarint32` 越界读留的余量），再用 `sqlite3BtreePayload(pCur, 0, nRec, aRec)` 把整条记录读进来，之后按头长、serial type 逐字段 `sqlite3VdbeSerialGet`。
- **索引 seek（`btree.c` 的 `movetoIndex`）**：索引单元若能确定落在本地页，直接对页内字节做记录比较；只有「记录跨溢出页」时才走 `pPage->xParseCell` 解析、`sqlite3Malloc` 出 `nCell + 18` 字节的缓冲区（18 字节是注释里说的 overrun padding，因为比较例程可能多读两个 varint）、再 `accessPayload(pCur, 0, nCell, (unsigned char*)pCellKey, 0)` 读出整条记录。读完后清 `BTCF_ValidOvfl` 标志。
- **游标位置保存（`saveCursorKey`）**：索引 B-tree 保存游标位置时用 `sqlite3BtreePayloadSize` 取长度，再 `sqlite3BtreePayload` 把键内容整条读出；分配时额外加 `9 + 8` 字节，注释说明这是因为 `sqlite3VdbeRecordUnpack` 最多可能多读 1 个 varint 加 1 个 8 字节值。
- **增量 blob（`src/vdbeblob.c`）**：`sqlite3_blob_read` 把 `sqlite3BtreePayloadChecked` 作为函数指针传给 `blobReadWrite`。`blobReadWrite` 收到 `SQLITE_MOVED` 时**回退到 `blobReadWriteRetry`**——该函数用 `sqlite3BtreeIntegerKey` 取出 rowid，再调 `blobSeekToRow` 重新定位，然后重试 `blobReadWrite`；注释解释必须重算偏移，因为 ROLLBACK 可能改变前面列的长度从而移动 blob 在行内的位置。收到 `SQLITE_ABORT` 时调 `sqlite3VdbeFinalize` 释放语句。

## 关键取舍

**取舍一：本地 / 溢出的分界点必须写进文件格式，不能随意改。** `btreeParseCellAdjustSizeForOverflow` 的注释直接写明 "Warning: changing the way overflow payload is distributed in any way will result in an incompatible file format." 所以分界点由 `minLocal`/`maxLocal` 和 `usableSize-4` 这套公式一次性定死，而不是每次写入时动态择优。

**取舍二：溢出链页号缓存换来随机读性能，代价是缓存一致性由调用方兜。** `aOverflow` 分配了 `nOvfl*2` 个 `Pgno`（两倍冗余），换取后续按 `offset/ovflSize` 直接命中；但缓存的有效性完全依赖「其他游标写同表 / 游标换行 / auto-vacuum 收缩」时及时清 `BTCF_ValidOvfl` 标志。

**取舍三：宁可判损坏，也不做越界读。** 本地段越界检查、`nextPage > pBt->nPage`、溢出链提前结束、`nPayload > 0x7fffffff`、记录头长 > 98307，全部返回 `SQLITE_CORRUPT_*` 而不是尽力读取。

**取舍四：零拷贝与拷贝两条路并存。** `sqlite3BtreePayloadFetch` 只在数据确定落在本地页时返回页内指针；一旦溢出，就必须用 `accessPayload` 重新拼装。`OP_Column` 对此做了额外判断：若 `pC->szRow < aOffset[0]`（本地部分不足以覆盖记录头），就丢弃页内指针，改调 `sqlite3VdbeMemFromBtreeZeroOffset` 把头部读进独立内存。

**取舍五：`sqlite3BtreePayload` 与 `sqlite3BtreePayloadChecked` 不合并。** 前者用断言承担「调用方保证游标有效」的契约（断言在非 `SQLITE_DEBUG` 编译下是空操作），后者自己处理游标失效、恢复位置、以及恢复后行已消失三种情况。

**架构推论：** 记录格式的解析（serial type 串、字段值解码）不在 `btree.c` 而在 `vdbeaux.c` / `vdbe.c`，说明 SQLite 把「B-tree 单元」与「SQL 记录」当成两层格式：B-tree 层只负责字节的定位与搬运，字段语义由 VDBE 层承担。这与本条目 boundary 里「不负责索引键的比较语义」是一致的——记录比较在 `src/vdbeaux.c` 的 `sqlite3VdbeRecordCompare` 一侧。

**架构推论：** `accessPayload` 的 `eOp` 参数把读与写合并进同一个函数体，意味着「本地段 + 溢出链」的布局逻辑只需维护一份；写路径复用它意味着写溢出页的页号推进逻辑与读路径天然一致。

## 源码落点

| 路径 | 符号 | 作用 |
| --- | --- | --- |
| src/btree.c | `sqlite3BtreePayloadSize` | 返回当前单元的 payload 总长度；内部先走 `getCellInfo` 保证 `CellInfo` 已解析 |
| src/btree.c | `sqlite3BtreePayload` | 读接口，要求游标处于 `CURSOR_VALID`，转发给 `accessPayload` |
| src/btree.c | `sqlite3BtreePayloadChecked` | 增量 blob 用的读接口；游标失效时经 `accessPayloadChecked` 返回 `SQLITE_MOVED` 或 `SQLITE_ABORT` |
| src/btree.c | `accessPayload` | 核心搬运函数：本地段拷贝 + 溢出链遍历 + `aOverflow` 页号缓存 + 损坏检测 |
| src/btree.c | `accessPayloadChecked` | 游标状态非 `CURSOR_VALID` 时的分支：恢复位置并区分「行已消失」与「位置已变」 |
| src/btree.c | `btreeParseCellPtr` | 表 B-tree 叶节点单元头解析：payload 长度 varint + rowid varint，产出 `nPayload`/`nLocal`/`pPayload` |
| src/btree.c | `btreeParseCellPtrIndex` | 索引 B-tree 单元头解析：payload 长度 varint，`nKey` 等于 `nPayload` |
| src/btree.c | `btreeParseCellAdjustSizeForOverflow` | 决定本地留存字节数 `nLocal`，并给出单元总长 `nSize` |
| src/btree.c | `btreePayloadToLocal` | 由 payload 总长反算本地留存长度，与 `btreeParseCellAdjustSizeForOverflow` 用同一套公式 |
| src/btree.c | `btreeParseCell` | 按单元下标解析单元：定位靠 `findCell` + `MemPage.xParseCell` |
| src/btree.c | `getCellInfo` | `CellInfo` 缓存门闩：`nSize==0` 时才重新解析 |
| src/btree.c | `getOverflowPage` | 取溢出链的下一个页号；auto-vacuum 下先查指针映射，失败回退到读页内容 |
| src/btree.c | `copyPayload` | 真正的字节拷贝；`eOp` 非 0 时先 `sqlite3PagerWrite` 再写 |
| src/btree.c | `fetchPayload` | 零拷贝取本地 payload 指针与可用字节数 |
| src/btree.c | `sqlite3BtreePayloadFetch` | `fetchPayload` 的公开包装 |
| src/btree.c | `sqlite3BtreePutData` | 增量 blob 写接口；做写标志、游标恢复、`saveAllCursors` 等前置检查后调用 `accessPayload` |
| src/btree.c | `saveCursorKey` | 保存游标位置时把索引键整条读出，额外留 `9+8` 字节 padding |
| src/btree.c | `sqlite3BtreeOpen`（页头初始化段） | 计算 `maxLocal`/`minLocal`/`maxLeaf`/`minLeaf` 与 `usableSize`，并校验 `usableSize >= 480` |
| src/btree.c | `movetoIndex`（索引比较段） | 索引单元跨溢出页时分配 `nCell+18` 缓冲区并调用 `accessPayload` 读整条记录 |
| src/btreeInt.h | `BT_MAX_LOCAL`、`MX_CELL_SIZE` | 单元本地量的上限常量（65501）与单元最大尺寸公式（`pageSize-8`） |
| src/btreeInt.h | `BTCF_ValidOvfl` | 标记 `BtCursor.aOverflow` 溢出页号缓存是否有效 |
| src/vdbe.c | `OP_Column` | 记录头与字段类型串的解析点；用 `sqlite3BtreePayloadSize` + `sqlite3BtreePayloadFetch` 取数据，98307 头长上限在此判定 |
| src/vdbeaux.c | `sqlite3VdbeSerialTypeLen` | 由 serial type 求字段值长度 |
| src/vdbeaux.c | `sqlite3VdbeSerialGet` | 由 serial type 解出字段值到 `Mem`；serial type 7 为 IEEE 浮点，8/9 为常量 0/1 |
| src/vdbeaux.c | `sqlite3VdbeRecordCompare` 一侧 | 整条记录读入缓冲区后逐字段比较；读入前用 `sqlite3BtreePayloadSize` 取长度并做 `0x7fffffff` 上限检查 |
| src/vdbeblob.c | `blobReadWrite` | 把 `sqlite3BtreePayloadChecked` 当回调用；收到 `SQLITE_MOVED` 回退到 `blobReadWriteRetry` |
| src/vdbeblob.c | `blobReadWriteRetry` | 收到 `SQLITE_MOVED` 后的重定位与重试路径 |

## 与主核 / AI 的关系

**架构推论：** 本条目描述的机制在本仓 `engine/db/` 主核中没有直接对应物。主核的 `L1 storage/` 层自带一套页与记录管理（`storage/`、`wal/` 只依赖 `common/`），与 SQLite 的 B-tree 无共享代码。可迁移的是**机制层面的三条经验**：

1. **「零拷贝优先 + 溢出回退」的双路结构**：主核的向量化列存若遇到大字段（多模态 blob），同样面临「头部元数据在页内、大体在别处」的问题，页内直取与整段读回的两条路值得同时保留。
2. **解析与搬运分层**：把「字节怎么取到」与「字节是什么语义」分开，是本条目能独立成一页的前提；主核若把变长编码解码与页访问混在一个函数里，将难以单独测试损坏输入。
3. **损坏检测优先于尽力读取**：本条目里所有越界情形一律返回 `SQLITE_CORRUPT_*`。主核处理不可信输入文件时，同类的「宁可报错也不越界」策略应保持一致。

至于 AI 相关：`sqlite3BtreePayloadChecked` 返回 `SQLITE_MOVED` 后由 `blobReadWriteRetry` 重定位重试的这条路径，说明**当行内容可能因事务回滚而位移时，调用方必须重算偏移而不能复用旧偏移**。若把 SQLite 的 blob 增量读语义搬进 AI 检索链路（例如从库里分块拉取长文档片段喂给模型），同样的陷阱存在：拉取前缓存的片段偏移在并发写入后不再可靠，需要按行 ID 重新定位并重算偏移。

## 参考

- [SQLite 文件格式：B-tree 页与单元布局](https://sqlite.org/fileformat2.html) —— 官方文档，单元头、payload 长度 varint、溢出页链的格式定义。
- [SQLite 的 B-tree 实现（sqlite.org/src，src/btree.c）](https://sqlite.org/src/file?name=src/btree.c) —— 本页源码对应的官方 Fossil 源文件入口。
- [sqlite/sqlite 镜像仓库的 src/btree.c](https://github.com/sqlite/sqlite/blob/master/src/btree.c) —— GitHub 官方镜像上的同一文件。
- [SQLite 架构总览](https://sqlite.org/arch.html) —— B-tree 在整体分层（VDBE → B-Tree → Pager → WAL → VFS）中的位置。
