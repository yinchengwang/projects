---
project: sqlite
feature: sqlite-record-format
page_type: feature
schema_version: 1
refs:
  - path: src/insert.c
    symbols: [sqlite3SetMakeRecordP5]
---

# 记录格式与 varint

## 一句话职责

记录格式规定一行数据在页内怎么排布：先一段变长整数（varint）写明记录头长度，紧跟一串 varint 序列类型，再是各字段的值；`sqlite3SetMakeRecordP5` 在开启 `SQLITE_ENABLE_NULL_TRIM` 时，把 `OP_MakeRecord` 的 P5 操作数改成「不可被裁掉的列数」，从而允许把行尾连续的全 NULL 列从记录里省掉。

## 机制是什么

一条记录（record）由三段构成。`src/vdbe.c` 的 `OP_MakeRecord` 分支上方的注释给出的布局是：

```
| hdr-size | type 0 | type 1 | ... | type N-1 | data0 | ... | data N-1 |
```

- **hdr-size**：记录头的字节数，**包含这个 varint 自身**。它同时也是「从记录开头到第一个字段值 `data0` 的偏移」——因为字段值紧跟在全部序列类型之后。
- **type 0 … type N-1**：每列一个 serial type（序列类型），编码成 varint。
- **data 0 … data N-1**：各列的实际值，紧跟在记录头之后连续排列。

**序列类型表**（`src/vdbeaux.c` 中 `sqlite3VdbeSerialType` / `sqlite3VdbeSerialTypeLen` 上方的注释表）：

| serial type | 数据字节数 | 含义 |
|---|---|---|
| 0 | 0 | NULL |
| 1 | 1 | 有符号整数 |
| 2 | 2 | 有符号整数 |
| 3 | 3 | 有符号整数 |
| 4 | 4 | 有符号整数 |
| 5 | 6 | 有符号整数 |
| 6 | 8 | 有符号整数 |
| 7 | 8 | IEEE 浮点 |
| 8 | 0 | 整数常量 0 |
| 9 | 0 | 整数常量 1 |
| 10、11 | — | 保留给将来扩展 |
| N≥12 且为偶数 | (N-12)/2 | BLOB |
| N≥13 且为奇数 | (N-13)/2 | 文本 |

`src/vdbeaux.c` 的注释还说明：类型 8 和 9 是 3.3.0 版本随文件格式 4 引入的，更老的 SQLite 版本读不懂这两种序列类型。

**varint** 是 SQLite 自己的变长整数编码，大端、每字节 7 位有效、最高位为续位标志。`src/util.c` 的 `sqlite3PutVarint` 分两档：`v<=0x7f` 时写 1 字节；`v<=0x3fff` 时写 2 字节；其余交给 `putVarint64`，而 `putVarint64` 内部逐次取 7 位并置续位，最长 9 字节（源码里有 `assert( n<=9 )`）。解码侧 `sqlite3GetVarint` 是与之对称的逐字节移位读取，`sqlite3GetVarint32` 是它的 32 位裁剪版（超出 32 位时把 `*v` 置为 `0xffffffff`）。

**本机制的边界：** 不负责 B-tree 单元（cell）的管理——那是 B-tree 层的事（`src/btree.c` 的 `cellSizePtr` / `btreeParseCellPtr` 负责把记录连同 rowid 打包成一个单元）；不负责值的类型亲和性转换（亲和性由 `src/insert.c` 的 `sqlite3TableAffinity` 生成的 `OP_Affinity` 指令在记录构造之前完成）。

## 怎么实现的

### 写路径：`OP_MakeRecord`

`src/vdbe.c` 中 `case OP_MakeRecord:` 的实现分两个阶段：先算，再写。

**第一阶段：从右往左扫描，算出每个字段的序列类型与总长度。**

它用一个循环从最后一个字段 `pLast` 倒着扫到 `pData0`，把每列的序列类型暂存进 `pRec->uTemp`，同时累加 `nHdr`（记录头字节数）、`nData`（数据区字节数）、`nZero`（尾部零字节数）。各分支的判定依据是 `Mem.flags` 上的位：

