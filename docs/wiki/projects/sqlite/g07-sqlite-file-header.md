---
project: sqlite
feature: sqlite-file-header
page_type: feature
schema_version: 1
refs:
  - path: src/pager.c
    symbols: [sqlite3PagerReadFileheader, sqlite3PagerSetPagesize]
---

# 数据库文件头

## 一句话职责

`sqlite3PagerReadFileheader` 负责在分页器创建后、尚未进入 WAL 模式之前，把数据库文件开头的 100 个字节一次性读进调用方的缓冲区（读不满时补零），为上层解析头字段提供原始字节；`sqlite3PagerSetPagesize` 负责校验并落实页大小与每页预留字节数这两个头字段 —— 在满足「无内存库或内存库为空页」「页缓存无引用」「新页大小与当前不同」三个前提时真正切换 `Pager.pageSize` 并重新分配临时页缓冲，否则原样保留旧页大小并把旧值回写给调用方。

## 机制是什么

SQLite 数据库文件的前 100 个字节是**数据库文件头**（database file header），它是整个文件的自描述入口：文件头里记录了页大小、文件格式读写版本号、每页预留字节数、载荷分数、文件变更计数器、库内页数、空闲链表头、schema cookie、schema 格式号、文本编码、autovacuum 指针映射根页号，以及写入库的版本号。

文件头的位置是「物理上就是第 1 页的前 100 字节」，因此读文件头与读第 1 页是两件事：`sqlite3PagerReadFileheader` 走的是一次独立的 `sqlite3OsRead(fd, pDest, N, 0)`（偏移 0），而第 1 页本身还要作为 B-tree 页参与缓存与事务管理。

**本机制的边界**（与页面清单一致）：负责数据库文件头的字段含义、版本号与页大小的解析校验，以及写回时的头信息维护；不负责页的读写（G02/G04）、不负责 B-tree 的结构（F 组）。

需要区分两层职责：

| 层 | 符号 | 负责什么 |
|---|---|---|
| 分页器层（本文主体） | `src/pager.c` 的 `sqlite3PagerReadFileheader` / `sqlite3PagerSetPagesize` | 提供原始字节读取通道；校验并落实页大小 / 预留字节数这两个头字段 |
| B-tree 层 | `src/btree.c` 的 `sqlite3BtreeOpen` / `lockBtree` / `newDatabase` | 解释头字段的语义（魔数、版本号、载荷分数），并在创建新库时把头字段写进第 1 页 |

分页器自己并不解释「魔数字符串」「读/写版本号」「载荷分数」这些字段——它只搬运字节并落实页大小。其余字段的语义校验发生在 B-tree 层。

## 怎么实现的

### 一、读取：`sqlite3PagerReadFileheader`

`src/pager.c` 中 `sqlite3PagerReadFileheader(Pager *pPager, int N, unsigned char *pDest)` 的函数体只有四段有效逻辑：

```c
int sqlite3PagerReadFileheader(Pager *pPager, int N, unsigned char *pDest){
  int rc = SQLITE_OK;
  memset(pDest, 0, N);
  assert( isOpen(pPager->fd) || pPager->tempFile );
  assert( !pagerUseWal(pPager) );

  if( isOpen(pPager->fd) ){
    IOTRACE(("DBHDR %p 0 %d\n", pPager, N))
    rc = sqlite3OsRead(pPager->fd, pDest, N, 0);
    if( rc==SQLITE_IOERR_SHORT_READ ){
      rc = SQLITE_OK;
    }
  }
  return rc;
}
```

四个要点，全部可以从函数体直接读出：

