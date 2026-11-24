---
project: sqlite
feature: column-access
page_type: feature
schema_version: 1
refs:
  - path: src/vdbeapi.c
    symbols: [sqlite3_column_int, sqlite3_column_text, sqlite3_column_type, sqlite3_column_blob]
---

# 结果列读取

## 一句话职责

把结果行里已经算好的寄存器值，按调用方给的列号取出来，并按调用方要的格式（整数 / 文本 / 浮点 / Blob）做就地类型转换——`src/vdbeapi.c` 的 `sqlite3_column_int`、`sqlite3_column_text`、`sqlite3_column_type`、`sqlite3_column_blob` 是这条窄门上的四个入口。

## 机制是什么

SQLite 的「结果行读取」是一个**两段式**协议。

**第一段（产生行）**：虚拟机执行到 `src/vdbe.c` 的 `OP_ResultRow` 操作码时，把结果行寄存器区的首地址记到 `p->pResultRow`（语句对象上的一个指针），把 `rc` 置为 `SQLITE_ROW` 后跳转到 `vdbe_return` 退出虚拟机。调用方拿到的 `SQLITE_ROW` 就意味着「有一行可取」。

**第二段（取列）**：调用方在同一行上按列号 `i`（从 0 开始）调用 `sqlite3_column_*` 系列。这四个函数在 `src/vdbeapi.c` 里的函数体都极短，形状完全一致：

```c
int sqlite3_column_int(sqlite3_stmt *pStmt, int i){
  int val = sqlite3_value_int( columnMem(pStmt,i) );
  columnMallocFailure(pStmt);
  return val;
}
```

也就是说，**每个 `sqlite3_column_XXX` 都是三步**：

1. 调 `columnMem(pStmt, i)` 拿到这一列对应的 `Mem *`（寄存器槽）。
2. 把 `Mem *` 交给对应的 `sqlite3_value_XXX` 做取值与类型转换。
3. 调 `columnMallocFailure(pStmt)` 处理「转换过程中可能发生的内存分配失败」。

`sqlite3_column_text` 调 `sqlite3_value_text`，`sqlite3_column_blob` 调 `sqlite3_value_blob`，`sqlite3_column_type` 调 `sqlite3_value_type`，`sqlite3_column_int` 调 `sqlite3_value_int`。

**关键点：`sqlite3_column_*` 不做「选列」这件事，它做的是「按类型转换着取」。** 同一个 `Mem` 里可能同时存着数值表示和文本表示（`src/vdbeInt.h` 的 `MEM_Str` 标志可以与 `MEM_Int` / `MEM_Real` / `MEM_IntReal` 共存），取哪种表示由调用方调哪个函数决定，转换是**就地**发生的——改的是寄存器本身。

**类型的两套概念**需要分清：

- `sqlite3_column_type` 返回**存储类型**（storage class），取值来自 `src/sqlite.h.in` 的 `SQLITE_INTEGER`（1）、`SQLITE_FLOAT`（2）、`SQLITE_TEXT`（3）、`SQLITE_BLOB`（4）、`SQLITE_NULL`（5）。
- `sqlite3_column_int` / `sqlite3_column_text` 等返回的是**按请求格式转换后的值**，转换后的实际存储类型**不保证**与转换前一致。

**边界**：本机制不负责列的类型定义（类型亲和性、声明类型），那是 C03 的领域；也不负责参数绑定（`sqlite3_bind_*`），那是 E04 的领域。列的值本身由虚拟机算好放在寄存器里，本机制只负责「取出来 + 转换」。

## 怎么实现的

### `columnMem`：唯一的列定位入口

`columnMem` 是 `src/vdbeapi.c` 里的静态函数，四个 `sqlite3_column_XXX` 都走它：

