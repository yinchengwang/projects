---
project: sqlite
feature: load-extension
page_type: feature
schema_version: 1
refs:
  - path: src/loadext.c
    symbols: [sqlite3LoadExtension, sqlite3_auto_extension, sqlite3_enable_load_extension, sqlite3CloseExtensions]
---

# 可加载扩展

## 一句话职责

把一个动态库（含扩展入口函数）加载进当前连接：负责动态库的打开、入口点符号的查找与调用、扩展句柄在连接生命周期内的登记与释放，以及静态链接扩展的自动注册链；不负责用户自定义函数本身（l02），也不负责 VFS 的注册（j01）。

## 机制是什么

SQLite 的扩展分两种进入方式：

1. **动态加载**：`sqlite3_load_extension()` 打开一个共享库，找出它的入口函数并调用它。扩展拿到连接句柄后，用同一份 API 函数表注册自己的函数、虚表或排序规则。
2. **静态链接 + 自动注册**：`sqlite3_auto_extension()` 把一个静态链接的入口函数登记进一张全局链表，每个新连接建立时该链表上的入口函数被逐个调用。

两条路径的共同点是**入口函数签名统一**。`src/sqlite3ext.h` 里 `sqlite3_loadext_entry` 定义了它：

```
typedef int (*sqlite3_loadext_entry)(
  sqlite3 *db,                       /* Handle to the database. */
  char **pzErrMsg,                   /* Used to set error string on failure. */
  const sqlite3_api_routines *pThunk /* Extension API function pointers. */
);
```

`src/loadext.c` 把整份 SQLite 公开 API 打包成一个 `static const sqlite3_api_routines sqlite3Apis` 结构体（`sqlite3Apis` 是 `static const`，只读、进程内唯一），把这个结构体的地址作为 `pThunk` 传给入口函数。扩展侧的 API 宏（`src/sqlite3ext.h` 中 `#define sqlite3_load_extension sqlite3_api->load_extension` 之类）就是通过 `pThunk` 间接回到核心的。

**安全开关默认关闭。** `src/loadext.c` 的 `sqlite3LoadExtension` 函数体第一道实质判定就是：

```
if( (db->flags & SQLITE_LoadExtension)==0 ){
  if( pzErrMsg ){
    *pzErrMsg = sqlite3_mprintf("not authorized");
  }
  return SQLITE_ERROR;
}
```

`SQLITE_LoadExtension` 的值是 `0x00010000`（定义在 `src/sqliteInt.h`），控制的是 **C API** `sqlite3_load_extension()`；`SQLITE_LoadExtFunc` 的值是 `0x00020000`（同一处），控制的是 **SQL 函数** `load_extension()`。`src/func.c` 的 `loadExt` 函数体里单独判后者：

```
if( (db->flags & SQLITE_LoadExtFunc)==0 ){
  sqlite3_result_error(context, "not authorized", -1);
  return;
}
```

`sqlite3_enable_load_extension()` 一次把两个位都置上或都清掉（`db->flags |= SQLITE_LoadExtension|SQLITE_LoadExtFunc`），而 `sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, ...)` 只动 `SQLITE_LoadExtension` 一个位——`src/main.c` 的 `sqlite3_db_config` 函数体里，`aFlagOp[]` 表的对应项是 `{ SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, SQLITE_LoadExtension }`，开关值按 `onoff>0` 置位、`onoff==0` 清位。

**边界**（与页面清单给出的 boundary 一致）：本页负责动态库的加载与入口点调用，以及自动扩展的注册链；不负责用户自定义函数（l02），不负责 VFS 注册（j01）。

## 怎么实现的

### 加载一条动态扩展：sqlite3LoadExtension

`sqlite3_load_extension()` 是对外包装（`src/loadext.c`）：先 `sqlite3_mutex_enter(db->mutex)`，调 `sqlite3LoadExtension`，再 `rc = sqlite3ApiExit(db, rc)`，最后 `sqlite3_mutex_leave(db->mutex)`。真正的逻辑全在静态函数 `sqlite3LoadExtension` 里。它的执行顺序是：