1. **先清零**：`memset(pDest, 0, N)` 在读之前执行。这决定了「读不满」时的语义 —— 短读留下的尾部是 0，而不是脏数据。函数上方的注释块给出了理由：「a new transient or zero sized database has a header than consists entirely of zeroes」（新建的临时库或零长度库，其文件头全部由零构成）。
2. **只读偏移 0 的 N 字节**：`sqlite3OsRead(pPager->fd, pDest, N, 0)`，偏移量硬编码为 `0`。`N` 由调用方传入，B-tree 层传的是 `sizeof(zDbHeader)`。
3. **短读不算错误**：`SQLITE_IOERR_SHORT_READ` 被就地改写成 `SQLITE_OK`。函数注释明确说明：除短读以外的任何 IO 错误都会原样返回给调用方，此时输出缓冲区内容未定义。
4. **两条断言划定了调用时机**：`assert( isOpen(pPager->fd) || pPager->tempFile )` 要求文件已打开或是临时文件；`assert( !pagerUseWal(pPager) )` 要求此刻还没进入 WAL 模式。函数注释进一步说明原因：「This routine is only called by btree immediately after creating the Pager object. There has not been an opportunity to transition to WAL mode yet.」——**这个函数只能在 Pager 对象刚创建、尚未有任何 WAL 转换机会时被调用**，因此不能用它去读 WAL 模式下已更新的文件头。

唯一的调用方是 `src/btree.c` 的 `sqlite3BtreeOpen`：在 `sqlite3PagerOpen` 成功之后立刻调用 `rc = sqlite3PagerReadFileheader(pBt->pPager,sizeof(zDbHeader),zDbHeader);`，其中 `zDbHeader` 是 `unsigned char zDbHeader[100]`。读到的字节随后由 B-tree 层解释：

- `pBt->pageSize = (zDbHeader[16]<<8) | (zDbHeader[17]<<16);` —— 偏移 16、17 两字节给出页大小。
- 页大小若不合法（`< 512`、`> SQLITE_MAX_PAGE_SIZE`、或不是 2 的幂），`pBt->pageSize` 被置为 0，且 `nReserve` 置 0，走「按默认值建立新库」的路径。
- 页大小合法时，`nReserve = zDbHeader[20];`（偏移 20 的一字节预留空间），并置位 `BTS_PAGESIZE_FIXED`。
- `pBt->autoVacuum = (get4byte(&zDbHeader[36 + 4*4])?1:0);` 与 `pBt->incrVacuum = (get4byte(&zDbHeader[36 + 7*4])?1:0);` 读取偏移 52 与 64 的 autovacuum / 增量 vacuum 标志。

注意 `sqlite3BtreeOpen` 在读头之前先做了 `memset(&zDbHeader[16], 0, 8);`（抑制 PVS-Studio 的误报），这意味着偏移 16 到 23 这 8 个字节即使短读也是 0。

### 二、落实：`sqlite3PagerSetPagesize`

`src/pager.c` 中 `sqlite3PagerSetPagesize(Pager *pPager, u32 *pPageSize, int nReserve)` 是分页器侧的「页大小裁决者」。它的函数体注释把契约写得很清楚：

- 新的页大小必须是 **512 到 `SQLITE_MAX_PAGE_SIZE` 之间的 2 的幂**（含端点）；
- 必须**没有未决的页引用**；
- 数据库必须**不是内存库**，或者是**当前为零页的内存库**。

函数体的判定条件逐条对应这三项：

```c
u32 pageSize = *pPageSize;
assert( pageSize==0 || (pageSize>=512 && pageSize<=SQLITE_MAX_PAGE_SIZE) );
if( (pPager->memDb==0 || pPager->dbSize==0)
 && sqlite3PcacheRefCount(pPager->pPCache)==0
 && pageSize && pageSize!=(u32)pPager->pageSize
){
```

- `pPager->memDb==0 || pPager->dbSize==0`：非内存库，或内存库但零页。
- `sqlite3PcacheRefCount(pPager->pPCache)==0`：页缓存中没有未决引用。
- `pageSize` 非 0 且与 `pPager->pageSize` 不同：确实要改。

不满足时，函数**什么都不做**，但仍执行末尾的统一处理：

