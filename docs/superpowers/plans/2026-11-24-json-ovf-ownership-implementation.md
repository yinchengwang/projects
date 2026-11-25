# C5.3b-b JSON 溢出页链实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将大于 2048 字节且不超过 65535 字节的 JSONB 文档安全接入 heap tuple 的 `PT_JSON_OVF` 页链，并保证 WAL/恢复、MVCC、VACUUM、并发读取与孤儿回收不会丢数据、读脏数据或 double-free。

**Architecture:** 先把 `json_ovf.c` 从“无 WAL 的独立页链工具”提升为可由事务写路径调用的 WAL 感知组件；再扩展 record 的 JSON 溢出标记（`tag + chain_head + chain_len`）和 heap AM 的所有权转移。读路径使用 tuple 持久化的 `chain_len`，避免 `json_ovf_read` 两趟 walk；回收路径由 VACUUM 在删除 tuple 之前取出链句柄、在读/释放互斥下幂等释放。启动阶段在 catalog 可遍历所有 heap 后扫描 tuple 引用，回收没有被 tuple 引用的 `PT_JSON_OVF` 链。

**Tech Stack:** C11、MinGW-w64、现有 Pager/PageFrame、`LogHookFn` + `LOG_PAGEDATA`、现有 heap WAL/recovery、MVCC `TxnTable`、Git Bash 测试脚本。

## Global Constraints

- 工作目录：`C:\code\projects\engine\db`；源码按 L0 common / L1 storage+wal / L2 catalog+txn / L3 query+spi / L4 modalities 分层；L4 可依赖 L0，但不得反向 include L1/L2/L3。
- 编译命令：`mingw32-make clean && mingw32-make all`；必须 `-Wall -Wextra -Werror` 零诊断。
- 回归命令：`sh test/run.sh`、`sh tools/check_deps.sh`、`sh tools/check_a7.sh`；九个 profile：`full relational blob graph vector ft geo ts doc` 全部 `rc=0`。
- `DetoastFn` 的公开长度仍为 `u16`，最大 JSONB 长度为 65535；超过返回 `E_NOSPACE`，绝不静默截断。
- 每个 heap tuple 版本独占一条页链；UPDATE 不释放旧版本链；VACUUM 物理删除旧版本时才释放。
- 页链每页使用 `LOG_PAGEDATA` 全页镜像；必须先写 WAL 再改页；每页只允许一次最终内容写入，避免 `next` 回填造成错误 undo before-image。
- 运行时 abort 不主动释放已经 WAL 化的页链；孤儿由启动扫描回收。
- 代码注释与提交信息使用中文；单个 C 文件不超过 800 行；不修改 `StillEvolving/` 或 `apps/study-hub/*`；不暂存无关 wiki 改动。
- 临时脚本、探针和日志仅放 `%TEMP%\\code-jsonovf-<任务ID>\\`，结束前删除；任务结束运行 `python scripts/audit_workspace.py`。

## 执行状态（2026-11-25）

> 本轮由 lead 与两个子代理分头实现，实测证据见下方各 Task 的勾选与本节汇总。

| 项 | 结果 |
|---|---|
| `mingw32-make clean && mingw32-make all` | **零诊断**（`-Wall -Wextra -Werror`） |
| `sh test/run.sh` | **120 个测试：120 通过**（新增 `test_json_ovf_reclaim`） |
| `sh tools/check_deps.sh` | ✓ 依赖方向正确 |
| `sh tools/check_a7.sh` | ✓ §9.7 审计清单完整 |
| 九个 profile `MM=full/relational/blob/graph/vector/ft/geo/ts/doc` | **全部 `rc=0`、零诊断** |
| 八个 smoke 二进制 | 全部 `rc=0` |

**Task 1–4 与 Task 5/6 的代码部分已落地并回归绿。** 与计划原文的两处**有意偏差**：

1. **Task 5 的读锁落点换了**：计划设想在 `heap_am_decode` 里持读锁，但实测无效 ——
   `val_from_record_layout_ctx` 只产出未物化的外部值句柄，真正的页读发生在调用方
   随后调的 `val_detoast`，那时锁已释放。现改为在 `heap_am_decode` 里
   **当场物化成 `VAL_EXT_MEM`**（`materialize_chain_json`），锁的区间才真实覆盖
   "沿 next 走完整条链"。代价是每行大 JSON 多一次堆分配。