```c
static Mem *columnMem(sqlite3_stmt *pStmt, int i){
  Vdbe *pVm;
  Mem *pOut;

  pVm = (Vdbe *)pStmt;
  if( pVm==0 ) return (Mem*)columnNullValue();
  assert( pVm->db );
  sqlite3_mutex_enter(pVm->db->mutex);
  if( pVm->pResultRow!=0 && i<pVm->nResColumn && i>=0 ){
    pOut = &pVm->pResultRow[i];
  }else{
    sqlite3Error(pVm->db, SQLITE_RANGE);
    pOut = (Mem*)columnNullValue();
  }
  return pOut;
}
```

逐条拆开：

- **语句为空**：`pVm==0` 时返回 `columnNullValue()` 指向的静态 NULL 值对象，不报错。
- **加锁**：`sqlite3_mutex_enter(pVm->db->mutex)`，持连接互斥锁。这保证与 `sqlite3_step` 的执行不并发。
- **正常路径的三个条件**：`pVm->pResultRow!=0`（当前确实有一行结果）、`i<pVm->nResColumn`（列号小于结果列数）、`i>=0`（列号非负）。三者同时成立才返回 `&pVm->pResultRow[i]`。`nResColumn` 是结果列数，取值可由 `sqlite3_column_count` 读到——它的实现就是 `return pVm->nResColumn;`。
- **越界路径**：任一条件不成立就调 `sqlite3Error(pVm->db, SQLITE_RANGE)`。`sqlite3Error`（`src/util.c`）置 `db->errCode = err_code`，并调 `sqlite3ErrorFinish` 清掉旧错误消息；随后返回 `columnNullValue()`——**越界时读到的是 NULL 值，不是崩溃，但连接上的错误码已被置为 `SQLITE_RANGE`**。

`columnNullValue`（`src/vdbeapi.c`）返回指向一个静态 `Mem` 的指针，该 `Mem` 的 `flags` 置为 `MEM_Null`、`z` 置 0、`n` 置 0。源码注释说明用静态对象而不是局部变量，是因为 `Mem` 结构体含 `i64` 成员，在 x86 等某些架构上局部变量可能未对齐，而静态对象能保证对齐。

### `sqlite3_value_type`：把 `Mem` 标志翻译成存储类型

`sqlite3_value_type`（`src/vdbeapi.c`）不读数据，只读 `Mem` 的 `flags`：

```c
int sqlite3_value_type(sqlite3_value* pVal){
  static const u8 aType[] = { ... };
  ...
  return aType[pVal->flags&MEM_AffMask];
}
```

`aType` 是一张 64 项的查表数组，索引是 `pVal->flags & MEM_AffMask`。`MEM_AffMask` 定义在 `src/vdbeInt.h`，值为 `0x003f`（注释写「Mask of affinity bits」），它覆盖了 `MEM_Null`（0x0001）、`MEM_Str`（0x0002）、`MEM_Int`（0x0004）、`MEM_Real`（0x0008）、`MEM_Blob`（0x0010）、`MEM_IntReal`（0x0020）这六个类型位。

查表把标志组合映射到 5 个存储类型码，例如：

- `0x00` → `SQLITE_BLOB`（注释标注「not possible」）
- `0x01` → `SQLITE_NULL`
- `0x02` → `SQLITE_TEXT`
- `0x04` → `SQLITE_INTEGER`
- `0x08` → `SQLITE_FLOAT`
- `0x06`（INTEGER + TEXT）→ `SQLITE_INTEGER`
- `0x0a`（FLOAT + TEXT）→ `SQLITE_FLOAT`
- `0x20` → `SQLITE_FLOAT`（注释标注「INTREAL」）
- `0x10` → `SQLITE_BLOB`

表里那些注释为「not possible」的槽位统一填 `SQLITE_NULL`——它们是留给未来标志组合的占位。

`sqlite3_value_type` 在 `SQLITE_DEBUG` 编译下还有一段交叉校验：它按 `MEM_Null` / `(MEM_Real|MEM_IntReal)` / `MEM_Int` / `MEM_Str` 的顺序独立推导一遍 `eType`，然后断言 `eType == aType[pVal->flags&MEM_AffMask]`。也就是说**查表法与直接判断法在调试构建下必须一致**。