```c
  *pPageSize = pPager->pageSize;
  if( rc==SQLITE_OK ){
    if( nReserve<0 ) nReserve = pPager->nReserve;
    assert( nReserve>=0 && nReserve<1000 );
    pPager->nReserve = (i16)nReserve;
    pagerFixMaplimit(pPager);
  }
  return rc;
```

关键点：`*pPageSize = pPager->pageSize;` —— **无论是否真的切换，输出参数总是被回写成「最终生效的页大小」**。调用方 `sqlite3BtreeOpen` 依赖这一点来拿到权威值：`rc = sqlite3PagerSetPagesize(pBt->pPager, &pBt->pageSize, nReserve);` 之后立刻 `pBt->usableSize = pBt->pageSize - nReserve;`。

`nReserve` 传 `-1` 表示「沿用 `pPager->nReserve` 当前值」；函数体注释与 `if( nReserve<0 ) nReserve = pPager->nReserve;` 一致。断言 `assert( nReserve>=0 && nReserve<1000 );` 界定了预留字节数的上界。

### 三、切换路径的四个动作

条件满足时，函数体进入真正的切换，分四步：

1. **取文件大小**：`if( pPager->eState>PAGER_OPEN && isOpen(pPager->fd) ){ rc = sqlite3OsFileSize(pPager->fd, &nByte); }` —— 只有分页器已开锁且文件已打开时才取。
2. **分配新的临时页缓冲**：`pNew = (char *)sqlite3PageMalloc(pageSize+8);`。多分配的 8 字节有明确用途，注释写明：「8 bytes of zeroed overrun space is sufficient so that the b-tree cell header parser will never run off the end of the allocation」。分配失败则 `rc = SQLITE_NOMEM_BKPT;`，页大小保持不变。
3. **重置页缓存**：`pager_reset(pPager); rc = sqlite3PcacheSetPageSize(pPager->pPCache, pageSize);`。`pager_reset` 的函数体是 `pPager->iDataVersion++; sqlite3BackupRestart(pPager->pBackup); sqlite3PcacheClear(pPager->pPCache);`。
4. **提交新状态**：

```c
      sqlite3PageFree(pPager->pTmpSpace);
      pPager->pTmpSpace = pNew;
      pPager->dbSize = (Pgno)((nByte+pageSize-1)/pageSize);
      pPager->pageSize = pageSize;
      pPager->lckPgno = (Pgno)(PENDING_BYTE/pageSize) + 1;
```

- `dbSize` 由文件字节数向上取整成页数（`(nByte+pageSize-1)/pageSize`）。
- `lckPgno` 由 `PENDING_BYTE` 常量除以新页Size得到 —— 锁字节所在页号随页大小变化，这正是 `PENDING_BYTE` 必须参与换算的原因。

任一步失败走 `else{ sqlite3PageFree(pNew); }`，页大小不变。

### 四、头字段的写回

页大小之外的头字段由各自的调用方在写第 1 页时更新：

- **变更计数器（偏移 24 / 92）与写入库版本号（偏移 96）**：`src/pager.c` 的 `pager_write_changecounter(PgHdr *pPg)` 函数体是：

```c
  change_counter = sqlite3Get4byte((u8*)pPg->pPager->dbFileVers)+1;
  put32bits(((char*)pPg->pData)+24, change_counter);
  put32bits(((char*)pPg->pData)+92, change_counter);
  put32bits(((char*)pPg->pData)+96, SQLITE_VERSION_NUMBER);
```

即：把 `dbFileVers` 加一，同时写入偏移 24（文件变更计数器）与偏移 92（version-valid-for 号），并在偏移 96 写入 `SQLITE_VERSION_NUMBER`。其上的注释写明这是「无条件更新」，与 `pager_incr_changecounter` 的「按需更新」相对：后者用 `pPager->changeCountDone` 状态位决定是否真的要改。