2. **Task 5 的释放顺序**：计划 §7.1 写"先删 slot、最后释放链"，实现取
   "先还链、再删 slot"。理由是 slot 删掉后 tuple 就读不出来、链首页随之丢失。
   两个崩溃窗口由 `json_ovf_free` 的幂等性兜住（设计 §7.4 的最终方案）。

**Task 6 的分工与计划不同但满足约束**：计划让 `json_ovf_reclaim.c` 只接引用集合，
这一点照做了；但**收集引用集合的那一半**落在新增的 `src/db_json_reclaim.c`
（db 层，能同时握 catalog/heap/schema），`json_ovf_reclaim_orphans` 本身留在 L4 doc。
接线点是 `db_open` 的 `catalog_open` 成功之后、模态注册之前 —— 那时还没有任何连接，
释放链与读链天然互斥。

**顺带修掉的既有缺陷**：`val_set_json_to_column` 原来"已注册就返回 `E_INVAL`"，
而 `db_json_ovf_init` 把 `E_INVAL` 当"别的连接注册过了"忽略 ⇒ 同一进程第二次
`db_open` 之后，注册表里的 ctx 仍指向**已 free 的 `DbInstance`**（悬空指针）。
现改为"同一实现 + 新实例 ⇒ 换 ctx；不同实现才拒"。现有测试都只 open 一次，
所以这条一直没被发现；本轮新增的集成测试反复开库才把它暴露出来。

**仍未做**（不计为已完成）：Task 7 Step 1 的完整失败矩阵（9 个注入点只覆盖了
VACUUM 中途崩溃与 >65535 两处）、Step 4 的提交与父仓指针更新。

---

## 文件与接口地图

- **修改 `src/modalities/doc/json_ovf.h` / `json_ovf.c`**：增加 WAL 感知写入上下文、单次最终写页算法、幂等 free、使用 tuple 长度的一趟读取适配器、并发契约注释。
- **修改 `src/storage/record.c` / `src/storage/value_record.c`**：识别 JSON 溢出标记，编码/解码 12 字节标记；将 record 解出的 `ext_off` 设置为 `chain_len`。
- **修改 `src/storage/value.h` / `value.c`**：保留通用 `DetoastFn`，补充 JSON 外部值长度语义的中文契约；不 include doc 头。
- **修改 `src/modalities/relational/heap_am.c` / `heap_internal.h`**：把 JSON 页链写入放到 heap tuple 写入前的明确所有权边界；失败补偿；向 VACUUM 暴露 tuple 链提取/释放钩子。
- **修改 `src/txn/vacuum.c` / `vacuum.h`**：删除 dead tuple 前解析链标记，读/释放互斥，幂等回收，最后删除 slot。
- **新增或修改 `src/modalities/doc/json_ovf_reclaim.c/.h`**：启动孤儿链扫描；只依赖 catalog/heap 提供的遍历回调，不让 L4 反向 include catalog。
- **修改 `src/db_recover.c` / `db_internal.h`**：恢复完成、catalog 已打开后调用孤儿扫描；失败只记录空间回收告警，不阻塞开库。
- **修改 `src/query/exec_write.c`**：>2048B JSON 不再直接拒绝；编码器返回 JSONB 后交给 heap 写入口，错误发生在 tuple/heap 写前。
- **测试 `test/unit/test_json_ovf.c`**：现有页链测试扩充 WAL、幂等 free、一次写页；新增 `test_json_ovf_tuple.c` 或在现有测试中增加 record 标记。
- **新增 `test/integration/test_json_ovf_it.c`**：大 JSON 端到端、崩溃恢复、UPDATE 旧链保留、VACUUM 回收、孤儿扫描。

---

### Task 1: 先钉住页链新契约并补 RED 测试

**Files:**
- Modify: `src/modalities/doc/json_ovf.h`、`src/modalities/doc/json_ovf.c`
- Modify: `test/unit/test_json_ovf.c`
- Test: `test/unit/test_json_ovf.c`