### `sqlite3_column_int` → `sqlite3_value_int` → `sqlite3VdbeIntValue`

`sqlite3_column_int` 拿 `Mem *` 后调 `sqlite3_value_int`，后者是：

```c
int sqlite3_value_int(sqlite3_value *pVal){
  return (int)sqlite3VdbeIntValue((Mem*)pVal);
}
```

注意这里是 `(int)` 截断——`sqlite3_column_int` 返回 32 位整数，超出范围的值被截断；需要完整 64 位必须用 `sqlite3_column_int64`。

`sqlite3VdbeIntValue`（`src/vdbemem.c`）是真正的取值逻辑：

```c
i64 sqlite3VdbeIntValue(const Mem *pMem){
  ...
  flags = pMem->flags;
  if( flags & (MEM_Int|MEM_IntReal) ){
    return pMem->u.i;
  }else if( flags & MEM_Real ){
    return sqlite3RealToI64(pMem->u.r);
  }else if( (flags & (MEM_Str|MEM_Blob))!=0 && pMem->z!=0 ){
    return memIntValue(pMem);
  }else{
    return 0;
  }
}
```

四条分支：

1. `MEM_Int` 或 `MEM_IntReal`：直接取 `pMem->u.i`。
2. `MEM_Real`：调 `sqlite3RealToI64` 把双精度浮点转成 64 位整数（截断而非四舍五入）。
3. `MEM_Str` 或 `MEM_Blob` 且 `pMem->z!=0`：调 `memIntValue(pMem)`。`memIntValue`（`src/vdbemem.c`）调 `sqlite3Atoi64(pMem->z, &value, pMem->n, pMem->enc)` 做字符串到整数的解析。**这一分支会修改 `Mem` 的编码**——`sqlite3VdbeIntValue` 的注释明确写着「If pMem represents a string value, its encoding might be changed」。
4. 其余（NULL 等）：返回 0。

`sqlite3VdbeIntValue` 的函数体注释给出了语义摘要：整数原样返回；浮点返回整数部分；字符串或 Blob 尝试转成整数；SQL NULL 返回 0。

### `sqlite3_column_text` → `sqlite3_value_text` → `sqlite3ValueText` → `valueToText`

`sqlite3_column_text` 调 `sqlite3_value_text`，后者是：

```c
const unsigned char *sqlite3_value_text(sqlite3_value *pVal){
  return (const unsigned char *)sqlite3ValueText(pVal, SQLITE_UTF8);
}
```

`sqlite3_value_text` 固定用 `SQLITE_UTF8` 编码。`sqlite3_value_text16` 用 `SQLITE_UTF16NATIVE`，`sqlite3_value_text16be` / `sqlite3_value_text16le` 分别用 `SQLITE_UTF16BE` / `SQLITE_UTF16LE`（这三者被 `#ifndef SQLITE_OMIT_UTF16` 包住）。

`sqlite3ValueText`（`src/vdbemem.c`）是转换的分派点：

```c
const void *sqlite3ValueText(sqlite3_value* pVal, u8 enc){
  if( !pVal ) return 0;
  ...
  if( (pVal->flags&(MEM_Str|MEM_Term))==(MEM_Str|MEM_Term) && pVal->enc==enc ){
    return pVal->z;
  }
  if( pVal->flags&MEM_Null ){
    return 0;
  }
  return valueToText(pVal, enc);
}
```

三条分支：

1. **已有零结尾的文本且编码匹配**（`MEM_Str` 与 `MEM_Term` 同时置位，且 `pVal->enc==enc`）：直接返回 `pVal->z`，零拷贝。
2. **NULL 值**：返回 0（空指针）。
3. **其余**：调 `valueToText(pVal, enc)` 做转换。

`valueToText`（`src/vdbemem.c`，标注 `SQLITE_NOINLINE`）：