- **`MEM_Null`**：普通 NULL 记 `pRec->uTemp = 0`，`nHdr++`，不占数据区。但若同时带 `MEM_Zero` 位（源码注释说明这是虚拟表的 `xColumn` 方法在 UPDATE 时算未变化列、却从不调用 `sqlite3_result_xxxxx()` 造成的），记 `pRec->uTemp = 10` —— 类型 10 是内部专用序列类型，用来把「无变化」标志透传给 `xUpdate`。
- **`MEM_Int` / `MEM_IntReal`**：整数按绝对值选宽度。`i<0` 时取 `uu = ~i`，否则 `uu = i`，然后逐档比较：`uu<=127` → 序列类型 1（1 字节）；`uu<=32767` → 2；`uu<=8388607` → 3；`uu<=2147483647` → 4；`uu<=140737488355327LL` → 5（6 字节）；否则 → 6（8 字节）。这里有一条例外：若 `uu<=127` 且 `(i&1)==i`（即值为 0 或 1）且 `p->minWriteFileFormat>=4`，则序列类型取 `8+(u32)uu`，即 0 写成类型 8、1 写成类型 9，数据区不占字节。
- **`MEM_Real`**：序列类型 7，`nData += 8`。
- **`MEM_Str` / `MEM_Blob`**：`serial_type = (len*2) + 12 + ((pRec->flags & MEM_Str)!=0)`。即偶数偏移 12 得 BLOB，奇数偏移 13 得文本——与上表的 (N-12)/2、(N-13)/2 一一对应。若带 `MEM_Zero` 位，还要把 `pRec->u.nZero` 累加进长度并把序列类型加上 `2*(u32)pRec->u.nZero`。
- **`MEM_IntReal` 且需要 8 字节**：整数放不下 6 字节时，若该值带 `MEM_IntReal` 位，源码注释说「we might as well make it an 8-byte floating point value」，于是把它转成 `MEM_Real`、序列类型改成 7。

**第二阶段：算记录头长度并落盘。**

记录头长度用了一个自洽的判据：若 `nHdr<=126`（头部里除长度 varint 外的部分），则头部总长是 `nHdr+1`，因为 1 字节就能装下这个长度；否则长度 varint 本身要占多字节，于是 `nVarint = sqlite3VarintLen(nHdr); nHdr += nVarint; if( nVarint<sqlite3VarintLen(nHdr) ) nHdr++;`——最后一次自增用于处理「加上 varint 长度后 varint 自己又变长」的情形。`nByte = nHdr+nData` 是记录总长度。

长度检查有两个出口：缓冲区不够时先判 `if( nByte+nZero>db->aLimit[SQLITE_LIMIT_LENGTH] ) goto too_big;`，再 `sqlite3VdbeMemClearAndResize(pOut, (int)nByte+OVERRUN)`。`SQLITE_LIMIT_LENGTH` 的默认值由 `src/sqliteLimit.h` 给出：`# define SQLITE_MAX_LENGTH 1000000000`。`OVERRUN` 宏的值由编译期条件决定：`#if SQLITE_MAX_LENGTH<=2147483640 && SQLITE_BYTEORDER>0` 时取 7，否则取 0。

落盘时先写头长度：`if( nHdr<0x80 ){ *(zHdr++) = nHdr; }` —— 1 字节直写；否则 `zHdr += sqlite3PutVarint(zHdr,nHdr);`。再逐字段写序列类型：`serial_type<=7` 时单字节直写（`*(zHdr++) = serial_type;`），`serial_type<0x80` 时也单字节直写，否则调 `sqlite3PutVarint(zHdr, serial_type)`。字段值随后按序列类型对应的字节宽度依次写入数据区，其中类型 1–6 的字节宽度取自 `sqlite3SmallTypeSizes`（`src/vdbeaux.c` 中定义为 `const u8 sqlite3SmallTypeSizes[128]`，其前 12 项为 `0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0`），类型 7 的浮点按大端 IEEE 754 写入。

**亲和性在记录构造之前施加。** `OP_MakeRecord` 指令的 P4 操作数若非空，会先对所有输入寄存器逐个调 `applyAffinity(pRec, zAffinity[0], encoding)`。这个亲和性字符串由 `src/insert.c` 的 `sqlite3TableAffinity` 通过 `sqlite3VdbeChangeP4(v, -1, zColAff, i)` 写到 `OP_MakeRecord` 上（该函数在 `iReg==0` 时改写上一条指令的 P4，源码里断言上一条必须是 `OP_MakeRecord`）。