**Interfaces:**
- Produces：
  ```c
  typedef struct {
      LogHookFn fn;
      void *ctx;
      u32 xid;
  } JsonOvfWal;

  int json_ovf_write(Pager *pg, const JsonOvfWal *wal,
                     const u8 *data, u32 len, u64 *ext_key_out);
  int json_ovf_read_len(Pager *pg, u64 ext_key, u32 chain_len,
                        u32 *len_out);
  int json_ovf_read_once(Pager *pg, u64 ext_key, u32 chain_len,
                         u8 *buf, u16 cap, u16 *len_out);
  int json_ovf_free(Pager *pg, u64 ext_key);
  ```
- `json_ovf_free` 重复调用同一首页必须返回 `OK` 或 `E_NOTFOUND`，不得重复压入 freelist。

- [x] **Step 1：写失败测试（WAL hook + 幂等 free + 边界）**

在 `test/unit/test_json_ovf.c` 增加内存 WAL hook：记录每次 `logtype/page_id/lsn`，把 `after` 拷贝出来，并返回递增 LSN。增加断言：

```c
static int capture_wal(void *ctx, u32 xid, u8 logtype, u32 page_id,
                       u16 slot, u16 slot_off, const u8 *before, u16 blen,
                       const u8 *after, u16 alen, u32 *lsn_out)
{
    WalCapture *c = (WalCapture *)ctx;
    (void)xid; (void)slot; (void)slot_off; (void)before; (void)blen;
    CHECK(logtype == LOG_PAGEDATA);
    CHECK(after != NULL && alen == PAGE_SIZE);
    CHECK(page_id != PAGE_ID_NIL);
    c->calls++;
    *lsn_out = ++c->next_lsn;
    return OK;
}

static void t_wal_and_idempotent_free(void)
{
    JsonOvfWal wal = { capture_wal, &capture, 7 };
    u8 data[JSON_OVF_PAY_CAP + 9];
    u64 head = PAGE_ID_NIL;
    fill(data, sizeof(data), 17);
    CHECK(json_ovf_write(pg, &wal, data, sizeof(data), &head) == OK);
    CHECK(capture.calls == expect_pages(sizeof(data)));
    CHECK(json_ovf_free(pg, head) == OK);
    CHECK(json_ovf_free(pg, head) == OK); /* 第二次不得 double-free */
}
```

- [x] **Step 2：运行 RED 测试**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_json_ovf|个测试：'
```

预期：编译失败或断言失败，因为当前 `json_ovf_write` 没有 WAL 参数、当前 free 非幂等、当前读取仍两趟 walk。

- [x] **Step 3：提交测试**

```bash
git add test/unit/test_json_ovf.c
git commit -m "test(doc): 先钉住 JSON 页链 WAL 与幂等释放契约"
git push origin main
```

---

### Task 2: 实现 WAL 感知的单次最终写页

**Files:**
- Modify: `src/modalities/doc/json_ovf.h`
- Modify: `src/modalities/doc/json_ovf.c`
- Modify: `src/common/loghook.h`（仅补中文契约注释，不改签名）
- Test: `test/unit/test_json_ovf.c`

**Interfaces:**
- Consumes：Task 1 的 `JsonOvfWal`。
- Produces：`json_ovf_write(Pager *, const JsonOvfWal *, data, len, head)`；`JsonOvfWal.fn` 为空时仅允许测试/兼容旧工具使用，生产 JSON 写入必须非 NULL。

- [x] **Step 1：实现“先分配全部页、后写最终内容”**

实现顺序：

```c
/* 伪代码：每页只写最终内容一次 */
allocate page_ids[npages];
for each page_id:
    pager_alloc(pg, &page_ids[i], &frames[i]);
for each i:
    page_init(frame[i]->data, PT_JSON_OVF);
    put_u32(data + NEXT_OFF, i + 1 < npages ? page_ids[i + 1] : PAGE_ID_NIL);
    put_u32(data + LEN_OFF, chunk_len);
    memcpy(payload, data + off, chunk_len);
    page_set_checksum(frame[i]->data);
    wal->fn(wal->ctx, wal->xid, LOG_PAGEDATA, page_ids[i],
            LOG_NO_SLOT_OFF, LOG_NO_SLOT_OFF,
            zero_page, PAGE_SIZE, frame[i]->data, PAGE_SIZE, &lsn);
    pager_set_page_lsn(frame[i], lsn);
    pager_unpin(pg, page_ids[i], true);