`pager_incr_changecounter(Pager *pPager, int isDirectMode)` 在 `!pPager->changeCountDone && pPager->dbSize>0` 时取第 1 页（`sqlite3PagerGet(pPager, 1, &pPgHdr, 0)`），非 direct 模式下先 `sqlite3PagerWrite(pPgHdr)` 把它变成可写页，再调 `pager_write_changecounter(pPgHdr)`。direct 模式（仅在定义 `SQLITE_ENABLE_ATOMIC_WRITE` 时可用）直接 `sqlite3OsWrite(pPager->fd, zBuf, pPager->pageSize, 0)` 把整页写回偏移 0。

- **库内页数（偏移 28）**：`src/btree.c` 的 `btreeBeginTrans` 在开写事务时校正它：

```c
      if( pBt->nPage!=get4byte(&pPage1->aData[28]) ){
        rc = sqlite3PagerWrite(pPage1->pDbPage);
        if( rc==SQLITE_OK ){
          put4byte(&pPage1->aData[28], pBt->nPage);
        }
      }
```

注释解释了为什么这么早写：「Doing this sooner rather than later means the database size can safely re-read the database size from page 1 if a savepoint or transaction rollback occurs within the transaction.」

- **新建库时的整头初始化**：`src/btree.c` 的 `newDatabase(BtShared *pBt)` 一次性写入全部固定字段：

```c
  memcpy(data, zMagicHeader, sizeof(zMagicHeader));
  assert( sizeof(zMagicHeader)==16 );
  data[16] = (u8)((pBt->pageSize>>8)&0xff);
  data[17] = (u8)((pBt->pageSize>>16)&0xff);
  data[18] = 1;
  data[19] = 1;
  data[20] = (u8)(pBt->pageSize - pBt->usableSize);
  data[21] = 64;
  data[22] = 32;
  data[23] = 32;
  memset(&data[24], 0, 100-24);
  ...
  put4byte(&data[36 + 4*4], pBt->autoVacuum);
  put4byte(&data[36 + 7*4], pBt->incrVacuum);
  ...
  data[31] = 1;
```

- 魔数来自 `zMagicHeader`，其定义是 `static const char zMagicHeader[] = SQLITE_FILE_HEADER;`，而 `SQLITE_FILE_HEADER` 在 `src/btreeInt.h` 中定义为 `"SQLite format 3"`。该头文件的注释说明它可以在编译期用 `-DSQLITE_FILE_HEADER="..."` 覆盖，但「The header must be exactly 16 bytes including the zero-terminator so the string itself should be 15 characters long」。
- 偏移 18、19（写/读格式版本号）在新建时写 `1`，对应回滚日志模式；切到 WAL 模式时写 `2`。
- 偏移 21、22、23 固定写 `64`、`32`、`32`——最大/最小载荷分数与叶载荷分数。`lockBtree` 中有对应的校验：`if( memcmp(&page1[21], "\100\040\040",3)!=0 ){ goto page1_init_failed; }`，注释写明这三值自 3.6.0 起固定为 64、32、32。
- 偏移 24 到 99 全部清零（`memset(&data[24], 0, 100-24)`），随后再逐字段填入。
- 偏移 31 写 `1`（文件格式读写版本号为 1 的标记位）。

### 五、B-tree 层的二次校验：`lockBtree`

`src/btree.c` 的 `lockBtree(BtShared *pBt)` 是文件头语义校验的真正执行者。它先取共享锁、取第 1 页，然后逐项检查：