### `sqlite3SetMakeRecordP5`：允许裁掉行尾 NULL 列

`src/insert.c` 中该函数的定义被包在 `#ifdef SQLITE_ENABLE_NULL_TRIM` 里；`src/sqliteInt.h` 中对应地声明为 `void sqlite3SetMakeRecordP5(Vdbe*,Table*);`，而在未定义该宏时 `# define sqlite3SetMakeRecordP5(A,B)` 把它降级成空宏——所以调用点无需条件编译。

函数体做三件事：

1. **schema 格式版本门槛。** `if( pTab->pSchema->file_format<2 ) return;` —— 函数注释写明「Records with omitted columns are only allowed for schema format version 2 and later (SQLite version 3.1.4, 2005-02-20)」。版本不够时直接返回，P5 保持原值。
2. **从最右列往前找可裁剪边界。** `for(i=pTab->nCol-1; i>0; i--){ if( pTab->aCol[i].iDflt!=0 ) break; if( pTab->aCol[i].colFlags & COLFLAG_PRIMKEY ) break; }` —— 遇到「有 DEFAULT 值的列」（`aCol[i].iDflt` 是 1 起的 DEFAULT 序号，0 表示没有，见 `src/sqliteInt.h` 中 `Column.iDflt` 的注释）或「属于主键的列」（`COLFLAG_PRIMKEY` 定义在 `src/sqliteInt.h`，取值 `0x0001`）就停下。
3. **写 P5。** `sqlite3VdbeChangeP5(v, i+1);`。`sqlite3VdbeChangeP5`（`src/vdbeaux.c`）的实现是 `if( p->nOp>0 ) p->aOp[p->nOp-1].p5 = p5;` —— 改的是**最后一条**指令的 P5，因此调用方必须保证刚发出的最后一条指令就是目标 `OP_MakeRecord`。

P5 的语义由 `src/vdbe.c` 的 `OP_MakeRecord` 注释规定：开了 `SQLITE_ENABLE_NULL_TRIM` 时，P5 是「最右侧仍允许被裁剪的表列的索引」；没开时，P5 取 `OPFLAG_NOCHNG_MAGIC`（`src/sqliteInt.h` 定义为 `0x6d`），表示允许 `OP_MakeRecord` 接受序列类型 10 的无变化记录。

裁剪动作在 `OP_MakeRecord` 里执行：`if( pOp->p5 ){ while( (pLast->flags & MEM_Null)!=0 && nField>pOp->p5 ){ pLast--; nField--; } }`。即从最右列开始，只要该列是 NULL 且剩余列数仍多于 P5，就把它丢掉。源码注释解释了这个 P5 的定义：「If P5>0 then it will be one more than the index of the right-most column with a non-NULL default value」。

**`sqlite3SetMakeRecordP5` 的调用方**有两个，都在 `src/insert.c`：

- `sqlite3GenerateConstraintChecks` 中生成表记录处：`sqlite3VdbeAddOp3(v, OP_MakeRecord, regNewData+1, pTab->nNVCol, regRec);` 紧跟着 `sqlite3SetMakeRecordP5(v, pTab);`。这里 `pTab->nNVCol` 是「非 VIRTUAL 的列数」（`src/sqliteInt.h` 中 `Table.nNVCol` 的注释）。
- 生成索引记录处：`sqlite3VdbeAddOp3(v, OP_MakeRecord, regIdx, pIdx->nColumn, aRegIdx[ix]);` 之后，`在 pIdx->idxType==SQLITE_IDXTYPE_PRIMARYKEY` 时调 `sqlite3SetMakeRecordP5(v, pIdx->pTable);`。

**没有开 `SQLITE_ENABLE_NULL_TRIM` 时的回退路径：** `sqlite3SetMakeRecordP5` 被降级为空宏，P5 保持为 `OPFLAG_NOCHNG_MAGIC`。这个值由另外三处代码显式写入：`src/select.c` 中两处（`sqlite3VdbeChangeP5(v, OPFLAG_NOCHNG_MAGIC);`，分别在「UPDATE ... FROM 写到虚拟表或视图」与物化输出路径，条件是 `!defined(SQLITE_ENABLE_NULL_TRIM) && defined(SQLITE_DEBUG)`）、`src/update.c` 中一处（`sqlite3VdbeChangeP5(v, OPFLAG_NOCHNG_MAGIC);`，条件是 `defined(SQLITE_DEBUG) && !defined(SQLITE_ENABLE_NULL_TRIM)`，注释说明用途是「Signal an assert() within OP_MakeRecord that it is allowed to accept no-change records with serial_type 10」）。`OP_MakeRecord` 里对应的检查是 `assert( pOp->p5==OPFLAG_NOCHNG_MAGIC || CORRUPT_DB );`。