**第 0 步：参数与开关。** `nMsg = strlen(zFile)`；`if( pzErrMsg ) *pzErrMsg = 0;`；然后是上面那条 `SQLITE_LoadExtension` 未置位即 `"not authorized"` 并 `return SQLITE_ERROR`。`zEntry = zProc ? zProc : "sqlite3_extension_init"`——默认入口点名就写在源码里。

**第 1 步：路径长度与空文件名拦截。**

```
if( nMsg>SQLITE_MAX_PATHLEN ) goto extension_not_found;
if( nMsg==0 ) goto extension_not_found;
```

`SQLITE_MAX_PATHLEN` 定义在 `src/os.h`：`#ifndef SQLITE_MAX_PATHLEN / # define SQLITE_MAX_PATHLEN FILENAME_MAX`，即默认取 C 标准库的 `FILENAME_MAX`。源码注释（tag-20210611-1）写明理由：有些 `dlopen()` 实现在文件名过长时会 segfault，而多数文件系统路径上限是 4K；2023-03-25 的补充注释说要多留 6 字节给文件名后缀。

`nMsg==0` 的拦截，源码注释写明是「不允许 `sqlite3_load_extension()` 通过传入空文件名链接到运行中应用程序的副本」。

**第 2 步：打开动态库并补后缀重试。** 先 `handle = sqlite3OsDlOpen(pVfs, zFile)`。若失败，在 `SQLITE_OS_UNIX || SQLITE_OS_WIN` 下按 `azEndings[]` 数组重试：

```
static const char *azEndings[] = {
#if SQLITE_OS_WIN
     "dll"
#elif defined(__APPLE__)
     "dylib"
#else
     "so"
#endif
};
```

数组只有 1 个元素——按平台编译期三选一，不是运行时列表。每一轮拼 `zAltFile = sqlite3_mprintf("%s.%s", zFile, azEndings[ii])`，且只在 `nMsg+strlen(azEndings[ii])+1<=SQLITE_MAX_PATHLEN` 时才真正调用 `sqlite3OsDlOpen`。`zAltFile` 分配失败直接 `return SQLITE_NOMEM_BKPT`。重试完仍为 0 则 `goto extension_not_found`。

`sqlite3OsDlOpen` 在 `src/os.c` 里只是转发：`assert( strlen(zPath)<=SQLITE_MAX_PATHLEN )` 后调 `pVfs->xDlOpen(pVfs, zPath)`。Unix 实现在 `src/os_unix.c` 的 `unixDlOpen`：`return dlopen(zFilename, RTLD_NOW | RTLD_GLOBAL);`——`RTLD_NOW` 表示符号在加载时就解析，`RTLD_GLOBAL` 表示导出给后续加载的库。

**第 3 步：查找入口点。** `xInit = (sqlite3_loadext_entry)sqlite3OsDlSym(pVfs, handle, zEntry);`

若**没有显式指定** `zProc`（即 `xInit==0 && zProc==0`），源码进入备用命名推导：构造 `"sqlite3_"` 前缀 + 库文件名主干 + `"_init"` 后缀。推导规则写在函数注释里，函数体逐条实现：

- 先 `memcpy(zAltEntry, "sqlite3_", 8)`。
- 从文件名末尾倒着找目录分隔符（`DirSep(X)` 宏：Windows 下 `'/'` 或 `'\\'`，其他平台只有 `'/'`），定位基名起点。
- `if( sqlite3_strnicmp(zFile+iFile, "lib", 3)==0 ) iFile += 3;`——跳过开头的 `lib`。
- 逐字符取到第一个 `'.'` 为止；`if( sqlite3Isalpha(c) || (cnt && sqlite3Isdigit(c)) )` 才收进名字，并经 `sqlite3UpperToLower[(unsigned)c]` 转小写。`cnt` 在第二轮为 1，所以第二轮数字也收进来。
- `memcpy(zAltEntry+iEntry, "_init", 6)` 拼后缀。
- `do{...}while( xInit==0 && (++cnt)<2 )`——**最多试 2 轮**：第 1 轮只收字母，第 2 轮收字母加数字。