| 检查项 | 判据（函数体原文） | 失败处理 |
|---|---|---|
| 魔数 | `if( memcmp(page1, zMagicHeader, 16)!=0 ) goto page1_init_failed;` | `SQLITE_NOTADB` |
| 写版本号（偏移 18） | `if( page1[18]>2 ){ pBt->btsFlags |= BTS_READ_ONLY; }`（未定义 `SQLITE_OMIT_WAL` 时） | 置只读 |
| 读版本号（偏移 19） | `if( page1[19]>2 ) goto page1_init_failed;` | `SQLITE_NOTADB` |
| WAL 模式识别 | `if( page1[19]==2 && (pBt->btsFlags & BTS_NO_WAL)==0 ){ ... sqlite3PagerOpenWal(pBt->pPager, &isOpen); ... }` | 打开 WAL 后 `releasePageOne(pPage1); return SQLITE_OK;` 让调用方重入 |
| 载荷分数（偏移 21-23） | `if( memcmp(&page1[21], "\100\040\040",3)!=0 ) goto page1_init_failed;` | `SQLITE_NOTADB` |
| 页大小（偏移 16、17） | `pageSize = (page1[16]<<8) | (page1[17]<<16);` 后判 `((pageSize-1)&pageSize)!=0 || pageSize>SQLITE_MAX_PAGE_SIZE || pageSize<=256` | `SQLITE_NOTADB` |
| 预留空间（偏移 20） | `usableSize = pageSize - page1[20];` 后判 `if( usableSize<480 ) goto page1_init_failed;` | `SQLITE_NOTADB` |

三处值得注意：

1. **页大小与预读值不一致时的处理**：`if( (u32)pageSize!=pBt->pageSize )` 分支里，函数先 `releasePageOne(pPage1);`、设置 `pBt->usableSize = usableSize;`、`pBt->pageSize = pageSize;`、置位 `BTS_PAGESIZE_FIXED`、`freeTempSpace(pBt);`，然后调 `sqlite3PagerSetPagesize(pBt->pPager, &pBt->pageSize, pageSize-usableSize);` 并**直接返回**。注释说明这是为了让调用方用正确的页大小再来一次：`The caller will call this function again with the correct page-size.` —— 这是 `sqlite3PagerSetPagesize` 除 `sqlite3BtreeOpen` 之外的另一个调用方。

2. **页大小下限在两层不一致**：`lockBtree` 判 `pageSize<=256` 即失败（实际生效下限是 512），而 `sqlite3BtreeOpen` 判 `pBt->pageSize<512`。两处都依赖 `SQLITE_MAX_PAGE_SIZE` 这个上限。

3. **头内页数的可信度判断**：

```c
  nPage = get4byte(28+(u8*)pPage1->aData);
  sqlite3PagerPagecount(pBt->pPager, (int*)&nPageFile);
  if( nPage==0 || memcmp(24+(u8*)pPage1->aData, 92+(u8*)pPage1->aData,4)!=0 ){
    nPage = nPageFile;
  }
```

偏移 28（库内页数）只有在**非零**、且**偏移 24 的变更计数器与偏移 92 的 version-valid-for 号逐字节相同**时才被采信，否则回退到按文件实际大小算出的 `nPageFile`。这正是 `pager_write_changecounter` 同时写 24 与 92 两个位置的原因：两处相等是「头内页数可信」的凭据。若 `nPage>nPageFile` 且库不可写（`sqlite3WritableSchema(pBt->db)==0`），则 `rc = SQLITE_CORRUPT_BKPT;`。

### 六、相关常量与配置

| 常量 | 值 | 出处 | 作用 |
|---|---|---|---|
| `SQLITE_MAX_PAGE_SIZE` | `65536` | `src/sqliteLimit.h` 的 `#define SQLITE_MAX_PAGE_SIZE 65536` | 页大小上限；该头文件注释说明它已不可在编译期覆盖（`#ifdef SQLITE_MAX_PAGE_SIZE / #undef`），理由是「creates a library that is technically incompatible with an SQLite library compiled with a different limit」，会导致崩溃后无法回滚 |
| `SQLITE_DEFAULT_PAGE_SIZE` | `4096` | `src/sqliteLimit.h` 的 `#ifndef SQLITE_DEFAULT_PAGE_SIZE / # define SQLITE_DEFAULT_PAGE_SIZE 4096` | 新建库的默认页大小；若大于 `SQLITE_MAX_PAGE_SIZE` 会被压回该上限 |
| `SQLITE_MAX_DEFAULT_PAGE_SIZE` | `8192` | `src/sqliteLimit.h` | SQLite 自行选择页大小时允许取到的最大值 |
| `SQLITE_FILE_HEADER` | `"SQLite format 3"` | `src/btreeInt.h` | 文件头魔数，必须恰好 16 字节（含结尾的零字节） |
| `PENDING_BYTE` | — | `src/os_unix.c` 等 | 锁字节位置常量；`sqlite3PagerSetPagesize` 用 `PENDING_BYTE/pageSize + 1` 换算出锁字节所在页号 |
| `SQLITE_VERSION_NUMBER` | — | `src/sqlite.h.in` | `pager_write_changecounter` 写入偏移 96 的写入库版本号 |