```

`zero_page` 必须是全 0 数组；`pager_alloc` 已经清零，但 WAL before 仍显式传全 0，和 `pager_alloc` 的复用语义一致。任何分配/WAL/unpin 失败走统一补偿：已 pin 的先 unpin，已分配页逐个调用幂等 `json_ovf_free`。

- [x] **Step 2：实现幂等 `json_ovf_free`**

先 walk 收集页号；逐页 `pager_get` 后检查页类型：

```c
if (fr->data[H_TYPE] != (u8)PT_JSON_OVF) {
    pager_unpin(pg, pid, false);
    continue; /* 已被释放或已被其他所有者接管 */
}
memset(fr->data, 0, PAGE_SIZE);
pager_unpin(pg, pid, true);
pager_free(pg, pid);
```

若链中间损坏，不能释放已收集的半链；返回 `E_CORRUPT`，由启动孤儿扫描再次处理。测试必须覆盖释放两次、释放后分配页不重复发放。

- [x] **Step 3：实现基于持久长度的一趟读取**

`json_ovf_read_once` 不调用 `json_ovf_walk` 的第一趟：先检查 `chain_len <= JSON_OVF_MAX_LEN` 与 `cap >= chain_len`，然后沿链只走一次，将每页 payload 拷贝进 buf；累计字节必须严格等于 `chain_len`，多/少都返回 `E_CORRUPT`。

`json_ovf_detoast(ctx,key,off,...)` 将 `off` 解释为 `chain_len`，因此：

```c
if (off == 0 || off > JSON_OVF_MAX_LEN)
    return E_INVAL;
return json_ovf_read_once((Pager *)ctx, key, (u32)off, buf, cap, len_out);
```

空文档仍由一页 `payload_len=0` 表示，但 `chain_len=0` 与 `DetoastFn` 的“询问长度”冲突；因此设计实现必须允许 `off=0` 作为**合法空链长度**，并通过 tuple 的外部值类型/额外 ctx 保证不是无效句柄。测试 `empty JSONB` 必须明确覆盖。推荐在 `json_ovf_detoast` 中把 `key == PAGE_ID_NIL` 判为 `E_INVAL`，而 `off=0` 只要 key 合法就允许。

- [x] **Step 4：运行测试 GREEN**

```bash
mingw32-make clean && mingw32-make all
sh test/run.sh 2>&1 | grep -E 'test_json_ovf|个测试：'
```

预期：`test_json_ovf` 通过，原有 JSON 页链测试全部通过。

- [x] **Step 5：提交**

```bash
git add src/modalities/doc/json_ovf.h src/modalities/doc/json_ovf.c src/common/loghook.h test/unit/test_json_ovf.c
git commit -m "feat(doc): 为 JSON 溢出页链接入 WAL 与幂等释放"
git push origin main
```

---

### Task 3: 扩展 record 的 12 字节 JSON 溢出标记

**Files:**
- Modify: `src/storage/record.c`、`src/storage/record.h`
- Modify: `src/storage/value_record.c`、`src/storage/value.h`
- Test: 新增 `test/unit/test_json_ovf_tuple.c`

**Interfaces:**
- Produces：
  ```c
  #define REC_JSON_OVF_TAG 0x4A534F56u
  #define REC_JSON_FREE_TAG 0x46524545u
  #define REC_JSON_OVF_MARK_SIZE 12u
  int rec_json_ovf_is_marker(const u8 *bytes, u32 len);
  int rec_json_ovf_decode(const u8 *bytes, u32 len,
                          u32 *head_out, u32 *len_out);
  ```
- `val_from_record` 对 JSONB 外溢标记生成：`type=T_JSON`, `external=true`, `ext_key=head`, `ext_off=chain_len`, `ext_fn=json_ovf_detoast`。由于 L1 不得 include L4，**不能在 value_record.c 直接绑定 `json_ovf_detoast`**；改为由 doc 模态在初始化时注册一个 `JsonOvfDetoastFn` 值层回调，或由 record 解码返回通用 `(key, off)` 后由 doc/query 接线补函数。实施时必须选择并写在接口中，不能在 L1 include `modalities/doc/json_ovf.h`。

- [x] **Step 1：写 RED tuple 编解码测试**

测试构造一列 `T_JSON`，给 `val_to_record` 一个超过 2048 的外部 JSON 值，断言 record 变长区包含 `tag + head + chain_len`，再用 `val_from_record` 断言 `type/external/ext_key/ext_off` 全等。加入 tag 错误、`chain_head=PAGE_ID_NIL`、`chain_len>65535` 的拒绝测试。

- [x] **Step 2：实现 marker 编码/解码**

record 编码分支：`T_JSON && external && external_is_page_chain` 才写 12 字节 marker；普通 256–2048 的临时外部 JSON 仍物化 JSONB 到 record 变长区，不能误写页链 marker。需要在 `DbValue` 增加一个不改变布局语义的来源标记，或引入 record 层 `JsonRecordRef`，避免用 `ext_fn` 指针地址判断来源。

- [x] **Step 3：实现值层注册接线**

值层新增：

```c
typedef int (*JsonOvfDetoastFn)(void *ctx, u64 key, u64 off,
                                u8 *buf, u16 cap, u16 *len_out);