```c
static SQLITE_NOINLINE const void *valueToText(sqlite3_value* pVal, u8 enc){
  ...
  if( pVal->flags & (MEM_Blob|MEM_Str) ){
    if( ExpandBlob(pVal) ) return 0;
    pVal->flags |= MEM_Str;
    if( pVal->enc != (enc & ~SQLITE_UTF16_ALIGNED) ){
      sqlite3VdbeChangeEncoding(pVal, enc & ~SQLITE_UTF16_ALIGNED);
    }
    ...
    sqlite3VdbeMemNulTerminate(pVal); /* IMP: R-31275-44060 */
  }else{
    sqlite3VdbeMemStringify(pVal, enc, 0);
    ...
  }
  ...
}
```

- **Blob / Str 分支**（原本就是文本或 Blob）：先 `ExpandBlob(pVal)`——这是个宏，定义在 `src/vdbeInt.h`：`#define ExpandBlob(P) (((P)->flags&MEM_Zero)?sqlite3VdbeMemExpandBlob(P):0)`，即只有当 `MEM_Zero` 置位（表示「Blob 尾部带若干零字节」）时才真正调 `sqlite3VdbeMemExpandBlob` 把零填充尾巴展开成普通 Blob。展开失败（返回非 0）就直接返回 0。然后置上 `MEM_Str`，编码不匹配就调 `sqlite3VdbeChangeEncoding` 转码，最后 `sqlite3VdbeMemNulTerminate` 补零结尾符。
- **数值分支**（`else`，即 `MEM_Int` / `MEM_Real` / `MEM_IntReal`）：调 `sqlite3VdbeMemStringify(pVal, enc, 0)`。`sqlite3VdbeMemStringify`（`src/vdbemem.c`）先 `sqlite3VdbeMemClearAndResize(pMem, nByte)` 分配缓冲区，`nByte` 是该函数内的局部常量 **32**；然后 `vdbeMemRenderNum(nByte, pMem->z, pMem)` 渲染数字，`pMem->enc = SQLITE_UTF8`，置 `MEM_Str|MEM_Term`；`bForce` 传 0，所以 `MEM_Int` / `MEM_Real` / `MEM_IntReal` 三个标志**保留**——数值表示和文本表示共存于同一个 `Mem`。最后 `sqlite3VdbeChangeEncoding(pMem, enc)`。

`sqlite3_value_text` 这条路径的最终返回：若 `pVal->enc` 已经等于目标编码，返回 `pVal->z`；否则返回 0。

### `sqlite3_column_blob` → `sqlite3_value_blob`

`sqlite3_column_blob` 调 `sqlite3_value_blob`，后者：

```c
const void *sqlite3_value_blob(sqlite3_value *pVal){
  Mem *p = (Mem*)pVal;
  if( p->flags & (MEM_Blob|MEM_Str) ){
    if( ExpandBlob(p)!=SQLITE_OK ){
      assert( p->flags==MEM_Null && p->z==0 );
      return 0;
    }
    p->flags |= MEM_Blob;
    return p->n ? p->z : 0;
  }else{
    return sqlite3_value_text(pVal);
  }
}
```

- **Blob / Str 分支**：`ExpandBlob` 展开零填充尾巴，然后置 `MEM_Blob`。返回 `p->n ? p->z : 0`——**长度为 0 的 Blob 返回空指针**，这一点在 `src/sqlite.h.in` 的官方文档里写明：「The return value from sqlite3_column_blob() for a zero-length BLOB is a NULL pointer」。
- **其他分支（数值、NULL）**：直接 `return sqlite3_value_text(pVal)`，即**退回到取文本**（对数值就是先转成文本再当字节串用）。

注意 `sqlite3_column_blob` 的注释说明了为什么要调 `columnMallocFailure`：「Even though there is no encoding conversion, value_blob() might need to call malloc() to expand the result of a zeroblob() expression.」

### `columnMallocFailure`：把 OOM 变成语句级错误码

每个 `sqlite3_column_XXX` 在取值之后都调 `columnMallocFailure(pStmt)`。这个静态函数的函数体注释说明了它的来历：