源码注释给的两个例子：`/usr/local/lib/libExample5.4.3.so` 得到 `sqlite3_example_init`（第 1 轮）与 `sqlite3_example5_init`（第 2 轮）；`C:/lib/mathfuncs.dll` 得到 `sqlite3_mathfuncs_init`。

缓冲区分配是 `zAltEntry = sqlite3_malloc64(ncFile+30)`，其中 `ncFile = sqlite3Strlen30(zFile)`；`sqlite3Strlen30` 在 `src/util.c` 里返回 `0x3fffffff & (int)strlen(z)`，即只保留低 30 位。分配失败时先 `sqlite3OsDlClose(pVfs, handle)` 再 `return SQLITE_NOMEM_BKPT`。

**第 4 步：入口点缺失的报错。** `xInit==0` 时，若 `pzErrMsg` 非空则分配 `nMsg += strlen(zEntry) + 300` 字节，用 `sqlite3_snprintf` 写入 `"no entry point [%s] in shared library [%s]"`，再用 `sqlite3OsDlError(pVfs, nMsg-1, zErrmsg)` 追加平台动态链接器的错误文本；然后 `sqlite3OsDlClose` 释放句柄、`sqlite3_free(zAltEntry)`、`return SQLITE_ERROR`。Unix 上 `unixDlError` 取 `dlerror()` 的文本。

**第 5 步：调用入口函数。**

```
rc = xInit(db, &zErrmsg, &sqlite3Apis);
if( rc ){
  if( rc==SQLITE_OK_LOAD_PERMANENTLY ) return SQLITE_OK;
  if( pzErrMsg ){
    *pzErrMsg = sqlite3_mprintf("error during initialization: %s", zErrmsg);
  }
  sqlite3_free(zErrmsg);
  sqlite3OsDlClose(pVfs, handle);
  return SQLITE_ERROR;
}
```

`SQLITE_OK_LOAD_PERMANENTLY` 定义在 `src/sqlite.h.in`：`#define SQLITE_OK_LOAD_PERMANENTLY (SQLITE_OK | (1<<8))`，即 256。扩展返回这个值表示「初始化成功，且希望这个动态库在连接关闭前一直驻留」——`src/loadext.c` 把它直接折成 `SQLITE_OK` 返回，**并且不走后面的句柄登记**，因为该扩展自己管理驻留。

**第 6 步：登记句柄。** 用 `sqlite3DbMallocZero(db, sizeof(handle)*(db->nExtension+1))` 分配新数组，把旧的 `db->aExtension` 内容 `memcpy` 过去，`sqlite3DbFree(db, db->aExtension)` 释放旧数组，然后 `db->aExtension = aHandle`，最后 `db->aExtension[db->nExtension++] = handle;`。分配失败 `return SQLITE_NOMEM_BKPT`。注意：数组扩容失败时句柄没有被登记，但 `xInit` 已经执行过。

### 关闭时释放：sqlite3CloseExtensions

```
void sqlite3CloseExtensions(sqlite3 *db){
  int i;
  assert( sqlite3_mutex_held(db->mutex) );
  for(i=0; i<db->nExtension; i++){
    sqlite3OsDlClose(db->pVfs, db->aExtension[i]);
  }
  sqlite3DbFree(db, db->aExtension);
}
```

调用方是 `src/main.c` 的连接关闭路径（`sqlite3Close` 函数体内，`sqlite3Error(db, SQLITE_OK)` 与 `sqlite3ValueFree(db->pErr)` 之后）。`src/sqliteInt.h` 里 `sqlite3CloseExtensions` 有条件编译：定义 `SQLITE_OMIT_LOAD_EXTENSION` 时整个声明被 `#define sqlite3CloseExtensions(X)` 消掉。

### 开关：sqlite3_enable_load_extension

```
int sqlite3_enable_load_extension(sqlite3 *db, int onoff){
#ifdef SQLITE_ENABLE_API_ARMOR
  if( !sqlite3SafetyCheckOk(db) ) return SQLITE_MISUSE_BKPT;
#endif
  sqlite3_mutex_enter(db->mutex);
  if( onoff ){
    db->flags |= SQLITE_LoadExtension|SQLITE_LoadExtFunc;
  }else{
    db->flags &= ~(u64)(SQLITE_LoadExtension|SQLITE_LoadExtFunc);
  }
  sqlite3_mutex_leave(db->mutex);
  return SQLITE_OK;
}
```