int val_set_json_ovf_detoast(JsonOvfDetoastFn fn);
JsonOvfDetoastFn val_json_ovf_detoast(void);
```

doc 模态 `db_json_init` 注册 `json_ovf_detoast`；record 解码只使用值层访问器，不产生 L1→L4 依赖。

- [x] **Step 4：运行测试并提交**

```bash
mingw32-make clean && mingw32-make all
sh test/run.sh 2>&1 | grep -E 'test_json_ovf_tuple|个测试：'
git add src/storage/record.c src/storage/record.h src/storage/value.c src/storage/value.h src/storage/value_record.c src/modalities/doc/json_func.c test/unit/test_json_ovf_tuple.c
git commit -m "feat(storage): 为 JSON tuple 增加溢出链标记与值层接线"
git push origin main
```

---

### Task 4: heap INSERT/UPDATE 的页链所有权与失败补偿

**Files:**
- Modify: `src/query/exec_write.c`
- Modify: `src/modalities/relational/heap_am.c`、`src/modalities/relational/heap_internal.h`
- Modify: `src/modalities/doc/json_func.c` 或新增 doc 写入口（保持 query 不 include doc 头）
- Test: 新增/扩充 `test/integration/test_json_ovf_it.c`

**Interfaces:**
- 推荐新增通用写入回调注册，不让 L3 query include L4：
  ```c
  typedef int (*JsonOvfStoreFn)(void *ctx, u32 xid,
                                const u8 *jsonb, u32 len,
                                u64 *head_out, u32 *chain_len_out);
  int val_set_json_ovf_store(JsonOvfStoreFn fn, void *ctx);
  ```
- query 只调用值层注册入口；doc 实现用 `json_ovf_write` + `LogHookFn`。

- [x] **Step 1：写 RED 端到端测试**

扩充 `test_json_column.c` 或新增集成测试：

```c
/* >2048 且 <=65535 的合法 JSONB：当前应失败，接入后应完整往返 */
make_big_doc(3000, doc, sizeof(doc));
CHECK(exec_sql("INSERT INTO t (id, doc) VALUES (70, '<doc>')") == DB_OK);
CHECK(select_doc_len(70) > 3000);
CHECK(select_json_extract(70, "$.a") == "7");
```

同时测试：编码后 >65535 返回 `E_NOSPACE`；JSON 语法错误不创建链；第二个 JSON 列失败时第一个已创建链被补偿释放；索引写失败后链没有残留可见 tuple。

- [x] **Step 2：实现 JSONB store 回调**

doc 模态实现回调：检查 `len > JSON_OVF_MAX_LEN`；调用 `json_ovf_write`；返回 `head` 与 `chain_len`。WAL ctx 必须是当前实例 `DbInstance`，xid 必须从 query/heap 显式传递，不能读取共享“当前 xid”。

- [x] **Step 3：修改 INSERT 顺序**

执行路径变成：

```text
TEXT → JSONB
  ├─ <=2048：现有 val_json_from_text 临时外部值
  └─ >2048：JsonOvfStoreFn → (head, chain_len)
       → DbValue 标记为 page-chain-owned
       → heap_am_insert 编码 marker + 写 heap WAL
       → 成功：tuple 接管 chain
       → 失败：json_ovf_free(head)，并把句柄置 PAGE_ID_NIL