### 读路径：`OP_Column` 与 `sqlite3VdbeSerialGet`

`src/vdbe.c` 的 `OP_Column` 分支有一条内联的 `switch( t )`，源码注释说它是「an in-line variant of `sqlite3VdbeSerialGet()` that has been optimized for the OP_Column opcode」。通用版在 `src/vdbeaux.c` 的 `sqlite3VdbeSerialGet`：类型 0 与 11 解成 `MEM_Null`；类型 1–6 分别用 `ONE_BYTE_INT` / `TWO_BYTE_INT` / `THREE_BYTE_INT` / `FOUR_BYTE_INT` / `SIX_BYTE_INT` / `sqlite3Get8byte` 读出大端补码整数；类型 7 交给 `sqlite3VdbeSerialGet7`，其中 `u64 x = sqlite3Get8byte(buf);` 读出大端 IEEE 754 双精度，`IsNaN(x)` 为真时把结果置成 `MEM_Null` 并返回 1，否则置 `MEM_Real`；类型 8 与 9 解成 `pMem->u.i = t-8`。

`OP_Column` 解析记录头的过程（`src/vdbe.c`）是：先取 `aOffset[0]`（头长度）与 `zEndHdr = zData + aOffset[0]`；循环里 `if( (pC->aType[i] = t = zHdr[0])<0x80 ){ zHdr++; offset64 += sqlite3VdbeOneByteSerialTypeLen(t); } else { zHdr += sqlite3GetVarint32(zHdr, &t); pC->aType[i] = t; offset64 += sqlite3VdbeSerialTypeLen(t); }` —— 序列类型小于 0x80 时单字节直读，否则走 varint。

记录损坏的判据写在同一处，源码注释列了三条：「(1) the bytes of the header extend past the declared header size (2) the entire header was used but not all data was used (3) the end of the data extends beyond the end of the record」，对应的判断是 `if( (zHdr>=zEndHdr && (zHdr>zEndHdr || offset64!=pC->payloadSize)) || (offset64 > pC->payloadSize) )`，命中后走 `op_column_corrupt` 分支。

**列数不足记录时的回退路径：** 若解析完头部后 `pC->nHdrParsed<=p2`（记录里的列数少于要取的那一列），`OP_Column` 走 `sqlite3VdbeMemSetNull(pDest);` 并 `goto op_column_out` —— 取到 NULL 而不是报错。

### `sqlite3VdbeSerialTypeLen` 与 `sqlite3SmallTypeSizes`

`src/vdbeaux.c` 中 `sqlite3VdbeSerialTypeLen(u32 serial_type)`：`if( serial_type>=128 ){ return (serial_type-12)/2; }` —— 大序列类型直接按公式算长度；否则返回查表 `sqlite3SmallTypeSizes[serial_type]`。查表分支上有断言 `serial_type<12 || sqlite3SmallTypeSizes[serial_type]==(serial_type - 12)/2;`，保证表项与公式一致。

`sqlite3SmallTypeSizes` 是一张 128 项的 `u8` 表，声明在 `src/vdbeInt.h`（`extern const u8 sqlite3SmallTypeSizes[];`），定义在 `src/vdbeaux.c`。表项按「序列类型 → 数据字节数」排布：前 12 项为 `0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0`（类型 0、8、9、10、11 的数据长度为 0，类型 5 占 6 字节，类型 6 占 8 字节，类型 7 占 8 字节）；从类型 12 起每两个类型共用一个长度值（12 与 13 长度为 0，14 与 15 长度为 1，依次递增），到类型 126、127 长度为 57。

## 关键取舍