它总是返回 `SQLITE_OK`。`SQLITE_ENABLE_API_ARMOR` 下多一道 `sqlite3SafetyCheckOk(db)` 检查，失败返回 `SQLITE_MISUSE_BKPT`。

### 自动扩展链

自动扩展表在 `src/loadext.c` 里定义为进程级状态：

```
typedef struct sqlite3AutoExtList sqlite3AutoExtList;
static SQLITE_WSD struct sqlite3AutoExtList {
  u32 nExt;              /* Number of entries in aExt[] */
  void (**aExt)(void);   /* Pointers to the extension init functions */
} sqlite3Autoext = { 0, 0 };
```

`SQLITE_WSD` 是「可写静态数据（writeable static data）」属性：当目标平台不支持可写静态数据时，`wsdAutoextInit` / `wsdAutoext` 宏改为在运行时通过 `GLOBAL(sqlite3AutoExtList,sqlite3Autoext)` 定位状态向量（`SQLITE_OMIT_WSD` 分支）。

**注册：sqlite3_auto_extension。** 函数体流程：

1. `SQLITE_ENABLE_API_ARMOR` 下 `if( xInit==0 ) return SQLITE_MISUSE_BKPT;`。
2. 未定义 `SQLITE_OMIT_AUTOINIT` 时先 `rc = sqlite3_initialize()`，失败直接返回该错误码。
3. `SQLITE_THREADSAFE` 下取 `mutex = sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MAIN)`——`SQLITE_MUTEX_STATIC_MAIN` 的值是 2（`src/sqlite.h.in`）。源码注释写明这张表跨线程共享，访问时必须持有这个静态互斥量。
4. 在 `wsdAutoext.aExt[]` 里线性查找 `xInit`；**已存在则不重复登记**（`if( i==wsdAutoext.nExt )` 才扩容并追加）。这一步是去重，不是"替换"。
5. 扩容用 `sqlite3_realloc64(wsdAutoext.aExt, nByte)`，`nByte = (wsdAutoext.nExt+1)*sizeof(wsdAutoext.aExt[0])`；失败 `rc = SQLITE_NOMEM_BKPT`。成功后 `wsdAutoext.aExt[wsdAutoext.nExt] = xInit; wsdAutoext.nExt++;`。
6. 结尾 `assert( (rc&0xff)==rc );`——断言返回码是单字节错误码。

**取消：sqlite3_cancel_auto_extension。** 从表尾向前线性查找 `xInit`，找到则把末位元素搬到该位置再 `wsdAutoext.nExt--`（`wsdAutoext.aExt[i] = wsdAutoext.aExt[wsdAutoext.nExt]`），并 `break`（只删一个）。返回 1 表示找到并删除，返回 0 表示不在表里。`SQLITE_ENABLE_API_ARMOR` 下 `xInit==0` 直接返回 0。

**清空：sqlite3_reset_auto_extension。** 持 `SQLITE_MUTEX_STATIC_MAIN` 互斥量，`sqlite3_free(wsdAutoext.aExt); wsdAutoext.aExt = 0; wsdAutoext.nExt = 0;`。`SQLITE_OMIT_AUTOINIT` 未定义时，先要求 `sqlite3_initialize()==SQLITE_OK` 才执行清空。

**执行：sqlite3AutoLoadExtensions。** 这是本页 `refs` 里没有列出但属于同一机制的函数：

```
void sqlite3AutoLoadExtensions(sqlite3 *db){
  ...
  if( wsdAutoext.nExt==0 ){
    /* Common case: early out without every having to acquire a mutex */
    return;
  }
  for(i=0; go; i++){
    ...
    sqlite3_mutex_enter(mutex);
    if( i>=wsdAutoext.nExt ){
      xInit = 0;
      go = 0;
    }else{
      xInit = (sqlite3_loadext_entry)wsdAutoext.aExt[i];
    }
    sqlite3_mutex_leave(mutex);
    zErrmsg = 0;
    if( xInit && (rc = xInit(db, &zErrmsg, pThunk))!=0 ){
      sqlite3ErrorWithMsg(db, rc,
            "automatic extension loading failed: %s", zErrmsg);
      go = 0;
    }
    sqlite3_free(zErrmsg);
  }
}
```