```c
static void columnMallocFailure(sqlite3_stmt *pStmt)
{
  Vdbe *p = (Vdbe *)pStmt;
  if( p ){
    assert( p->db!=0 );
    assert( sqlite3_mutex_held(p->db->mutex) );
    p->rc = sqlite3ApiExit(p->db, p->rc);
    sqlite3_mutex_leave(p->db->mutex);
  }
}
```

要点：

- 它检查的**不是**转换本身的返回值，而是 `db->mallocFailed` 标志——SQLite 的内存分配失败是**延迟上报**的，`sqlite3ValueText` / `valueToText` / `sqlite3VdbeMemStringify` 在分配失败时把 `db->mallocFailed` 置位并返回 0，而 `sqlite3_column_XXX` 拿到的是 0（空指针）而不是错误码。
- `sqlite3ApiExit`（`src/malloc.c`）是所有 API 出口的错误闸门：`if( db->mallocFailed || rc )` 才走 `apiHandleError(db, rc)`；`apiHandleError` 在 `db->mallocFailed || rc==SQLITE_IOERR_NOMEM` 时 `sqlite3OomClear(db)`、`sqlite3Error(db, SQLITE_NOMEM)` 并返回 `SQLITE_NOMEM_BKPT`。
- 把结果写回 `p->rc`——**语句对象的错误码**。注释写明后果：「The next call to _step() (if any) will return SQLITE_ERROR and _finalize() will return NOMEM.」也就是说，OOM 不会从 `sqlite3_column_*` 直接返回给调用方（这些函数的返回类型装不下错误码），而是**寄存**在语句上，等下一次 `sqlite3_step` 或 `sqlite3_finalize` 把它带出来。
- `sqlite3_mutex_leave(p->db->mutex)`：`columnMem` 里加的锁在这里被释放。**四个 `sqlite3_column_XXX` 的锁是「借」来的**——`columnMem` 加锁，`columnMallocFailure` 解锁，两者必须配对。

### `sqlite3_column_value` 与 `sqlite3_column_bytes` 的对照

同族里另有两个函数，行为与上面四个不同源：

- `sqlite3_column_value`（`src/vdbeapi.c`）直接返回 `columnMem(pStmt, i)` 得到的 `Mem *`，并把 `MEM_Static` 标志换成 `MEM_Ephem`（源码写法：`pOut->flags &= ~MEM_Static; pOut->flags |= MEM_Ephem;`）。`src/sqlite.h.in` 的文档警告说这个对象是「unprotected sqlite3_value」，在多线程环境下只应配合 `sqlite3_bind_value()` 和 `sqlite3_result_value()` 使用。
- `sqlite3_column_bytes` / `sqlite3_column_bytes16` 调 `sqlite3_value_bytes` / `sqlite3_value_bytes16`，后者分别转调 `sqlite3ValueBytes(pVal, SQLITE_UTF8)` 与 `sqlite3ValueBytes(pVal, SQLITE_UTF16NATIVE)`。

## 关键取舍