- **记录头里放序列类型串，值集中放尾部。** 表记录的所有序列类型聚在头部、所有值紧跟其后，索引记录则序列类型与值交替出现。`src/vdbeaux.c` 的注释把这两种布局并列讲明：「In an SQLite index record, the serial type is stored directly before the blob of data that it corresponds to. In a table record, all serial types are stored at the start of the record, and the blobs of data at the end.」**架构推论：** 头部集中放类型串，使得读路径可以只解析头部就知道每列的偏移与长度（`OP_Column` 的 `pC->aType[]` 与 `aOffset[]` 缓存正是如此），代价是读第 N 列也要先扫过前 N-1 个序列类型。
- **整数宽度按绝对值分 6 档。** 源码里逐档写死 `127 / 32767 / 8388607 / 2147483647 / 140737488355327LL` 五个阈值，分别对应 1/2/3/4/6 字节，再往上是 8 字节。**架构推论：** 这套阈值是「用最少字节装下该值」的最优切分点，代价是小整数也要占掉一个序列类型槽位（类型 1 的数据区 1 字节），与类型 8/9（0 和 1，零数据字节）的零开销方案并存。
- **类型 8/9 只在新文件格式下启用。** 0 和 1 被编码成 `8+(u32)uu`，前提是 `p->minWriteFileFormat>=4`；`src/vdbeaux.c` 注释写明「The 8 and 9 types were added in 3.3.0, file format 4. Prior versions of SQLite will not understand those serial types.」**架构推论：** 用文件格式版本号做门槛，把「老读者读不懂」这一兼容性问题挡在写入侧，而不是在读取侧做版本分支。代价是要维护 `minWriteFileFormat` 这个随库演进的变量。
- **类型 10 是内部专用的，不进公开语义。** 它由 `MEM_Null|MEM_Zero` 组合产生，仅用于虚拟表 UPDATE 的「无变化」透传；`OP_MakeRecord` 里的断言 `assert( pOp->p5==OPFLAG_NOCHNG_MAGIC || CORRUPT_DB );` 要求调用方显式声明「我允许类型 10」。**架构推论：** 用一个保留类型号承载内部语义，并用 P5 操作数把「是否允许」变成调用方的显式承诺，避免了核心路径上多一个运行时分支。代价是 P5 这一个操作数承担了两义（NULL 裁剪边界 vs 无变化许可），两者靠编译宏 `SQLITE_ENABLE_NULL_TRIM` 二选一。
- **NULL 裁剪只从行尾开始，且不碰 DEFAULT 列与主键列。** `sqlite3SetMakeRecordP5` 从最右列往前扫，遇到 `aCol[i].iDflt!=0` 或 `aCol[i].colFlags & COLFLAG_PRIMKEY` 就停；`OP_MakeRecord` 里的裁剪循环 `while( (pLast->flags & MEM_Null)!=0 && nField>pOp->p5 )` 也只能从尾部连续裁。**架构推论：** 行尾裁剪之所以安全，是因为尾部 NULL 列在读回时能靠「记录列数不足 → 取 NULL」的路径补回来（`OP_Column` 里 `pC->nHdrParsed<=p2` 分支），而中间的列不能省——省掉会改变列的对应关系。代价是 NULL 稀疏分布在行中间时完全省不掉。
- **头部长度用「自洽修正」而不是先算后填。** `nHdr<=126` 时直接 `nHdr+1`；否则用 `nVarint = sqlite3VarintLen(nHdr); nHdr += nVarint; if( nVarint<sqlite3VarintLen(nHdr) ) nHdr++;` 处理 varint 自身变长的情况。**架构推论：** 记录头长度是「头部长度」与「头部末尾到数据区的偏移」这两个语义的合体，所以它必须自指（长度 varint 含自己）。源码在写头部时用 `if( nHdr<0x80 )` 直写 1 字节、否则调 `sqlite3PutVarint`，与 `sqlite3VarintLen` 的判定保持一致。
- **大端字节序固定，不用主机字节序。** 整数与浮点都按大端写入（`sqlite3Get8byte` 读、`swapMixedEndianFloat` 只处理 ARM7 那种混合端序浮点的历史问题）。**架构推论：** 固定字节序让数据库文件跨平台可移植，代价是写入时要做字节序转换——`OP_MakeRecord` 里为此引入了 `OVERRUN` 宏：当 `SQLITE_MAX_LENGTH<=2147483640 && SQLITE_BYTEORDER>0` 时多分配 7 字节溢出区，好让整数编码走 `memcpy` 一次性完成。
- **写路径把序列类型判定内联进 `OP_MakeRecord`。** `src/vdbeaux.c` 里 `sqlite3VdbeSerialType` 整段被 `#if 0` 包起来，注释解释：「By moving this routine in-line, we can omit some redundant tests and make that opcode a lot faster. So this routine is now only used by the STAT3 logic and STAT3 support has ended. The code is kept here for historical reference only.」**架构推论：** 把通用函数内联成 opcode 的专用逻辑，是把「每次调用都要过一遍的分支」消掉的标准手法；代价是序列类型的定义在源码里出现了两份（一份在 `#if 0` 的历史参考里，一份在 `OP_MakeRecord` 内联体里），改规则时必须两处同改。
- **记录长度上限走 `SQLITE_LIMIT_LENGTH` 而不是硬编码。** `OP_MakeRecord` 的超长出口是 `if( nByte+nZero>db->aLimit[SQLITE_LIMIT_LENGTH] ) goto too_big;`，而 `db->aLimit[]` 的默认值来自 `src/sqliteLimit.h` 的 `SQLITE_MAX_LENGTH`（默认 `1000000000`）。**架构推论：** 用「连接级 limit」而不是「编译期常量」把关，使得同一个二进制能按调用场景收紧上限。代价是每次写记录都要多查一次 limit 表。
- **记录与 B-tree 单元分层。** 记录只是页内的一段字节；把它连同 rowid 打包成单元、算单元在页里的偏移、决定是否溢出到溢出页，是 B-tree 层的事（`src/btree.c` 的 `cellSizePtr` / `cellSizePtrTableLeaf` 负责从单元头里解出 payload 长度并据此算单元总长）。**架构推论：** 把「值的字节表示」与「单元的存储布局」分成两层，让同一套记录格式可以同时服务于表 B-tree、索引 B-tree、以及 `OP_MakeRecord` 在物化（materialize）、ephemeral table 等非 B-tree 场景下的产物。代价是读路径要跨两层：`OP_Column` 先从 B-tree 拿到 payload，再自己解析记录头。