`src/pager.c` 的 `sqlite3PagerOpen` 用 `szPageDflt` 起步，其初值是 `u32 szPageDflt = SQLITE_DEFAULT_PAGE_SIZE;`；随后在非只读分支里按扇区大小与原子写能力上调：

```c
        if( szPageDflt<pPager->sectorSize ){
          if( pPager->sectorSize>SQLITE_MAX_DEFAULT_PAGE_SIZE ){
            szPageDflt = SQLITE_MAX_DEFAULT_PAGE_SIZE;
          }else{
            szPageDflt = (u32)pPager->sectorSize;
          }
        }
```

启用 `SQLITE_ENABLE_ATOMIC_WRITE` 时，还会从 `szPageDflt` 起按 2 倍递增到 `SQLITE_MAX_DEFAULT_PAGE_SIZE`，取设备特性 `iDc` 支持的最大原子写页大小（判据 `iDc&(SQLITE_IOCAP_ATOMIC|(ii>>8))`）。最终 `rc = sqlite3PagerSetPagesize(pPager, &szPageDflt, -1);` 把这个默认页大小落进分页器，并据此初始化页缓存（`sqlite3PcacheOpen(szPageDflt, nExtra, ...)`）。

## 关键取舍

### 取舍一：分页器只搬运字节，语义校验上移一层

`sqlite3PagerReadFileheader` 不做任何字段解释，只保证「前 N 字节被正确读出，读不满处为 0」；魔数、版本号、载荷分数的判定全在 `src/btree.c` 的 `lockBtree` 里。**代价**是分页器无法自己判断「这是个合法库」；**收益**是分页器不需要知道文件格式版本，WAL 模式的分页器（`src/wal.c`）完全不必维护一份文件头语义。

### 取舍二：读不满当成功处理

`SQLITE_IOERR_SHORT_READ` 被改写成 `SQLITE_OK`。这是有意为之：注释把理由写死为「a new transient or zero sized database has a header than consists entirely of zeroes」。**代价**是「文件被截断到只剩几十字节」与「文件是全新的」在分页器层无法区分 —— 区分被推给了上层（`lockBtree` 的魔数校验会把截断文件判为 `SQLITE_NOTADB`）。**收益**是新建空库与读半截文件可以走同一条代码路径，不需要单独的「首次创建」分支。

### 取舍三：页大小切换是一次性、不可逆的

`sqlite3PagerSetPagesize` 的三个前提里，「页缓存无未决引用」这一条决定了：页大小**只能在没有任何人持有页的时候改**。一旦有游标持有页引用，函数直接不作为，调用方拿回旧页大小。**代价**是「PRAGMA page_size」在有活跃读事务时改不动；**收益**是页大小变更不需要做页内容的重排 —— 因为数据库文件在页大小变更后必然是全新重建（`newDatabase` 路径），旧文件不会被就地转换。

### 取舍四：变更计数器写两遍，且两遍相等才可信

`pager_write_changecounter` 把同一个值同时写到偏移 24 与偏移 92。看似冗余，实则是**版本兼容的判据**：`lockBtree` 用 `memcmp(24+..., 92+..., 4)!=0` 判断「头内页数（偏移 28）是否可信」。老版本 SQLite 不会更新偏移 28 的头内页数，也不会动偏移 92，于是两处不相等，新版就能识别出「头内页数是陈旧的」并回退到按文件实际大小计算。**代价**是每次写第 1 页多写 4 字节；**收益**是不同版本 SQLite 混用同一文件时不会因陈旧的头内页数而误判。