要点：

- 表为空时**直接返回，不取互斥量**——源码注释把这条标为 "Common case"。
- 逐个取入口函数时**每次都单独进出互斥量**，取完立刻放锁，然后才调用 `xInit`。
- `pThunk` 的取值受编译选项影响：`SQLITE_OMIT_LOAD_EXTENSION` 定义时 `const sqlite3_api_routines *pThunk = 0;`，否则 `= &sqlite3Apis`。
- 任一扩展返回非 0，就 `sqlite3ErrorWithMsg(db, rc, "automatic extension loading failed: %s", zErrmsg)` 把错误挂到连接上，并置 `go = 0` **停止继续加载后面的扩展**。
- 静态注册的入口函数原型是 `void (*)(void)`，被强转成 `sqlite3_loadext_entry`（即 `int (*)(sqlite3*, char**, const sqlite3_api_routines*)`）来调用。

调用方是 `src/main.c` 的连接打开路径（`sqlite3Open`/`openDatabase` 内的初始化段）：在 `sqlite3BuiltinExtensions[]` 逐个调完之后，

```
if( rc==SQLITE_OK ){
  sqlite3AutoLoadExtensions(db);
  rc = sqlite3_errcode(db);
  if( rc!=SQLITE_OK ){
    goto opendb_out;
  }
}
```

即**自动扩展加载失败会导致整个连接打开失败**，通过 `goto opendb_out` 回滚。

## 关键取舍

**默认关闭 + 双开关分离。** 源码注释（Ticket #1863）写明：为了不给那些重新链接到新版 SQLite 的老应用制造安全问题，`load_extension` 的执行能力默认关闭，必须显式调用 `sqlite3_enable_load_extension(db)` 或 `sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 1, 0)` 打开。`src/sqlite.h.in` 的 `sqlite3_load_extension` 文档进一步给出建议：**优先用 `SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION`**，因为它只开 C API，SQL 函数 `load_extension()` 保持关闭，可以挡住 SQL 注入拿到扩展加载能力的路径。这是本机制里唯一被官方文档点名为"安全警告"的设计选择。

**API 分发走只读结构体指针，而非符号重定向。** `sqlite3Apis` 是 `static const sqlite3_api_routines`。它的注释给出了扩展侧的兼容契约：新增 API **只能追加到这个结构体末尾**以保持向后兼容；使用较新 API 的扩展应先调 `sqlite3_libversion_number()` 确认版本，并检查函数指针非空再调用。这比让扩展直接链接符号的做法多了一层间接，但换来了 ABI 稳定与运行时裁剪的能力——被编译选项关掉的 API 在 `sqlite3Apis` 里填的是 `0`（例如 `SQLITE_OMIT_VIRTUALTABLE` 时 `sqlite3_create_module` 填 0，`SQLITE_MUTEX_OMIT` 时 5 个 mutex 函数填 0，`SQLITE_OMIT_WAL` 时 3 个 wal 函数填 0）。

**入口点命名推导是"兼容老扩展"的兜底，不是推荐做法。** 函数注释明确写着默认入口名 `sqlite3_extension_init` 的使用是 "recommended"；推导逻辑只在**调用方没有显式指定 `zProc`** 时才触发。推导最多试 2 轮（第 1 轮纯字母、第 2 轮字母+数字），因为文件名里的版本号（`libExample5.4.3.so`）不该进符号名。

**`SQLITE_OK_LOAD_PERMANENTLY` 让扩展自己管驻留。** 返回 256 的扩展不被登记进 `db->aExtension`，也就不会被 `sqlite3CloseExtensions` 关闭——它必须自己负责卸载。代价是核心不再追踪这个句柄。

**自动扩展失败即连接打开失败。** `sqlite3AutoLoadExtensions` 把错误写到连接上并停止加载，`src/main.c` 随即 `rc = sqlite3_errcode(db)` 并 `goto opendb_out`。这是"要么全装上、要么开不了连接"的严格语义，代价是一个坏扩展能阻断所有新连接。