## 与主核 / AI 的关系

主核 `engine/db/` 是从零手写的多模态数据库内核，不复用 SQLite 的源码；本页记录的是 SQLite 的机制，不构成主核的实现依据。

**架构推论：** SQLite 把「记录头长度自指」「序列类型与值分离」「行尾 NULL 可裁且裁剪边界由写方计算」这三件事组合起来，使得页内记录可以做到「按列随机访问但只需顺序扫头部」。主核若也采用「头部集中放元信息、值集中放尾部」的记录布局，则元信息头的解析应当与列数解耦——SQLite 这里是靠 `pC->nHdrParsed` 与 `pC->aType[]` / `aOffset[]` 三个游标级缓存实现增量解析的，主核可直接借鉴「按需解析到第 N 列」这一策略，而不是每次都全量解析。

**架构推论：** `sqlite3SetMakeRecordP5` 的设计里有一处值得主核留意：裁剪边界不是由记录层自己推断，而是由**知道表结构的那一层**（`src/insert.c` 的代码生成阶段，握着 `Table` 与 `Column` 的默认值、主键标志）算好，再以参数形式传给执行层。这条「结构知识在编译期、字节级决策在执行期」的分工，正是主核做多模态记录（变长 payload、变长元数据头）时同样需要的分层。

**架构推论：** `OP_MakeRecord` 里「序列类型由 `Mem.flags` 决定、宽度按绝对值分档、类型 8/9 依赖文件格式版本」这套编码方式，对主核的多模态字段有直接参考价值：可以同样把「固定宽度小整数用零开销类型号」「可变长 payload 的长度编码进类型号」作为编码原则，但主核需要额外考虑版本兼容——SQLite 用 `minWriteFileFormat` 这一现成机制承载了「新类型号不能被老读者误解」的约束，主核若要引入新的类型号（例如向量、稀疏索引），也需要一条等价的版本门槛。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/insert.c | `sqlite3SetMakeRecordP5` | 在 `SQLITE_ENABLE_NULL_TRIM` 下算出不可裁剪的列数并写入最后一条指令的 P5：schema 格式版本 < 2 时直接返回；从最右列往左扫，遇到有 DEFAULT 的列或带 `COLFLAG_PRIMKEY` 的列即停；最后调 `sqlite3VdbeChangeP5(v, i+1)` |
| src/insert.c | `sqlite3TableAffinity` | 把表的列亲和性字符串写进 `OP_MakeRecord` 的 P4（`iReg==0` 时改写上一条指令的 P4，断言上一条是 `OP_MakeRecord`）；STRICT 表则改为插入 `OP_TypeCheck` |
| src/insert.c | `sqlite3GenerateConstraintChecks` | 生成表记录的调用方：`sqlite3VdbeAddOp3(v, OP_MakeRecord, regNewData+1, pTab->nNVCol, regRec);` 后调 `sqlite3SetMakeRecordP5(v, pTab);` |
| src/insert.c | `sqlite3GenerateIndexKey` | 生成索引记录的调用方：`sqlite3VdbeAddOp3(v, OP_MakeRecord, regIdx, pIdx->nColumn, aRegIdx[ix]);` 后在 `pIdx->idxType==SQLITE_IDXTYPE_PRIMARYKEY` 时调 `sqlite3SetMakeRecordP5(v, pIdx->pTable);` |
| src/vdbe.c | `OP_MakeRecord` | 记录写路径：先按 `Mem.flags` 反扫各字段定序列类型与长度，再算记录头长度（`nHdr<=126` 走 `nHdr+1`，否则用 `sqlite3VarintLen` 自洽修正），最后写头长度 varint、序列类型串与字段值 |
| src/vdbe.c | `OP_Column` | 记录读路径：增量解析记录头写入 `pC->aType[]` / `aOffset[]`，内联 `switch(t)` 解出字段值；列数不足时走 `sqlite3VdbeMemSetNull` 返回 NULL |
| src/vdbe.c | `sqlite3SmallTypeSizes`（引用点） | `OP_MakeRecord` 写序列类型 1–6 的字段值时用它取字节宽度；该表定义在 `src/vdbeaux.c` |
| src/vdbeaux.c | `sqlite3VdbeSerialGet` | 记录读路径的通用版：按序列类型解出 NULL、1–6 的补码整数、7 的浮点、8/9 的常量 0/1；类型 7 经 `sqlite3VdbeSerialGet7` 处理并把 NaN 归为 `MEM_Null` |
| src/vdbeaux.c | `sqlite3VdbeSerialGet7` | 反序列化 8 字节大端 IEEE 754 双精度：`sqlite3Get8byte` 取值后 `swapMixedEndianFloat`，`IsNaN(x)` 为真时置 `MEM_Null` 并返回 1 |
| src/vdbeaux.c | `sqlite3VdbeSerialTypeLen` | 由序列类型求数据长度：`serial_type>=128` 时返回 `(serial_type-12)/2`，否则查 `sqlite3SmallTypeSizes`，并断言表项与公式一致 |
| src/vdbeaux.c | `sqlite3VdbeSerialType` | 序列类型判定函数，历史保留：整段被 `#if 0` 包住，注释说明它已内联进 `OP_MakeRecord`，「The code is kept here for historical reference only」 |
| src/vdbeaux.c | `sqlite3SmallTypeSizes` | 128 项的 `u8` 常量表，给出序列类型 0–127 的数据字节长度，前 12 项为 `0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0` |
| src/vdbeaux.c | `sqlite3VdbeChangeP5` | 改写最后一条 VDBE 指令的 P5 操作数：`if( p->nOp>0 ) p->aOp[p->nOp-1].p5 = p5;` —— `sqlite3SetMakeRecordP5` 靠它设定 NULL 裁剪边界 |
| src/util.c | `sqlite3PutVarint` | varint 写入：`v<=0x7f` 写 1 字节，`v<=0x3fff` 写 2 字节，其余转 `putVarint64` |
| src/util.c | `putVarint64` | 逐次取 7 位并置续位标志，最长 9 字节（`assert( n<=9 )`），最后逆序输出为大端 |
| src/util.c | `sqlite3GetVarint` | varint 读取：逐字节移位，最多读 9 字节，返回实际读到的字节数 |
| src/util.c | `sqlite3GetVarint32` | varint 的 32 位裁剪版：两字节与三字节走快速路径，更长则调 `sqlite3GetVarint`，超出 32 位时把 `*v` 置为 `0xffffffff` |
| src/util.c | `sqlite3VarintLen` | 返回存储给定 64 位整数所需的字节数：`for(i=1; (v >>= 7)!=0; i++)` |
| src/sqliteInt.h | `sqlite3SetMakeRecordP5`（声明） | 未定义 `SQLITE_ENABLE_NULL_TRIM` 时降级为 `# define sqlite3SetMakeRecordP5(A,B)` 空宏，调用点因此无需条件编译 |
| src/sqliteInt.h | `COLFLAG_PRIMKEY` | 宏，取值 `0x0001`；`sqlite3SetMakeRecordP5` 用它识别不能被裁掉的主键列 |
| src/sqliteInt.h | `Column.iDflt` | 结构体字段：1 起的 DEFAULT 序号，0 表示该列没有默认值；非 0 即不可裁剪 |
| src/sqliteInt.h | `Table.nNVCol` | 结构体字段：非 VIRTUAL 的列数，作为 `OP_MakeRecord` 的 P2 传入 |
| src/sqliteInt.h | `OPFLAG_NOCHNG_MAGIC` | 宏，取值 `0x6d`；未开 `SQLITE_ENABLE_NULL_TRIM` 时 `OP_MakeRecord` 的 P5 取此值，表示允许序列类型 10 |
| src/vdbeInt.h | `sqlite3SmallTypeSizes`（声明） | `extern const u8 sqlite3SmallTypeSizes[];` |
| src/sqliteLimit.h | `SQLITE_MAX_LENGTH` | 宏，默认值 `1000000000`；`db->aLimit[SQLITE_LIMIT_LENGTH]` 的默认值来源，也是 `OP_MakeRecord` 里 `OVERRUN` 宏取 7 的前提条件之一 |
| src/select.c | `sqlite3VdbeChangeP5(v, OPFLAG_NOCHNG_MAGIC)` | 未开 `SQLITE_ENABLE_NULL_TRIM` 且开 `SQLITE_DEBUG` 时的回退路径之一：物化输出时为 `OP_MakeRecord` 标注「允许无变化记录」 |
| src/update.c | `sqlite3VdbeChangeP5(v, OPFLAG_NOCHNG_MAGIC)` | 同上回退路径：把虚拟表参数行写入 ephemeral table 前为 `OP_MakeRecord` 标注 P5 |
| src/btree.c | `cellSizePtr` / `cellSizePtrTableLeaf` | B-tree 层从单元头解出 payload 长度并算出单元总长，是记录格式的上一层——不属于本页边界，但决定了记录如何进入页 |