### 取舍五：文件头偏移 16-23 在读之前被清零

`sqlite3BtreeOpen` 在调用 `sqlite3PagerReadFileheader` 之前先做 `memset(&zDbHeader[16], 0, 8);`（该处注释写明是「Suppress false-positive compiler warning from PVS-Studio」）。这使页大小、版本号、预留字节这三个最关键的字段在短读场景下必然读到 0，从而落入「页大小不合法 → 按默认值建立新库」的分支，而不是读到随机值。

### 取舍六：页大小上限不可在编译期调整

`src/sqliteLimit.h` 明确 `#undef SQLITE_MAX_PAGE_SIZE` 后写死 `65536`，并给出理由：若某个进程用 65536 字节页的库崩溃，默认上限的 SQLite 库将无法回滚该未完成事务，从而导致数据库损坏（注释原文：「This could lead to database corruption.」）。这是**用编译期灵活性换跨版本可恢复性**的取舍。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/pager.c | `sqlite3PagerReadFileheader` | 读文件头原始字节：`memset` 清零后 `sqlite3OsRead(fd, pDest, N, 0)`；`SQLITE_IOERR_SHORT_READ` 改写为 `SQLITE_OK`；断言要求未进 WAL 模式 |
| src/pager.c | `sqlite3PagerSetPagesize` | 校验并落实页大小与预留字节数：三个前提（无引用 / 非内存库或零页 / 页大小确实变化）全满足才切换；否则原样保留并回写旧值 |
| src/pager.c | `pager_reset` | 切换页大小时清空页缓存：`iDataVersion++`、`sqlite3BackupRestart`、`sqlite3PcacheClear` |
| src/pager.c | `pager_write_changecounter` | 写头字段：偏移 24 与 92 同写 `dbFileVers+1`，偏移 96 写 `SQLITE_VERSION_NUMBER` |
| src/pager.c | `pager_incr_changecounter` | 按 `changeCountDone` 状态位决定是否真的更新变更计数器；direct 模式（`SQLITE_ENABLE_ATOMIC_WRITE`）下直接 `sqlite3OsWrite` 整页 |
| src/pager.c | `sqlite3PagerOpen` | 计算默认页大小 `szPageDflt`（起于 `SQLITE_DEFAULT_PAGE_SIZE`，按扇区大小与原子写能力上调至 `SQLITE_MAX_DEFAULT_PAGE_SIZE`），并调 `sqlite3PagerSetPagesize` 落地 |
| src/pager.c | `read32bits` / `write32bits` | 大端 32 位读写原语；`read32bits` 走 `sqlite3Get4byte`，`write32bits` 走 `put32bits`（即 `sqlite3Put4byte`） |
| src/pager.c | `sqlite3PagerPagecount` | 返回 `pPager->dbSize`，供 `lockBtree` 计算文件实际页数作为头内页数的回退值 |
| src/btree.c | `sqlite3BtreeOpen` | 文件头的第一个消费者：读 100 字节头 → 解析偏移 16/17 页大小与偏移 20 预留字节 → 调 `sqlite3PagerSetPagesize` → 计算 `usableSize` |
| src/btree.c | `sqlite3BtreeSetPageSize` | 改页大小 / 预留字节的对外入口：校验 `pageSize>=512 && pageSize<=SQLITE_MAX_PAGE_SIZE && ((pageSize-1)&pageSize)==0`，再调 `sqlite3PagerSetPagesize`；`BTS_PAGESIZE_FIXED` 已置位时返回 `SQLITE_READONLY` |
| src/btree.c | `lockBtree` | 文件头语义校验主战场：魔数、写/读版本号、载荷分数、页大小、预留空间的合法性与 `SQLITE_NOTADB` / `SQLITE_CORRUPT_BKPT` 判定 |
| src/btree.c | `newDatabase` | 新建库时整头初始化：魔数、页大小、版本号 `1`、`64/32/32` 载荷分数、autovacuum 标志、偏移 31 标记位 |
| src/btree.c | `btreeBeginTrans` | 开写事务时校正头内页数（偏移 28），使事务内回滚后仍可从第 1 页读回库大小 |
| src/btreeInt.h | `SQLITE_FILE_HEADER` | 文件头魔数宏，值 `"SQLite format 3"`，必须恰好 16 字节含结尾零字节 |
| src/sqliteLimit.h | `SQLITE_MAX_PAGE_SIZE` / `SQLITE_DEFAULT_PAGE_SIZE` / `SQLITE_MAX_DEFAULT_PAGE_SIZE` | 页大小上限 `65536`、默认页大小 `4096`、自选页大小上限 `8192` |