- **转换是就地的，不是拷贝出来的**：类型转换直接改写结果行寄存器里的 `Mem`（`valueToText` 置 `MEM_Str`、`sqlite3VdbeMemStringify` 保留 `MEM_Int`/`MEM_Real` 标志）。好处是同一行的多个列可以用不同格式反复取，省掉一次复制；代价是**取值顺序会影响后续取值**——`src/sqlite.h.in` 的官方文档写明 `sqlite3_column_type()` 的返回值「only meaningful if no automatic type conversions have occurred for the value in question. After a type conversion, the result of calling sqlite3_column_type() is undefined, though harmless.」
- **同一列的多次读取必须同序**：`src/sqlite.h.in` 列出了一份「必须遵守的调用顺序」清单，例如 `sqlite3_column_text()` 后跟 `sqlite3_column_bytes()`、`sqlite3_column_blob()` 后跟 `sqlite3_column_bytes()` 是允许的；但 `sqlite3_column_bytes()` 先于 `sqlite3_column_text()` 就会拿到过时长度。文档的建议是「先调 `sqlite3_column_text()` / `sqlite3_column_blob()` / `sqlite3_column_text16()` 强制一次转换，再调 `sqlite3_column_bytes()` / `sqlite3_column_bytes16()` 取长度」。
- **零长度 Blob 返回 NULL 指针而不是非空指针**：`sqlite3_value_blob` 的 `return p->n ? p->z : 0;` 是这个约定在源码里的落点。代价是调用方无法用「指针非空」区分「零长度 Blob」与「NULL 值」——必须配合 `sqlite3_column_type()` 判断。
- **越界不崩溃但会污染连接错误码**：`columnMem` 的越界分支调 `sqlite3Error(pVm->db, SQLITE_RANGE)`，把 `SQLITE_RANGE` 写进 `db->errCode`，同时返回一个 NULL 值对象。好处是错误可观测；代价是**之后的 `sqlite3_errmsg()` 会看到这条范围错误**，即使调用方自己没在意过越界。
- **OOM 不从列读取 API 返回，而是寄存到语句上**：`columnMallocFailure` 把 `sqlite3ApiExit` 的结果写进 `p->rc`，由下一次 `sqlite3_step` / `sqlite3_finalize` 带出。好处是 `sqlite3_column_*` 的签名保持干净（`int` / 指针）；代价是**调用方必须检查 step / finalize 的返回值**，否则会静默丢数据。
- **NULL 值与「值本身是 NULL」共用一个指针表示**：`sqlite3_value_text` 对 `MEM_Null` 返回 0，`sqlite3_value_blob` 对零长度也返回 0。区分手段只有 `sqlite3_column_type()`。
- **`sqlite3_column_int` 返回 32 位**：`sqlite3_value_int` 的实现是 `return (int)sqlite3VdbeIntValue((Mem*)pVal);`，截断发生在 `sqlite3_value_int` 这一层。需要 64 位整数必须显式用 `sqlite3_column_int64`（走 `sqlite3_value_int64`，直接返回 `sqlite3VdbeIntValue` 的 64 位结果，不截断）。

## 与主核 / AI 的关系

本机制在整条链路上处于「执行结果」与「应用」之间：往上，`src/vdbe.c` 的 `OP_ResultRow` 产出 `SQLITE_ROW` 并把 `p->pResultRow` 指向结果行寄存器区；往下，四个 `sqlite3_column_XXX` 把寄存器里的 `Mem` 翻译成调用方要的字节形态。

**架构推论：** 主数据库内核（`engine/db/`）若采用 SQLite 这套「行驱动 + 按需转换」的接口形状，核心约束是：**值的存储形态与对外暴露形态分离**。`Mem` 用一组标志位（`MEM_Null` / `MEM_Str` / `MEM_Int` / `MEM_Real` / `MEM_Blob` / `MEM_IntReal`）同时承载多种表示，转换时保留原表示、只追加目标表示。这比「每列只存一种类型、按类型分派读取函数」的方案多占内存，但换来的是「同一行可以按整数、按文本、按字节串多次取」而不必重算。

**架构推论：** `columnMallocFailure` 把 OOM 寄存到语句对象而不是从列读取 API 直接抛出，这个设计对主核的启示是：**错误上报的通道要与 API 签名匹配**。`sqlite3_column_int` 的返回类型是 `int`，装不下 `SQLITE_NOMEM`；于是 SQLite 选了「语句级错误码 + 下次 step/finalize 带出」的方案。主核若提供类似的 `int` 返回型取值接口，需要同等机制，否则 OOM 会退化成「静默返回 0」。