## 参考

- [SQLite 文件格式：记录格式（sqlite.org 官方文档）](https://sqlite.org/fileformat2.html#record_format) —— 给出记录的官方定义：头长度 varint、序列类型串、字段值区的布局与各序列类型的字节宽度。
- [SQLite 文件格式（sqlite.org 官方文档）](https://sqlite.org/fileformat2.html) —— 说明文件头、页结构、单元布局与溢出页，界定记录格式在整体文件格式中的位置。
- [SQLite 官方文档：VDBE 指令 OP_MakeRecord / OP_Column](https://sqlite.org/opcode.html) —— 说明这两条指令的操作数语义与记录头解析规则。
- [SQLite 官方文档：数据类型与存储类别](https://sqlite.org/datatype3.html) —— 说明 NULL、整数、浮点、文本、BLOB 五种存储类别，与序列类型表的对应关系。
- [sqlite/sqlite 仓库中的 src/vdbe.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c) —— 支撑本页关于 `OP_MakeRecord` 写路径与 `OP_Column` 读路径的行为描述。
- [sqlite/sqlite 仓库中的 src/vdbeaux.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/vdbeaux.c) —— 支撑序列类型表、`sqlite3SmallTypeSizes`、`sqlite3VdbeSerialTypeLen`、`sqlite3VdbeSerialGet` 与 `sqlite3VdbeChangeP5` 的描述。
- [sqlite/sqlite 仓库中的 src/util.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/util.c) —— 支撑 varint 编解码（`sqlite3PutVarint` / `sqlite3GetVarint` / `sqlite3VarintLen`）的实现描述。
- [sqlite/sqlite 仓库中的 src/insert.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/insert.c) —— 支撑 `sqlite3SetMakeRecordP5`、`sqlite3TableAffinity` 及其两个调用点的描述。