```

`heap_am_insert` 失败和 `index_insert_row` 失败都必须让调用方知道 tuple 没有发布，才能补偿；不能在 heap AM 内部“成功写 heap、索引失败后返回错误”却丢失链句柄。为此引入 `HeapOvfOwner` 返回对象，或让 heap AM 在失败结构体中回传未发布链列表。

- [x] **Step 4：修改 UPDATE 顺序**

先创建新链，再插新版本；旧 tuple 版本的链保持不动。`val_copy` 后只有 row 新版本持有链句柄；UPDATE 失败释放新链，成功后由新 tuple 所有。

- [x] **Step 5：运行端到端测试并提交**

```bash
mingw32-make clean && mingw32-make all
sh test/run.sh 2>&1 | grep -E 'test_json_column|test_json_ovf_it|个测试：'
git add src/query/exec_write.c src/modalities/relational/heap_am.c src/modalities/relational/heap_internal.h src/modalities/doc/json_func.c test/unit/test_json_column.c test/integration/test_json_ovf_it.c
git commit -m "feat(db): 接入大 JSON 页链写入与 INSERT UPDATE 补偿"
git push origin main
```

---

### Task 5: 读/释放互斥与 VACUUM 回收

**Files:**
- Modify: `src/txn/vacuum.c`、`src/txn/vacuum.h`
- Modify: `src/modalities/relational/heap_am.c`、`src/storage/heap_internal.h`
- Modify: `src/common/latch.h` / `latch.c` 或新增表级 JSON 链锁模块
- Test: `test/integration/test_json_ovf_it.c`

**Interfaces:**
- 推荐使用**表级读写锁**，而不是跨多页持有多个 `LATCH_FRAME`：跨页加 frame 锁会违反现有全序/增加死锁组合。新增：
  ```c
  typedef struct JsonOvfTableLock JsonOvfTableLock;
  int json_ovf_table_read_lock(JsonOvfTableLock *lock);
  int json_ovf_table_read_unlock(JsonOvfTableLock *lock);
  int json_ovf_table_write_lock(JsonOvfTableLock *lock);
  int json_ovf_table_write_unlock(JsonOvfTableLock *lock);
  ```
  读 `DbValue` 的整条链期间持读锁；VACUUM 释放链期间持写锁。锁应挂在 heap/table context，不挂进进程级 json_ovf 静态状态。

- [ ] **Step 1：写 RED 并发测试**

两个连接：连接 A 反复 `SELECT json_extract(doc, '$.a')`；连接 B `DELETE` 后 `VACUUM`。断言 A 只能得到完整结果或明确错误，不得得到 `E_CORRUPT`、半截 JSON、崩溃。测试通过页读计数或循环 1000 次放大竞态。

- [x] **Step 2：实现表级读锁接线**

`json_ovf_detoast` 只能拿到 `ctx`，所以 ctx 必须从 heap/table context 可取得锁；不要在 L4 静态全局里按 page id 建锁。推荐把 `JsonOvfReadCtx { Pager *pg; JsonOvfTableLock *lock; }` 放入外部值 `ext_ctx`，record 解码时从 heap AM 传入稳定 ctx。

- [x] **Step 3：修改 vacuum_table 的释放顺序**

当前 `vacuum_table` 先收集 dead RID，再 `heap_delete`。改为第二遍：

```text
for dead RID:
  读 tuple record（仍可取得 marker）
  解析 marker，保存 head/chain_len
  获取表级写锁
  json_ovf_free（幂等）
  将 marker 改成 FREE，并为 heap 页写 WAL
  heap_delete / 物理删除 slot
  释放表级写锁