**互斥量的粒度分层。** 动态库的加载/卸载走的是**每连接**互斥量 `db->mutex`（`sqlite3_load_extension` / `sqlite3_enable_load_extension` / `sqlite3CloseExtensions` 三者都断言或进入 `db->mutex`）；自动扩展表是**进程级**共享状态，用静态互斥量 `SQLITE_MUTEX_STATIC_MAIN`（值为 2）保护，且取锁粒度被压到最小——`sqlite3AutoLoadExtensions` 每取一个入口函数就放一次锁，`sqlite3_auto_extension` / `sqlite3_cancel_auto_extension` / `sqlite3_reset_auto_extension` 也都只在真正改表时持锁。

**编译期裁剪的层次。** 整个文件被 `#ifndef SQLITE_OMIT_LOAD_EXTENSION` 包住；`src/os_unix.c` 在该宏定义时把 `unixDlOpen` / `unixDlError` / `unixDlSym` / `unixDlClose` 全部 `#define` 成 0，即 VFS 的 `xDlOpen` 等槽位为 NULL。`src/sqliteInt.h` 里 `sqlite3CloseExtensions` 也在同一宏下被消掉。

## 源码落点

| 路径 | 符号 | 作用 |
| --- | --- | --- |
| src/loadext.c | sqlite3Apis | `static const sqlite3_api_routines`，把全部 SQLite 公开 API 打包成只读结构体，作为 `pThunk` 传给扩展入口函数；被编译选项关掉的 API 在此填 0。 |
| src/loadext.c | sqlite3LoadExtension | 动态扩展加载的主体：查 `SQLITE_LoadExtension` 开关、按 `SQLITE_MAX_PATHLEN` 拦截超长与空文件名、按平台后缀（`dll`/`dylib`/`so`）重试 `sqlite3OsDlOpen`、查找 `sqlite3_extension_init` 或推导出的 `sqlite3_X_init`、调 `xInit`、把句柄追加进 `db->aExtension`。 |
| src/loadext.c | sqlite3_load_extension | 对外 C API 包装：进入 `db->mutex`，调 `sqlite3LoadExtension`，`sqlite3ApiExit` 归一化返回码，释放互斥量。 |
| src/loadext.c | sqlite3_enable_load_extension | 开关：`onoff` 非 0 时置 `SQLITE_LoadExtension|SQLITE_LoadExtFunc`，否则同时清掉；`SQLITE_ENABLE_API_ARMOR` 下先过 `sqlite3SafetyCheckOk`；恒返回 `SQLITE_OK`。 |
| src/loadext.c | sqlite3CloseExtensions | 连接关闭时逐个 `sqlite3OsDlClose` 掉 `db->aExtension[]` 里的句柄，并 `sqlite3DbFree(db, db->aExtension)`；断言持有 `db->mutex`。 |
| src/loadext.c | sqlite3_auto_extension | 把静态链接的扩展入口函数登记进全局自动扩展表：先 `sqlite3_initialize()`，持 `SQLITE_MUTEX_STATIC_MAIN` 互斥量，线性查重后用 `sqlite3_realloc64` 扩容并追加。 |
| src/loadext.c | sqlite3_cancel_auto_extension | 从自动扩展表尾向前线性查找并删除指定入口函数（末位元素搬到该位再缩短数组），找到返回 1，否则返回 0。 |
| src/loadext.c | sqlite3_reset_auto_extension | 清空自动扩展表：`sqlite3_free` 释放数组，置 `aExt=0`、`nExt=0`；需持 `SQLITE_MUTEX_STATIC_MAIN` 互斥量。 |
| src/loadext.c | sqlite3AutoLoadExtensions | 自动扩展的实际执行：表为空时直接返回不取锁；否则逐个取入口函数（每次单独进出互斥量）并调用；任一扩展返回非 0 就 `sqlite3ErrorWithMsg` 报 `"automatic extension loading failed: %s"` 并停止后续加载。 |
| src/loadext.c | sqlite3Autoext / sqlite3AutoExtList | 进程级自动扩展表：`u32 nExt` 计条目数、`void (**aExt)(void)` 存入口函数指针；`SQLITE_WSD` + `wsdAutoext` 宏在不支持可写静态数据的平台上改为运行时定位。 |
| src/loadext.c | DirSep | 目录分隔符判定宏：Windows 下 `'/'` 或 `'\\'`，其他平台只有 `'/'`；用于从文件名里定位基名起点。 |
| src/loadext.c | azEndings | 动态库后缀重试表：按平台编译期三选一（Windows `dll`、Apple `dylib`、其余 `so`），数组长度 1。 |
| src/sqlite3ext.h | sqlite3_loadext_entry | 扩展入口函数原型 `int (*)(sqlite3*, char**, const sqlite3_api_routines*)`；`src/loadext.c` 注释指出该签名也定义在 `loadext.c` 里。 |
| src/sqlite3ext.h | sqlite3_api_routines | 扩展侧的 API 分发表结构；`#define sqlite3_load_extension sqlite3_api->load_extension` 等宏把扩展里的 API 调用重定向到 `pThunk` 指向的表。 |
| src/sqliteInt.h | SQLITE_LoadExtension / SQLITE_LoadExtFunc | 连接级开关位，值分别为 `0x00010000` 与 `0x00020000`；前者管 C API，后者管 SQL 函数 `load_extension()`。 |
| src/sqliteInt.h | sqlite3CloseExtensions | 条件声明：定义 `SQLITE_OMIT_LOAD_EXTENSION` 时被 `#define sqlite3CloseExtensions(X)` 消掉。 |
| src/sqliteInt.h | sqlite3AutoLoadExtensions | `sqlite3AutoLoadExtensions(sqlite3*)` 的内部声明；由 `src/main.c` 在连接打开路径调用。 |
| src/os.h | SQLITE_MAX_PATHLEN | 路径长度上限，默认 `# define SQLITE_MAX_PATHLEN FILENAME_MAX`；`sqlite3LoadExtension` 用它拦截超长文件名并约束后缀重试。 |
| src/os.c | sqlite3OsDlOpen / sqlite3OsDlSym / sqlite3OsDlClose / sqlite3OsDlError | VFS 层动态库接口的转发层：`sqlite3OsDlOpen` 断言 `strlen(zPath)<=SQLITE_MAX_PATHLEN` 后调 `pVfs->xDlOpen`。 |
| src/os_unix.c | unixDlOpen / unixDlSym / unixDlClose / unixDlError | Unix 实现：`dlopen(zFilename, RTLD_NOW | RTLD_GLOBAL)`、`dlsym`、`dlclose`、`dlerror`；`SQLITE_OMIT_LOAD_EXTENSION` 下四个函数被 `#define` 成 0。 |
| src/func.c | loadExt | SQL 函数 `load_extension()` 的实现：`SQLITE_LoadExtFunc` 未置位时报 `"not authorized"`；参数 2 个时取第二个为入口点名，否则传 0 走默认名；调 `sqlite3_load_extension` 失败时 `sqlite3_result_error` 并 `sqlite3_free` 错误串。 |
| src/main.c | sqlite3AutoLoadExtensions 调用点 | 连接打开路径：内置扩展 `sqlite3BuiltinExtensions[]` 加载完毕后调 `sqlite3AutoLoadExtensions(db)`，再 `rc = sqlite3_errcode(db)`，非 `SQLITE_OK` 则 `goto opendb_out` 回滚。 |
| src/main.c | sqlite3CloseExtensions 调用点 | 连接关闭路径：`sqlite3Error(db, SQLITE_OK)` 与 `sqlite3ValueFree(db->pErr)` 之后调 `sqlite3CloseExtensions(db)`。 |
| src/main.c | sqlite3_db_config | `aFlagOp[]` 表中 `{ SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, SQLITE_LoadExtension }` 一项只翻转 `SQLITE_LoadExtension` 位，`onoff>0` 置位、`onoff==0` 清位，标志变化时 `sqlite3ExpirePreparedStatements(db, 0)`。 |
| src/sqlite.h.in | SQLITE_OK_LOAD_PERMANENTLY | `#define SQLITE_OK_LOAD_PERMANENTLY (SQLITE_OK | (1<<8))`，值 256；扩展返回它表示初始化成功且要自行保持驻留，`sqlite3LoadExtension` 折成 `SQLITE_OK` 返回且不登记句柄。 |
| src/sqlite.h.in | SQLITE_MUTEX_STATIC_MAIN | 静态互斥量编号，值 2；自动扩展表用它保护跨线程共享状态。 |
| src/sqlite.h.in | sqlite3_load_extension / sqlite3_enable_load_extension 文档 | 官方文档正文：说明后缀重试、入口点推导规则（`sqlite3_extension_init` 优先，其次 `sqlite3_X_init`）、默认关闭、以及优先用 `SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION` 的安全警告。 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层里没有"动态库加载"这一层，它的 `engine/db/` 全部围绕单个 storage 实例展开。若主核要支持插件化，`src/loadext.c` 有三处设计值得抄：一是**能力开关与注册链分离**——`sqlite3_enable_load_extension` 一次性管两件事（C API + SQL 函数），而 `sqlite3_db_config` 只管一件，这个「一粗一细」的组合让主核可以只放开受信任的注册路径、继续堵住 SQL 层的旁路；二是**API 通过只读结构体分发表发而非直接链接**——`sqlite3Apis` 尾部追加保证 ABI 稳定，被编译选项关掉的槽位填 0 让调用方能检查指针是否为空，这套「弱化链接」的做法比直接暴露符号更容易在多内核共存时做隔离；三是**失败即中止**的自动扩展语义——`sqlite3AutoLoadExtensions` 遇错即停并把错误写进连接，主核若照抄要注意「坏插件拖垮全部新连接」这个代价必须由上层显式接受。