**架构推论：** `columnMem` 的 `i<pVm->nResColumn && i>=0` 双重边界检查，加上 `sqlite3Error(db, SQLITE_RANGE)`，说明这一层把「越界」当作**可恢复的运行时错误**而不是编程错误。这与 `sqlite3_bind_*` 的处理形成对照——`sqlite3_bind_int64` 在 `i<=0 || (--i)>=p->nVar` 时同样返回 `SQLITE_RANGE`，但走的是 `sqlite3Error(p->db, SQLITE_RANGE)` 后 `sqlite3_mutex_leave` 并**直接把 `SQLITE_RANGE` 返回给调用方**，因为绑定 API 的返回类型是 `int`、装得下错误码。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/vdbeapi.c | `sqlite3_column_int` | 取整数值：`sqlite3_value_int(columnMem(pStmt,i))` 后调 `columnMallocFailure`，返回 32 位 int |
| src/vdbeapi.c | `sqlite3_column_text` | 取 UTF-8 文本：`sqlite3_value_text(columnMem(pStmt,i))` 后调 `columnMallocFailure`，返回零结尾的 `const unsigned char *` |
| src/vdbeapi.c | `sqlite3_column_type` | 取存储类型：`sqlite3_value_type(columnMem(pStmt,i))` 后调 `columnMallocFailure`，返回 `SQLITE_INTEGER`/`SQLITE_FLOAT`/`SQLITE_TEXT`/`SQLITE_BLOB`/`SQLITE_NULL` 之一 |
| src/vdbeapi.c | `sqlite3_column_blob` | 取 Blob 字节：`sqlite3_value_blob(columnMem(pStmt,i))` 后调 `columnMallocFailure`；零长度 Blob 返回空指针 |
| src/vdbeapi.c | `columnMem` | 静态函数，唯一的列定位入口：`pVm==0` 返回 `columnNullValue()`；加连接互斥锁；`pResultRow!=0 && i<nResColumn && i>=0` 时返回 `&pResultRow[i]`，否则 `sqlite3Error(db, SQLITE_RANGE)` 并返回 `columnNullValue()` |
| src/vdbeapi.c | `columnNullValue` | 静态函数，返回指向静态 `Mem` 的指针，该对象 `flags=MEM_Null`、`z=0`、`n=0`，用静态对象保证 `i64` 成员对齐 |
| src/vdbeapi.c | `columnMallocFailure` | 静态函数，把 `sqlite3ApiExit(p->db, p->rc)` 结果写回语句的 `p->rc`，并 `sqlite3_mutex_leave` 释放 `columnMem` 加的锁；OOM 经此寄存到语句上 |
| src/vdbeapi.c | `sqlite3_value_int` | `(int)sqlite3VdbeIntValue((Mem*)pVal)`，32 位截断发生在这里 |
| src/vdbeapi.c | `sqlite3_value_text` | `sqlite3ValueText(pVal, SQLITE_UTF8)` |
| src/vdbeapi.c | `sqlite3_value_type` | 查表 `aType[pVal->flags&MEM_AffMask]` 把 `Mem` 标志翻译成存储类型码；`SQLITE_DEBUG` 下另有一段独立推导的交叉断言 |
| src/vdbeapi.c | `sqlite3_value_blob` | Blob/Str 分支调 `ExpandBlob` 后置 `MEM_Blob` 并 `return p->n ? p->z : 0`；其他分支 `return sqlite3_value_text(pVal)` |
| src/vdbeapi.c | `sqlite3_column_count` | `return pVm->nResColumn;`，`columnMem` 的边界判断用的就是同一个字段 |
| src/vdbeapi.c | `sqlite3_column_value` | 直接返回 `columnMem` 得到的 `Mem *`，把 `MEM_Static` 换成 `MEM_Ephem` |
| src/vdbe.c | `OP_ResultRow` | 结果行产生点：`p->pResultRow = &aMem[pOp->p1]` 后 `rc = SQLITE_ROW; goto vdbe_return;`，`columnMem` 依赖的指针由它设置 |
| src/vdbemem.c | `sqlite3VdbeIntValue` | 四分支取整数：`MEM_Int`/`MEM_IntReal` 直接取 `u.i`；`MEM_Real` 走 `sqlite3RealToI64`；`MEM_Str`/`MEM_Blob` 走 `memIntValue`；其余返回 0 |
| src/vdbemem.c | `memIntValue` | `sqlite3Atoi64(pMem->z, &value, pMem->n, pMem->enc)`，把字符串解析成 64 位整数 |
| src/vdbemem.c | `sqlite3ValueText` | 文本分派：已是零结尾文本且编码匹配则零拷贝返回 `pVal->z`；`MEM_Null` 返回 0；否则调 `valueToText` |
| src/vdbemem.c | `valueToText` | `SQLITE_NOINLINE`；Blob/Str 分支 `ExpandBlob` → 置 `MEM_Str` → `sqlite3VdbeChangeEncoding` → `sqlite3VdbeMemNulTerminate`；数值分支调 `sqlite3VdbeMemStringify` |
| src/vdbemem.c | `sqlite3VdbeMemStringify` | 数值转文本：局部常量 `nByte = 32` 决定缓冲区大小，`sqlite3VdbeMemClearAndResize` 分配，`vdbeMemRenderNum` 渲染，置 `MEM_Str|MEM_Term`；`bForce` 传 0 时保留 `MEM_Int`/`MEM_Real`/`MEM_IntReal` 标志 |
| src/vdbemem.c | `sqlite3VdbeMemExpandBlob` | 把带 `MEM_Zero` 零填充尾巴的 Blob 展开成动态分配的普通 Blob，分配失败返回 `SQLITE_NOMEM` |
| src/vdbeInt.h | `MEM_AffMask` | 类型位掩码，值 `0x003f`，`sqlite3_value_type` 查表的索引来源 |
| src/vdbeInt.h | `MEM_Null` / `MEM_Str` / `MEM_Int` / `MEM_Real` / `MEM_Blob` / `MEM_IntReal` | `Mem` 的六个类型标志位，值依次为 `0x0001` / `0x0002` / `0x0004` / `0x0008` / `0x0010` / `0x0020` |
| src/vdbeInt.h | `MEM_Zero` | `0x0400`，表示 Blob 尾部附带零填充字节数，是 `ExpandBlob` 展开的触发条件 |
| src/vdbeInt.h | `MEM_Term` | `0x0200`，表示 `Mem.z` 已零结尾，是 `sqlite3ValueText` 零拷贝分支的前提 |
| src/vdbeInt.h | `MEM_Ephem` | `0x4000`，`sqlite3_column_value` 把 `MEM_Static` 换成它的目标标志 |
| src/vdbeInt.h | `ExpandBlob` | 宏：`(((P)->flags&MEM_Zero)?sqlite3VdbeMemExpandBlob(P):0)` |
| src/util.c | `sqlite3Error` | `db->errCode = err_code` 并调 `sqlite3ErrorFinish` 清旧消息；`columnMem` 越界时用它置 `SQLITE_RANGE` |
| src/malloc.c | `sqlite3ApiExit` | 统一错误闸门：`db->mallocFailed || rc` 非零时转 `apiHandleError`；`columnMallocFailure` 用它把 OOM 变成 `SQLITE_NOMEM_BKPT` |

## 参考

- [Result Values From A Query（sqlite3_column_* 官方文档）](https://sqlite.org/c3ref/column_blob.html)
- [sqlite3_column_text（官方文档）](https://sqlite.org/c3ref/column_text.html)
- [sqlite3_column_int（官方文档）](https://sqlite.org/c3ref/column_int.html)
- [Data Type Conversions In SQLite（官方文档）](https://sqlite.org/datatype3.html)
- [The SQLite Bytecode Engine（VDBE 官方文档）](https://sqlite.org/opcode.html)
- [Architecture of SQLite（SQLite 官方架构文档）](https://sqlite.org/arch.html)
- [SQLite 源码浏览：src/vdbeapi.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/vdbeapi.c)
- [sqlite/sqlite 仓库中的 src/vdbeapi.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/vdbeapi.c)
- [sqlite/sqlite 仓库中的 src/vdbemem.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/vdbemem.c)