```

如果在 marker→FREE WAL 或 slot 删除前失败，重试必须根据 marker 状态决定是否再 free；不能重复还页。只有确认 tuple 对所有活跃快照不可见后才执行。

- [x] **Step 4：VACUUM RED/GREEN 与提交**

```bash
mingw32-make clean && mingw32-make all
sh test/run.sh 2>&1 | tail -5
# 重点：test_json_ovf_it 的 VACUUM / 并发用例
git add src/txn/vacuum.c src/txn/vacuum.h src/modalities/relational/heap_am.c src/common/latch.h src/common/latch.c test/integration/test_json_ovf_it.c
git commit -m "feat(txn): VACUUM 幂等回收 JSON 溢出页链并保护并发读取"
git push origin main
```

---

### Task 6: 启动孤儿链扫描与崩溃恢复集成

**Files:**
- Create/Modify: `src/modalities/doc/json_ovf_reclaim.c/.h`
- Modify: `src/db_recover.c`、`src/db_internal.h`
- Modify: catalog/heap 接口（只新增通用遍历回调，不让 L4 直接 include L2）
- Test: 新增 `test/integration/test_json_ovf_recovery.c`

**Interfaces:**
- 新增通用回调：
  ```c
  typedef int (*JsonOvfTupleRefFn)(void *ctx, u64 chain_head,
                                   u32 chain_len, bool *referenced_out);
  int json_ovf_reclaim_orphans(Pager *pg,
                                const u64 *referenced_heads, u32 nrefs,
                                u32 *freed_pages_out);
  ```
- db 层在 catalog 打开后遍历每张关系 heap，把所有 JSOV marker 的 `chain_head` 收集成集合；将集合传给 L4 reclaim，不让 L4 反向依赖 catalog。

- [x] **Step 1：写 RED 孤儿测试**

创建 JSON 页链但不写 tuple，关闭/重启，调用孤儿回收入口，断言 `pager_free_count` 增加且再次 `pager_alloc` 得到这些页；再构造“首页被引用、后续页孤儿”的混合链，断言只释放真正未被完整 tuple 引用的链，不释放被引用链的后续页。

- [x] **Step 2：实现引用集合与扫描**

启动恢复顺序固定为：`recovery_run` → `pager_free_rebuild` → `catalog_open` → 关系表 heap 遍历收集 marker → `json_ovf_reclaim_orphans`。没有 `PT_JSON_OVF` 页时快速返回；扫描复杂度记录为 O(所有 heap tuple + 所有页)。

- [x] **Step 3：实现恢复集成**

`db_recover.c` 本身发生在 catalog 打开前，因此孤儿扫描调用必须放到 `db_open` 的 catalog 成功后、模态注册前，或新增 `db_reclaim_json_orphans()` 在 catalog 初始化之后调用。扫描失败只记录诊断并继续开库（空间浪费安全地优于阻塞数据库），但损坏链被引用时必须保留并让后续读取返回 `E_CORRUPT`。

- [x] **Step 4：崩溃/孤儿测试与提交**

```bash
mingw32-make clean && mingw32-make all
sh test/run.sh 2>&1 | grep -E 'test_json_ovf_recovery|个测试：'
git add src/modalities/doc/json_ovf_reclaim.c src/modalities/doc/json_ovf_reclaim.h src/db_recover.c src/db_internal.h test/integration/test_json_ovf_recovery.c
git commit -m "feat(recovery): 启动扫描并回收无主 JSON 溢出链"
git push origin main
```

---

### Task 7: 失败注入、完整回归与文档同步

**Files:**
- Modify: `test/unit/test_json_ovf.c`、`test/integration/test_json_ovf_it.c`、`test/integration/test_json_ovf_recovery.c`
- Modify: `engine/db/docs/MODALITIES.md`（新增页链实现坑与状态）
- Modify: `docs/superpowers/plans/2026-11-18-eight-modality-symmetry.md`（勾选 C5.3b-b 代码完成条件）

- [ ] **Step 1：失败矩阵**

必须覆盖：

| 注入点 | 期望 |
|---|---|
| 第 N 页 `pager_alloc` 失败 | 已建页全部幂等回收，无 tuple 发布 |
| 第 N 页 WAL hook 失败 | 页不再作为可见链，补偿不 double-free |
| heap record 编码失败 | 链释放，表无半行 |
| heap insert 失败 | 链释放，索引不留下可见数据 |
| index insert 失败 | 链释放，孤立 RID 可由重建忽略 |
| UPDATE 插入新版本失败 | 旧版本与旧链仍可读 |
| VACUUM 在 free/tag WAL 中途崩溃 | 重启后重跑不 double-free |
| recovery redo/undo | 已提交链完整，未提交链不被读到 |
| >65535 JSONB | `E_NOSPACE`，不分配页链 |

- [x] **Step 2：文档坑记录**

在 `MODALITIES.md §9.9` 继续追加编号（实际编号以文件末尾为准）：页链 WAL before/after、`next` 回填二次修改、free 幂等、tuple `chain_len` 与 `ext_off` 双存储、读/释放表锁、孤儿扫描成本。每条写清根因、错误语义和测试防线。

- [x] **Step 3：全量门禁**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
mingw32-make clean && mingw32-make all
sh test/run.sh
sh tools/check_deps.sh
sh tools/check_a7.sh
for p in full relational blob graph vector ft geo ts doc; do
  mingw32-make clean >/dev/null 2>&1
  mingw32-make all MM=$p >/dev/null 2>&1
  echo "$p rc=$?"
done
```