**架构推论：** 本页是 AI 检索 SQLite 扩展加载机制时的落点。全部动态加载逻辑集中在 `src/loadext.c`：`sqlite3LoadExtension`（加载主体）、`sqlite3_load_extension`（对外包装）、`sqlite3_enable_load_extension`（开关）、`sqlite3CloseExtensions`（句柄释放）、`sqlite3_auto_extension` / `sqlite3_cancel_auto_extension` / `sqlite3_reset_auto_extension` / `sqlite3AutoLoadExtensions`（自动注册链）。凡是"为什么 load_extension 报 not authorized"、"sqlite3_load_extension 报的 'no entry point' 到底在找什么符号"、"扩展返回什么值表示我不想被自动卸载"、"SQLITE_MAX_PATHLEN 从哪来"、"自动扩展失败会怎样影响建连接"这类问题，检索落点都应指向 `src/loadext.c`；而 `src/func.c` 的 `loadExt` 只管 SQL 侧开关，`src/os_unix.c` / `src/os_win.c` 只管平台动态链接器调用，两者都不是本机制的语义主体。

## 参考

- SQLite 官方文档《Run-Time Loadable Extensions》：<https://sqlite.org/loadext.html>
- SQLite 官方文档《sqlite3_load_extension》：<https://sqlite.org/c3ref/load_extension.html>
- SQLite 官方文档《sqlite3_enable_load_extension》：<https://sqlite.org/c3ref/enable_load_extension.html>
- SQLite 官方文档《sqlite3_auto_extension》与《sqlite3_reset_auto_extension》：<https://sqlite.org/c3ref/auto_extension.html>
- SQLite 官方文档《The load_extension() SQL Function》：<https://sqlite.org/lang_corefunc.html#load_extension>
- SQLite 官方 Fossil 源码 `src/loadext.c`（trunk，扩展加载与自动扩展链的全部实现）：<https://sqlite.org/src/file?name=src/loadext.c&ci=trunk>
- SQLite 官方 Fossil 源码 `src/sqlite3ext.h`（trunk，`sqlite3_loadext_entry` 入口签名与 `sqlite3_api_routines` 分发表）：<https://sqlite.org/src/file?name=src/sqlite3ext.h&ci=trunk>
- GitHub 源码镜像 `src/loadext.c`：<https://github.com/sqlite/sqlite/blob/master/src/loadext.c>
- GitHub 源码镜像 `src/sqlite3ext.h`：<https://github.com/sqlite/sqlite/blob/master/src/sqlite3ext.h>