## 与主核 / AI 的关系

数据库文件头是 SQLite「文件自描述」设计的最前端，也是主核 `engine/db/` 与 AI 协作时最容易踩坑的一层。

**架构推论：** `lockBtree` 里「头内页数只有在变更计数器与 version-valid-for 号相等时才可信，否则回退到文件实际大小」的判定，说明 SQLite 对文件头的信任是**分级**的：魔数与格式版本号是硬校验（不通过即 `SQLITE_NOTADB`），而某些统计性字段（页数）只是「软优先」字段。这与主核 `engine/db/` 的做法形成对照 —— 主核把元数据放在独立的 catalog 里（`engine/db/index/catalog/`），文件本身没有自描述能力，因此不存在「文件头字段陈旧」这一整类问题；但代价是主核必须额外维护 catalog 的原子性。

**架构推论：** `sqlite3PagerReadFileheader` 断言「只能在 Pager 刚创建、尚未进 WAL 模式时调用」，是一条把**调用时机**写进代码的硬约束。主核若在分页器之上再做一层「文件头元数据缓存」，同样需要为缓存失效时机设定显式边界，否则会读到 WAL 已更新而缓存未失效的旧头。

**架构推论：** 页大小切换的三个前提（无页引用、非内存库或零页、大小确实变化）是一组**保守的可行性判定**，把「什么时候可以改变物理布局」压缩成三条可静态检查的条件。主核的行存/列存混合格式若需要在线调整页大小或 block 大小，同类约束会出现在「无活跃读事务 + 无活跃写事务」这类更粗粒度的条件上。

**架构推论：** `pager_write_changecounter` 同时写偏移 24 与 92，是用**两份相同数据的冗余**换取「本文件由哪个版本写过」的可判定性 —— 旧版本写文件时只更新前者，新版本通过两者是否相等来识别「头内统计信息已陈旧」。主核若需要跨版本兼容的自描述元数据，可以借鉴这种「冗余副本 + 不等即降级」的判据，比引入版本号协商协议便宜得多。

## 参考

- [SQLite Database File Format（sqlite.org 官方文档）](https://sqlite.org/fileformat2.html)
- [SQLite The Rollback Journal（sqlite.org 官方文档）](https://sqlite.org/fileformat2.html#the_rollback_journal)
- [SQLite Write-Ahead Logging（sqlite.org 官方文档）](https://sqlite.org/wal.html)
- [SQLite PRAGMA page_size（sqlite.org 官方文档）](https://sqlite.org/pragma.html#pragma_page_size)
- [SQLite Compile-Time Options（sqlite.org 官方文档）](https://sqlite.org/compile.html)
- [sqlite/sqlite 仓库中的 src/pager.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/pager.c)
- [sqlite/sqlite 仓库中的 src/btree.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/btree.c)
- [sqlite/sqlite 仓库中的 src/sqliteLimit.h（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/sqliteLimit.h)
- [SQLite Source Code Browser：src/pager.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/pager.c)