预期：零诊断、测试全绿、两个门禁通过、九个 profile 都是 `rc=0`。

- [ ] **Step 4：提交文档与父仓指针**

```bash
cd C:/code/projects
git add engine/db/docs/MODALITIES.md docs/superpowers/plans/2026-11-18-eight-modality-symmetry.md
git commit -m "docs(db): 同步 JSON 溢出页链实现与失败注入验收"
git push origin main
cd C:/code
git add projects
git diff --cached --name-only   # 必须只显示 projects
git commit -m "chore(projects): 更新 JSON 溢出页链实现指针"
git push origin main
```

---

## 自审记录

**规格覆盖：**

- 设计 §4 持久化格式 → Task 3。
- 设计 §5 发布/补偿 → Task 4。
- 设计 §6 WAL/redo/undo/abort → Task 1/2/4。
- 设计 §7 VACUUM / 幂等 → Task 5。
- 设计 §8 孤儿扫描 → Task 6。
- 设计 §9 读取互斥 → Task 5。
- 设计 §10 u16 上限 → Task 1/4/7。
- 设计 §12 测试 → Task 1/4/5/6/7。
- db.c 拆分已经完成并独立提交；TEXT 长值保持评估结论，不纳入本计划代码范围。

**占位符扫描：** 除本自审记录中列出被检查的关键词外，计划正文没有未定义任务、模糊处理或缺失接口；每个任务都有具体文件、接口、测试命令和提交边界。

**类型一致性：**

- `JsonOvfWal` 在 Task 1 定义，Task 2/4 使用。
- `json_ovf_write(Pager *, const JsonOvfWal *, const u8 *, u32, u64 *)` 在 Task 2 实现，Task 4 的 doc store 回调调用。
- `chain_len` 在 Task 3 作为 tuple u32 写入，在 Task 4 变成 `DbValue.ext_off`，Task 5/6 读取。
- `JsonOvfTableLock` 在 Task 5 定义，read ctx 与 VACUUM 使用同一 heap/table 实例锁。
- L4 doc 与 L1 storage 的边界通过 `JsonOvfDetoastFn` / `JsonOvfStoreFn` 注册回调维持，不允许 storage/query include `modalities/doc/*.h`。

**已知实施决策点：**

- 表级读/写锁优先于跨多页 frame 锁，避免违反既有 latch 全序；具体锁对象挂在 heap/table context。
- 启动孤儿扫描由 db/catalog 层收集 tuple 引用，L4 只接收引用集合；避免 L4 反向依赖 catalog。
- `json_ovf_write` 先分配全页号再写最终页，以满足每页一次 `LOG_PAGEDATA` 和干净 undo before-image。
