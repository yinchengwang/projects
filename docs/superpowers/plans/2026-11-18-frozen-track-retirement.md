# 冻结轨退役 —— 删除 `engine/src/db/` 并让 SDK 自持（工作流 W-A）

> 依据：`docs/superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md` §5.1 / §5.4 / §5.5 / §5.6
> 落地日期：2026-11-18
> 前置：**必须先做完 W-B 的 P0（CI 门禁）** —— 没有红灯就看不出删除有没有打坏东西。
> 后续：W-C 的 C-1（执行器脱钩）与本计划**不冲突** —— C-1 动的是 `engine/db/src/`（`spi/` / `query/` / `modalities/`），本计划动的是 `engine/src/sdk/` 与 `engine/include/`。

---

## Goal

把冻结轨 `engine/src/db/`（524 文件 / 209,907 行）整棵拔掉，让仓库里**只剩一个数据库内核**（`engine/db/`），同时保证：

1. `engine/db/`（主核，自带 Makefile，不在 CMake 图内）**完全不受影响**；
2. `engine/src/sdk/`（嵌入式 SDK）继续可构建、可测试，且**不再引用任何冻结轨的源码或公开头**；
3. CMake 工程轨 `cmake -B build/engine -S engine` 继续绿，ctest 用例数只减冻结轨那部分。

## Architecture

两条线必须**同时**落地，中间不存在"能编译但没意义"的中间态：

```
线 ①  SDK 自持（先做，做完就能证明"不依赖冻结轨"）
    M-1  memctx 移植
         engine/include/db/sql/memctx.h   (503 行)  →  engine/include/sdk/impl/memctx.h
         engine/src/db/sql/memctx.c       (834 行)  →  engine/src/sdk/core/memctx.c
         连带：memctx.h 依赖的 db/parser/sql/parsenodes.h 也在删除清单里
               ⇒ NodeTag 换成 SDK 本地的两值枚举
    H-1  向量索引后端
         engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw.h  （头本身在冻结轨内）
         engine/include/db/index/vector_index/vector_index_selector.h （头在 engine/include/db/）
               →  engine/include/sdk/impl/index_flat.h
                  engine/src/sdk/vectors/index_flat.c
         语义：近似 HNSW → **精确暴力 KNN**（召回率只升不降；延迟换来的）

线 ②  删轨（后做，**一个原子提交**：删了就得同时把构建接线修好）
    D-1  engine/src/db/                     524 文件 / 209,907 行
    D-2  engine/test/db/                    289 文件 / ~587 ctest 用例
    D-3  engine/include/db/                 409 个公开头
    D-4  engine/include/rag/ 2 头 + engine/include/tree_index/ 8 头
    D-5  engine/cmake/ModalityProfiles.cmake、engine/cmake/MultimodalConfig.cmake（旧轨机制）
    D-6  engine/test/smoke/                 13 个文件（全是冻结轨）
D-7  engine/test/index/                8 个目录（每个目录下的测试都 include `<index/...>`，随冻结轨一起没了 ⇒ 全部要删；设计 D-10 写的「4 个」是错的）
    D-8  engine/tools/benchmark/c_api/      依赖 mm_vector_index / faiss_hnsw
D-9  engine/src/db/ 下被 SDK 引用的**三个** CMake 目标：sql_engine / faiss_hnsw / mm_vector_index
```

**关键约束：SDK 的依赖必须先解，轨才能删。** `mmsdk` 链接的**四个**目标 `sqlite3 / faiss_hnsw / mm_vector_index / sql_engine` 里，后三个**全部定义在冻结轨的 CMakeLists 里**（`sqlite3` 是第三方，不受影响）：

- `engine/src/db/sql/CMakeLists.txt:7` → `add_library(sql_engine STATIC ...)`
- `engine/src/db/index/vector_index/CMakeLists.txt:39` → `add_library(mm_vector_index STATIC ...)`
- `engine/src/db/index/vector_index/faiss_hnsw/CMakeLists.txt:6` → `add_library(faiss_hnsw STATIC ...)`

## Tech Stack

- C11（SDK 侧 `CMAKE_C_STANDARD 11`）、C++17（测试）
- CMake ≥ 3.20、GNU Make（仅主核，本计划不碰）
- GoogleTest（vendored，`vendor/third_part/googletest/`）
- SQLite amalgamation（`engine/third_part/sqlite3/`）—— SDK 的唯一外部依赖

## Global Constraints

1. **提交信息用中文**；每完成一个 Task 立即 `git push origin main`（`projects` 仓）。
2. **只提交与本任务相关的文件**；父仓 `code` 的 `chore(submodule): 更新 projects 指针` 单独提交。
3. **不准碰**这些既有未提交文件：`apps/study-hub/server/app.js`、`apps/study-hub/server/router.js`、`apps/study-hub/server/tests/router.test.js`。
4. **不准把 `StillEvolving/` 加进版本库**（未跟踪目录）。
5. 父仓根目录的越界文件（`b.txt`、`bench_out.txt`、`ftdbg1~6.c`、`stat_err.txt`、`build_a3.log`、`extract-plan.js`、`verify_urls.py`）**本次不动**，只在交付说明里提醒。
6. **不新建 `engine/db/` 之外的数据库内核**；本计划不碰 `engine/db/` 任何文件。
7. 公开 API（`engine/include/sdk/mmdb*.h`）**签名不许变**；只允许改 `engine/include/sdk/impl/` 下的实现级头。
8. 源码注释用中文；注释里说明"为什么"，不说明"是什么"。
9. 每个 Task 结束时**工作区必须能完整构建 + 全部 ctest 通过**。

## Review Focus（审查者重点看这几处）

| # | 关注点 | 判据 |
|---|---|---|
| R1 | memctx 移植有没有**漏符号** | `nm -g build/engine/src/sdk/libmmsdk.a` 里 `palloc` / `pfree` / `AllocSetContextCreate` / `MemoryContextReset` / `MemoryContextDelete` / `MemoryContextCheckThread` / `mmdb_mem_register_resource` / `mmdb_mem_unregister_resource` 全部是 `T`（已定义），不是 `U` |
| R2 | 移植后有没有**残留冻结轨引用** | `grep -rn 'db/sql/memctx\|db/parser/sql/parsenodes\|faiss_hnsw\|vector_index_selector' engine/src/sdk engine/include/sdk` **零命中** |
| R3 | SDK 向量检索**语义有没有变差** | 召回率只升不降（HNSW 近似 → 精确）；`mmdb_vectors_search` 的返回结构、`apply_pagination`、`mmdb_metrics_record_query` 调用点一个不少 |
| R4 | 删轨是不是**一个原子提交** | 删轨提交里必须同时包含构建接线修复；不允许出现"删了但 build 不过"的提交 |
| R5 | 有没有**误删主核** | `engine/db/` 目录在提交后**仍然完整存在**且 `mingw32-make check` 绿 |
| R6 | ctest 用例数 | 删除前后差值必须**恰好等于**冻结轨那部分（`engine/test/db/` + `engine/test/smoke/` + `engine/test/index/`）—— ⚠️ **（复审修正）** `engine/test/index/` 实际是 **8 个目录**（设计 D-10 写的「4 个」是错的），差值要按 8 个目录算

---

## 文件结构

### 新增

| 文件 | 行数量级 | 作用 |
|---|---|---|
| `engine/include/sdk/impl/memctx.h` | ~500 | SDK 自持的 AllocSet 内存上下文（原 `engine/include/db/sql/memctx.h` 移植） |
| `engine/src/sdk/core/memctx.c` | ~840 | 同上实现（原 `engine/src/db/sql/memctx.c` 移植） |
| `engine/include/sdk/impl/index_flat.h` | ~90 | SDK 自持的扁平（精确 KNN）索引接口 |
| `engine/src/sdk/vectors/index_flat.c` | ~260 | 扁平索引实现（原 `index_flat.c` 的 10 行占位替换） |
| `engine/test/sdk/vectors/index_flat_test.cpp` | ~180 | 扁平索引的单元测试 |

### 修改

| 文件 | 改什么 |
|---|---|
| `engine/include/sdk/impl/mmdb_memctx.h` | include 从 `db/sql/memctx.h` 换成 `sdk/impl/memctx.h` |
| `engine/src/sdk/core/mmdb_memctx.c` | 无需改（它只 include `sdk/impl/mmdb_memctx.h`） |
| `engine/src/sdk/vectors/vectors.c` | HNSW → 扁平索引（9 个调用点 + 1 个类型 + 2 个 include） |
| `engine/include/sdk/impl/vectors.h` | `mmdb_vectors_hnsw_*` → `mmdb_vectors_index_*` |
| `engine/include/sdk/impl/mmdb_internal.h` | `mmdb_collection_t.hnsw` 字段 → `vec_index` |
| `engine/src/sdk/CMakeLists.txt` | `SDK_CORE_SOURCES` 加 `core/memctx.c`；链接只留 `sqlite3`；删 `../` include 路径 |
| `engine/src/CMakeLists.txt` | 删 `add_subdirectory(db)` |
| `engine/test/CMakeLists.txt` | 删冻结轨守卫块、删 `smoke` / `kbase` 子目录引用、清理 `_test_targets` |
| `engine/test/sdk/memory/*.cpp`（**6 个**，其中 **5 个**含 `db/sql/memctx.h`） | include `db/sql/memctx.h` → `sdk/impl/memctx.h` |
| `engine/test/sdk/CMakeLists.txt` | 删 `hnsw` 子目录引用 |

### 删除

| 路径 | 规模 |
|---|---|
| `engine/src/db/` | 524 文件 / 209,907 行 |
| `engine/test/db/` | 289 文件 / ~587 ctest 用例 |
| `engine/include/db/` | 409 个头 |
| `engine/include/rag/` | 2 个头 |
| `engine/include/tree_index/` | 8 个头 |
| `engine/cmake/ModalityProfiles.cmake`、`engine/cmake/MultimodalConfig.cmake` | 旧轨模态开关机制 |
| `engine/test/smoke/` | 13 个文件 |
| `engine/test/index/` | **8 个目录**（不是 8 个文件；设计 D-10 写「4 个目录」有误） |
| `engine/test/sdk/hnsw/` | 2 个文件（测的是冻结轨的 HNSW） |
| `engine/tools/benchmark/c_api/` | 依赖 `mm_vector_index` / `faiss_hnsw` |

---

### Task 1：采基线（不改代码，只产出一份可比较的数字）

**Files**
- 新建：`docs/superpowers/plans/2026-11-18-frozen-track-retirement-baseline.md`

**Interfaces**
- 产出一份"删除前"的基线表：ctest 用例名集合、用例总数、构建产物清单。

**Why 单独一个 Task**：删轨之后没法回看删了什么。基线是后面每个 Task 的验收依据。

- [ ] **1.1 重建 `build/engine`（CMakeCache 已过期 2 周，必须先重建）**

  ```powershell
  cd C:\code\projects
  cmake -B build/engine -S engine -DBUILD_TESTING=ON
  cmake --build build/engine --parallel 4
  ```

  预期：`-- Build files have been written to: C:\code\projects\build/engine` + 编译无 error。

- [ ] **1.1b 打回滚 tag `pre-frozen-track-removal`**

  ⚠️ **（复审修正）删除前必须留一个能回退的点** ——
  这次要删掉 **524 个文件 / 209,907 行**，是不可逆的大动作。
  设计 §9 与 §7 的风险表都点名要打 tag，原计划漏了。

  ```powershell
  cd C:\code\projects
  git status                     # 必须干净（或只剩无关的改动）
  git tag -a pre-frozen-track-removal -m "冻结轨删除前的基线（W-A 执行前）"
  git tag -l pre-frozen-track-removal
  git rev-parse pre-frozen-track-removal
  ```

  ⚠️ **打 tag 前先确认工作区状态** —— 别把别人正在做的改动一起打进去。

  ⚠️ **回退命令**（万一删完发现回不来）：

  ```powershell
  git reset --hard pre-frozen-track-removal
  git push --force origin main     # ⚠️ 危险：只在确认要放弃后续提交时用
  ```

- [ ] **1.2 采 ctest 基线**
  ⚠️ **（复审修正）只采「用例总数 N」不够** ——
  设计 §7 的风险表明令：**必须采改造前基线（失败用例名集合，跑两次取交集），
  只比「集合不新增」而非绝对值**。原计划只记了总数 N，
  验收时又拿「用例数 == N」当判据 ——
  **方向反了**：删轨本来就该让用例数变少，绝对值相等反而是错的判据。

  ⇒ 补采**失败用例名集合**（跑两遍取交集，滤掉偶发失败）：

  ```powershell
  cd C:\code\projects
  # 第一遍
  ctest --test-dir build/engine --output-on-failure 2>&1 |
    Select-String -Pattern '^\s*\d+ - .*\(Failed\)' |
    ForEach-Object { $_.Line -replace '^\s*\d+ - ','' -replace ' \(Failed\).*$','' } |
    Sort-Object -Unique | Out-File -Encoding utf8 build/engine/failed-run1.txt
  # 第二遍（隔一会儿再跑，滤掉偶发）
  ctest --test-dir build/engine --output-on-failure 2>&1 |
    Select-String -Pattern '^\s*\d+ - .*\(Failed\)' |
    ForEach-Object { $_.Line -replace '^\s*\d+ - ','' -replace ' \(Failed\).*$','' } |
    Sort-Object -Unique | Out-File -Encoding utf8 build/engine/failed-run2.txt
  # 交集 = 稳定的失败集合
  Compare-Object (Get-Content build/engine/failed-run1.txt) (Get-Content build/engine/failed-run2.txt) -IncludeEqual -ExcludeDifferent |
    Select-Object -ExpandProperty InputObject | Out-File -Encoding utf8 build/engine/failed-baseline.txt
  ```

  ⇒ 把 `failed-baseline.txt` 一起提交（它是**基线**，不是临时文件）。


- [ ] **1.2 采 ctest 基线**

  ```powershell
  ctest --test-dir build/engine -N | Out-File -Encoding utf8 build/engine/ctest-baseline.txt
  (Select-String -Path build/engine/ctest-baseline.txt -Pattern '^\s*Test\s+#').Count
  ```

  预期：打印用例总数 N（冻结轨默认关闭，所以 N 里**不含** `engine/test/db/` 的 587 条）。
  把这个 N 记下来 —— 后面验收要逐条比对。

- [ ] **1.3 采"若打开冻结轨测试"的用例总数（用于核对删除量）**

  ```powershell
  cmake -B build/engine-frozen -S engine -DBUILD_TESTING=ON -DENABLE_FROZEN_SRC_DB_TESTS=ON
  cmake --build build/engine-frozen --parallel 4
  ctest --test-dir build/engine-frozen -N | Select-String 'Total Tests'
  ```

  预期：比 1.2 多出约 587 条。记下差值 **D**（= 冻结轨独占的用例数）。
  删轨后，工程轨的用例数必须**恰好回到 N**。

- [ ] **1.4 采主核测试基线（证明本计划没碰主核）**

  ```powershell
  cd C:\code\projects\engine\db
  mingw32-make clean
  mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：`95 个测试：95 通过`（64 unit + 31 integration），0 失败。

- [ ] **1.5 写基线文件**

  `docs/superpowers/plans/2026-11-18-frozen-track-retirement-baseline.md` 内容：

  ```markdown
  # 冻结轨删除前基线（2026-11-18）

  | 项 | 值 |
  |---|---|
  | 工程轨 ctest 用例数 N | <1.2 输出> |
  | 打开冻结轨后 ctest 用例数 | <1.3 输出> |
  | 冻结轨独占用例数 D | <1.3 − 1.2> |
| **稳定失败用例名集合**（两次跑取交集） | `build/engine/failed-baseline.txt` |

⚠️ **（复审修正）删除后的验收判据**：
- ❌ **不是**「用例数 == N」—— 删轨本来就要让用例数变少，绝对值相等是错的判据。
- ✅ **是**「**失败用例名集合 ⊆ 基线集合（不新增）**」 + 「主核的 95 个测试全绿」。
- ✅ 另外对账：删掉的用例数应与 D **大致相符**（多删少删都要能解释清楚 ——
  W-A 会多删 `engine/test/index/` 的全部 8 个目录，见 Task 4 的偏离说明）。
  | 主核测试文件数 | 95（64 unit + 31 integration） |
  | 主核测试结果 | 95 通过 / 0 失败 |
  | SDK 用例数 | <ctest -N 里 sdk_* 的条数> |
  | SDK 内存上下文用例数 | 6（memory/ 下 6 个 .cpp） |

  **删除后验收**：工程轨 ctest 用例数 == N；主核结果与上表一致。
  ```

- [ ] **1.6 提交**

  ```powershell
  cd C:\code\projects
  git add docs/superpowers/plans/2026-11-18-frozen-track-retirement-baseline.md
  git commit -m "docs(冻结轨): 补删除前基线（ctest 用例数 / 主核测试结果）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

  ⚠️ `build/engine-frozen/` 是临时构建目录，**不进版本库**（`.gitignore` 已覆盖 `build/`）。

---

### Task 2：memctx 移植进 SDK（冻结轨的第一条依赖线）

**Files**
- 新建：`engine/include/sdk/impl/memctx.h`、`engine/src/sdk/core/memctx.c`
- 修改：`engine/include/sdk/impl/mmdb_memctx.h`、`engine/src/sdk/CMakeLists.txt`
- 修改：`engine/test/sdk/memory/memory_leak_detection_test.cpp`、`mmdb_request_scope_test.cpp`、`mmdb_root_context_test.cpp`、`sdk_core_memctx_test.cpp`、`sdk_vectors_memctx_test.cpp`

**Interfaces**
- 新增公开符号（SDK 内）：`palloc` / `palloc0` / `pfree` / `reset_memory` / `delete_memory` / `AllocSetContextCreate` / `MemoryContextReset` / `MemoryContextResetChildren` / `MemoryContextDelete` / `MemoryContextCurrent` / `MemoryContextSwitchTo` / `MemoryContextCheckThread` / `MemoryContextSetThreadOwner` / `MemoryContextGetGeneration` / `mmdb_current_thread_id` / `mmdb_mem_register_resource` / `mmdb_mem_unregister_resource`
- 新增类型：`MemoryContext`（`struct MemoryContextData *`）、`MemoryContextData`、`MemoryAllocationHeader`、`AllocSetContext`、`AllocSetBlock`、`MemoryResource`、`Size`、`AllocSetPreset`
- `typedef MemoryContext mmdb_memctx_t;` 的定义位置从 `db/sql/memctx.h` 换到 `sdk/impl/memctx.h`，**签名不变**

**前置**：Task 1 完成。

- [ ] **2.1 新建 `engine/include/sdk/impl/memctx.h`**

  从 `engine/include/db/sql/memctx.h`（503 行）复制，**只做下面三处改动**，其余逐字保留：

  ```c
  #ifndef SDK_IMPL_MEMCTX_H
  #define SDK_IMPL_MEMCTX_H
  ```

  ```c
  /* 前向声明 */
  typedef struct MemoryContextData *MemoryContext;

  /*
   * SDK 自持的节点类型。
   *
   * 【为什么换掉原来的 NodeTag】
   * 原实现复用冻结内核 parser 层的 NodeTag（只为区分"普通上下文"与
   * "AllocSet 上下文"两种），而那个头 db/parser/sql/parsenodes.h
   * 随冻结轨一并删除 —— 留着就是断链。
   *
   * 这里换成本地的两值枚举：字段名（type）、整型宽度（int 宽的枚举）
   * 与原实现完全一致 ⇒ SDK 侧的内存布局不变。
   */
  typedef enum {
      T_MemoryContext   = 100,
      T_AllocSetContext = 101
  } MmdbNodeTag;
  ```

  ```c
  typedef struct MemoryContextData {
      MmdbNodeTag            type;          /**< T_MemoryContext / T_AllocSetContext */
      ...
  ```

  ⚠️ **必须保留 `ALLOCSET_PRESET_SMALL高频` 这个标识符原样**（枚举名里嵌了中文）。
  它出现在 `memctx.h` 的 `AllocSetPreset` 枚举里。改名会牵连别处，且属于
  "顺手优化相邻代码"——本次不做。**在移植后的文件里必须逐字保留**。

- [ ] **2.2 新建 `engine/src/sdk/core/memctx.c`**

  从 `engine/src/db/sql/memctx.c`（834 行）复制，只改 include：

  ```c
  #include "sdk/impl/memctx.h"   /* 原为 #include "db/sql/memctx.h" */
  ```

  ⚠️ **两个文件必须当成一个整体搬**。`memctx.c` 用到的
  `MemoryAllocationHeader` / `GET_ALLOCATION_HEADER` / `ALLOCSET_ALIGN` /
  `ALLOCSET_MAX_SIZE` / `CHECK_THREAD_RET_NULL` / `CHECK_THREAD_RET_VOID`
  分散在头与实现两侧。**不要凭记忆判断谁定义在哪** —— 搬完直接编译，
  缺什么编译器会一条条报出来，按报错补。

- [ ] **2.3 改 `engine/include/sdk/impl/mmdb_memctx.h`**

  ```c
  /* 原：#include "db/sql/memctx.h" */
  #include "sdk/impl/memctx.h"
  ```

  同时把文件头注释里的 `与 \`db/sql/memctx.h\` 中的核心 API 一一对应`
  改成 `与 \`sdk/impl/memctx.h\` 中的核心 API 一一对应`。

- [ ] **2.4 改 5 个 SDK 测试的 include**

  这 5 个文件里有 `#include "db/sql/memctx.h"`（为了访问 `MemoryContextData` 字段）：

  - `engine/test/sdk/memory/memory_leak_detection_test.cpp`
  - `engine/test/sdk/memory/mmdb_request_scope_test.cpp`
  - `engine/test/sdk/memory/mmdb_root_context_test.cpp`
  - `engine/test/sdk/memory/sdk_core_memctx_test.cpp`
  - `engine/test/sdk/memory/sdk_vectors_memctx_test.cpp`

  逐个改成：

  ```cpp
  #include "sdk/impl/memctx.h"           /* MemoryContextData 字段访问 */
  ```

  ⚠️ `mmdb_memctx_test.cpp` 只 include `sdk/impl/mmdb_memctx.h`，**不用改**。

- [ ] **2.5 改 `engine/src/sdk/CMakeLists.txt`：加源文件 + 摘 `sql_engine`**

  ```cmake
  set(SDK_CORE_SOURCES
      ...
      core/mmdb_memctx.c  # Task 7：SDK 兼容层内存上下文
      core/memctx.c        # SDK 自持的 AllocSet 实现（脱离冻结内核）
  )
  ```

  ```cmake
  # 链接：sqlite3 是唯一外部依赖。
  # faiss_hnsw / mm_vector_index / sql_engine 三个目标原本都定义在
  # 冻结轨 engine/src/db/ 的 CMakeLists 里 —— 摘轨后会消失。
  # sql_engine 提供的是 memctx 符号，已由 core/memctx.c 本地提供（Task 2）。
  target_link_libraries(mmsdk PUBLIC sqlite3)
  ```

- [ ] **2.6 构建 + 验证（红灯 → 绿灯）**

  ```powershell
  cd C:\code\projects
  cmake -B build/engine -S engine -DBUILD_TESTING=ON
  cmake --build build/engine --parallel 4
  ctest --test-dir build/engine -R "memctx|leak|request_scope|root_context" --output-on-failure
  ```

  预期：6 个内存上下文用例（`mmdb_memctx_test` / `mmdb_root_context_test` /
  `mmdb_request_scope_test` / `sdk_core_memctx_test` / `sdk_vectors_memctx_test` /
  `memory_leak_detection_test`）全部 PASS。

  ⚠️ **若出现未定义符号**（`palloc` / `pfree` / `AllocSetContextCreate` 等），
  说明移植漏了实现 —— **把它补进 `engine/src/sdk/core/memctx.c`**。
  **不许**把 `sql_engine` 加回链接行充数（那是把冻结轨又拖回来）。

- [ ] **2.7 机械验收**

  ```powershell
  nm -g build/engine/src/sdk/libmmsdk.a 2>$null | Select-String 'palloc|pfree|AllocSetContextCreate|MemoryContextReset|MemoryContextDelete|MemoryContextCheckThread|mmdb_mem_register_resource|mmdb_mem_unregister_resource'
  ```

  预期：每个符号至少出现一次且标记为 `T`（已定义文本段）。**出现 `U` 就是没移干净。**

  ```powershell
  Select-String -Path engine/src/sdk/*/*.c,engine/include/sdk/*/*.h,engine/include/sdk/impl/*.h -Pattern 'db/sql/memctx|db/parser/sql/parsenodes'
  ```

  预期：**零命中**。

- [ ] **2.8 全量回归 + 提交**

  ```powershell
  ctest --test-dir build/engine --output-on-failure
  git add engine/include/sdk/impl/memctx.h engine/include/sdk/impl/mmdb_memctx.h `
          engine/src/sdk/core/memctx.c engine/src/sdk/CMakeLists.txt `
          engine/test/sdk/memory/
  git commit -m "feat(sdk): 内存上下文脱离冻结内核（memctx 移植进 SDK）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

  预期：全部用例通过；`git status` 里不应出现 `engine/src/db/` 的任何变化。

---

### Task 3：SDK 向量后端脱离冻结轨（HNSW → 精确扁平索引）

**Files**
- 新建：`engine/include/sdk/impl/index_flat.h`、`engine/src/sdk/vectors/index_flat.c`
- 修改：`engine/src/sdk/vectors/vectors.c`、`engine/include/sdk/impl/vectors.h`、`engine/include/sdk/impl/mmdb_internal.h`
- 修改：`engine/src/sdk/CMakeLists.txt`
- 新建：`engine/test/sdk/vectors/index_flat_test.cpp`、`engine/test/sdk/vectors/CMakeLists.txt`
- 删除：`engine/test/sdk/hnsw/`（2 个文件）

**Interfaces**
- 新增公开符号（SDK 内）：`mmdb_flat_index_create` / `mmdb_flat_index_drop` /
  `mmdb_flat_index_reserve` / `mmdb_flat_index_add` / `mmdb_flat_index_size` /
  `mmdb_flat_search_filtered`
- 新增类型：`mmdb_flat_index_t`、`mmdb_flat_filter_fn`
- 重命名（**实现级**，不碰公开头 `engine/include/sdk/mmdb_vectors.h`）：
  `mmdb_vectors_hnsw_rebuild` → `mmdb_vectors_index_rebuild`、
  `mmdb_vectors_hnsw_ensure` → `mmdb_vectors_index_ensure`、
  `mmdb_vectors_hnsw_free` → `mmdb_vectors_index_free`
- **公开 API 的签名与结构体布局不变**：`engine/include/sdk/mmdb_vectors.h` 的**声明部分**一个字都不改。
  ⚠️ **（复审修正）但「不改」不等于「一个字都不改」** ——
  设计 §5.3 明确要求：**B-7/B-8 的降级语义必须写进该头的注释与 `engine/docs/` 里，不能默默改**，
  §7 的风险表还要求写进 changelog。
  「HNSW 近似检索 → 精确暴力 KNN」是**行为变更**，必须让使用者看得见。

  ⇒ **本步做三件事**：

  1. **在 `engine/include/sdk/mmdb_vectors.h` 的注释里写明降级**（声明部分不动）：

     ```c
     /*
      * ⚠️ 检索实现已降级（2026-11-18，冻结轨退役）：
      *   原实现 = faiss HNSW（近似检索，复杂度约 O(log N)）
      *   现实现 = 扁平索引 + 精确暴力 KNN，复杂度 **O(N)**
      * 语义不变（返回最近的 k 个），**性能显著下降**。
      * 小规模（十万级以内）可接受；更大规模请另寻近似索引方案。
      */
     ```

  2. **在 `engine/docs/` 下写一条**（该目录已存在），
     建议文件名 `engine/docs/sdk-vector-search-downgrade.md`：
     为什么降级（冻结轨删除）、换成什么、复杂度变化、影响面。

  3. **写进 changelog**：`2026-11-18 冻结轨退役：SDK 向量检索由 HNSW 近似降级为精确 KNN`。

  ⚠️ **这三条是设计 §5.3 的硬要求**，不是可选项 —— 默默改行为而不同步文档，
  是本项目明令禁止的。
- **公开 API 不变**：`engine/include/sdk/mmdb_vectors.h` 一个字都不改；
  `engine/include/sdk/mmdb_metrics.h` 的 `hnsw_build_total` / `hnsw_build_time_ms`
  字段名与 `mmdb_metrics_record_hnsw_build()` **保留原名**（公开 API 稳定性优先），
  只在函数注释里说明它现在记录的是"内存索引构建耗时"。

**为什么是"精确"而不是"换个近似算法"**：HNSW 是近似最近邻，有召回率损失；
换成暴力精确 KNN，**召回率只升不降**，代价是延迟。这是"降级"里唯一不牺牲正确性的方向。

- [ ] **3.1 新建 `engine/include/sdk/impl/index_flat.h`**

  ```c
  #ifndef SDK_IMPL_INDEX_FLAT_H
  #define SDK_IMPL_INDEX_FLAT_H

  #include <stdint.h>

  #ifdef __cplusplus
  extern "C" {
  #endif

  /*
   * 扁平索引 —— SDK 自持的内存向量索引。
   *
   * 【为什么是"扁平"而不是 HNSW】
   * 原实现用的是冻结内核的 HNSW（faiss_hnsw），近似最近邻，有召回率损失。
   * 扁平索引是**精确**最近邻：召回率 100%，代价是 O(N) 扫描。
   * 对嵌入式 SDK 的典型规模（万级向量）这是可接受的；
   * 真要更大规模，应当引入**量化**（PQ/SQ），而不是回到近似。
   *
   * 【与原 API 的对应关系】
   * 这套接口刻意与原来调用的 faiss_hnsw_* 一一对应，
   * 便于机械替换、便于对照评审：
   *   faiss_hnsw_index_create(M, dims, ef, metric, quant) → mmdb_flat_index_create(dims)
   *   faiss_hnsw_index_reserve(idx, n)                  → mmdb_flat_index_reserve(idx, n)
   *   faiss_hnsw_index_add(idx, n, vectors)             → mmdb_flat_index_add(idx, n, vectors)
   *   faiss_hnsw_index_size(idx)                        → mmdb_flat_index_size(idx)
   *   faiss_hnsw_index_drop(idx)                        → mmdb_flat_index_drop(idx)
   *   faiss_hnsw_search_filtered(...)                   → mmdb_flat_search_filtered(...)
   */

  typedef struct mmdb_flat_index mmdb_flat_index_t;

  /* 过滤谓词：返回非 0 表示该候选通过。 */
  typedef int (*mmdb_flat_filter_fn)(int32_t vec_id, void *user_data);

  /* 建索引。dims = 向量维度。失败返回 NULL。 */
  mmdb_flat_index_t *mmdb_flat_index_create(int32_t dims);

  /* 释放索引。可传 NULL。 */
  void mmdb_flat_index_drop(mmdb_flat_index_t *idx);

  /* 预分配 n 条的容量（避免逐条 realloc）。0 成功 / -1 失败。 */
  int32_t mmdb_flat_index_reserve(mmdb_flat_index_t *idx, int32_t n);

  /*
   * 批量追加 n 条向量（行主序，长度 n*dims）。
   * 返回实际追加的条数；失败返回负数。
   */
  int32_t mmdb_flat_index_add(mmdb_flat_index_t *idx, int32_t n, const float *vectors);

  /* 已索引的向量条数。idx 为 NULL 返回 0。 */
  int32_t mmdb_flat_index_size(const mmdb_flat_index_t *idx);

  /*
   * 精确 KNN 搜索（带过滤）。
   *
   * 距离口径：**L2 平方距离**（与原 HNSW 的 DISTANCE_METRIC_L2_SQUARED
   * 以及 vectors.c 里 l2_distance_* 一致）。
   *
   * filter 为 NULL 表示不过滤。filter 返回 0 的候选被丢弃。
   * out_ids / out_distances 由调用方给（各 k 项）。
   * 输出按距离**升序**。
   *
   * 返回实际命中条数（<= k）；-1 表示参数非法。
   */
  int32_t mmdb_flat_search_filtered(mmdb_flat_index_t *idx,
                                    const float *query,
                                    int32_t k,
                                    mmdb_flat_filter_fn filter,
                                    void *user_data,
                                    int32_t *out_ids,
                                    float *out_distances);

  #ifdef __cplusplus
  }
  #endif

  #endif /* SDK_IMPL_INDEX_FLAT_H */
  ```

- [ ] **3.2 新建 `engine/src/sdk/vectors/index_flat.c`**

  替换原来 10 行的占位实现。核心结构与算法：

  ```c
  struct mmdb_flat_index {
      int32_t dims;
      int32_t n;       /* 已索引条数 */
      int32_t cap;     /* 容量 */
      float  *data;    /* cap * dims，行主序 */
  };
  ```

  - `create(dims)`：`calloc(1, sizeof(*idx))`，`idx->dims = dims`。
  - `reserve(idx, n)`：`realloc` 到 `n*dims` 个 float（溢出检查：`n > SIZE_MAX/(4*dims)` ⇒ 失败）。
  - `add(idx, n, vectors)`：`memcpy` 进 `data + n_used*dims`，`n += count`，返回 count。
  - `size(idx)`：返回 `idx->n`。
  - `drop(idx)`：`free(idx->data); free(idx);`（可传 NULL）。
  - `search_filtered(...)`：
    1. 遍历 `i ∈ [0, idx->n)`；
    2. 先过 filter（`filter && !filter(i, user_data)` ⇒ 跳过）；
    3. 算 L2 平方距离 `sum((data[i*d+j] - query[j])^2)`；
    4. 维护一个大小为 k 的**最大堆**（堆顶是当前最远的那个），满了之后只在更近时替换；
    5. 收尾把堆里 k 项按距离**升序**排好写进 `out_ids` / `out_distances`。

  ⚠️ 距离口径必须与 `vectors.c` 里已有的 `l2_distance_scalar` / `l2_distance_simd`
  **完全一致**（都是平方距离，不开根号）。两处实现的口径不一致 = 静默给错答案。

- [ ] **3.3 改 `engine/src/sdk/vectors/vectors.c`（9 个调用点 + 1 个类型 + 2 个 include）**

  | 位置 | 原来 | 改成 |
  |---|---|---|
  | L16 | `#include <db/index/vector_index/faiss_hnsw/faiss_hnsw.h>` | `#include "sdk/impl/index_flat.h"` |
  | L18 | `#include <db/index/vector_index/vector_index_selector.h>` | **删除**（决策逻辑内联，见 3.4） |
  | L388 | `faiss_hnsw_t* index;` | `mmdb_flat_index_t* index;` |
  | L427 | `faiss_hnsw_index_drop(w->index);` | `mmdb_flat_index_drop(w->index);` |
  | L610 | `faiss_hnsw_index_drop(w->index);` | `mmdb_flat_index_drop(w->index);` |
  | L662 | `faiss_hnsw_index_drop(w->index);` | `mmdb_flat_index_drop(w->index);` |
  | L675 | `faiss_hnsw_index_drop(w->index);` | `mmdb_flat_index_drop(w->index);` |
  | L647-649 | `faiss_hnsw_index_create(use_m, (int32_t)dim, use_ef, DISTANCE_METRIC_L2_SQUARED, QUANTIZATION_TYPE_NONE)` | `mmdb_flat_index_create((int32_t)dim)` |
  | L660 | `faiss_hnsw_index_reserve(w->index, (int32_t)row_idx)` | `mmdb_flat_index_reserve(w->index, (int32_t)row_idx)` |
  | L667 | `faiss_hnsw_index_add(w->index, (int32_t)row_idx, all_vectors)` | `mmdb_flat_index_add(w->index, (int32_t)row_idx, all_vectors)` |
  | L722 | `vector_index_selector_choose(&info, &decision)` | **删除**，见 3.4 |
  | L1117 | `faiss_hnsw_index_size(w->index) > 0` | `mmdb_flat_index_size(w->index) > 0` |
  | L1149 | `faiss_hnsw_search_filtered(w->index, (const float*)q->query_vector, search_k, has_filter ? hnsw_filter_predicate : NULL, has_filter ? (void*)&filter_ctx : NULL, hnsw_ids, distances)` | `mmdb_flat_search_filtered(w->index, (const float*)q->query_vector, search_k, has_filter ? hnsw_filter_predicate : NULL, has_filter ? (void*)&filter_ctx : NULL, hnsw_ids, distances)` |

  ⚠️ `faiss_hnsw_filter_fn` 与 `mmdb_flat_filter_fn` 签名一致
  （`int (*)(int32_t, void *)`），所以 `hnsw_filter_predicate` **不用改**。

- [ ] **3.4 把索引选择决策内联（替换 `vector_index_selector_choose`）**

  原选择器会在 HNSW / IVF-PQ / DiskANN / 暴力之间选。SDK 现在**只有一种索引**，
  决策退化成"建不建内存索引"：

  ```c
  /*
   * 内存索引的构建门槛：向量数低于此值时直接扫 SQLite，不建内存副本。
   *
   * 【为什么有这个门槛】
   * 内存索引 = 全部向量的一份连续内存副本。向量少时，
   * 复制一遍再扫一遍比直接扫 SQLite 更慢（多了一次拷贝）。
   *
   * 【原来的决策从哪来】
   * 冻结内核的 vector_index_selector_choose()（按规模/维度/内存/QPS/召回率
   * 在多种索引类型里选）。SDK 侧只剩一种索引，决策退化为"建不建"。
   */
  #define VEC_INDEX_BUILD_THRESHOLD 1000
  ```

  `mmdb_vectors_hnsw_ensure` 改名为 `mmdb_vectors_index_ensure`，函数体改成：

  ```c
  int mmdb_vectors_index_ensure(mmdb_collection_t* c) {
      if (!c || !c->has_vector) return MMDB_ERR_INVALID;

      /* 已有内存索引，直接返回 */
      if (c->vec_index) return MMDB_OK;

      /* 确保表存在 */
      if (mmdb_vectors_ensure_table(c) != MMDB_OK) return MMDB_ERR_IO;

      char tname[128];
      build_table_name(tname, sizeof(tname), c->name);

      /* 检查当前行数 */
      size_t count = vec_count_vectors(c, tname);
      if (count < VEC_INDEX_BUILD_THRESHOLD) return MMDB_OK;  /* 不值得建 */

      index_wrapper_t* w = index_wrapper_create();
      if (!w) return MMDB_ERR_NOMEM;
      c->vec_index = w;

      int rc = mmdb_vectors_index_rebuild(c);
      if (rc != MMDB_OK) {
          /* 构建失败，降级到直扫模式（不报错） */
          index_wrapper_free(w);
          c->vec_index = NULL;
          return MMDB_OK;
      }
      return MMDB_OK;
  }
  ```

  `mmdb_vectors_hnsw_rebuild` 改名为 `mmdb_vectors_index_rebuild`，
  **签名去掉两个参数**（`hnsw_m` / `hnsw_ef_c` 对扁平索引无意义）：

  ```c
  int mmdb_vectors_index_rebuild(mmdb_collection_t* c);
  ```

- [ ] **3.5 同步重命名（`hnsw` → `index`）**

  `engine/include/sdk/impl/vectors.h`：

  ```c
  int  mmdb_vectors_index_ensure(mmdb_collection_t* coll);
  int  mmdb_vectors_index_rebuild(mmdb_collection_t* coll);
  void mmdb_vectors_index_free(mmdb_collection_t* coll);
  ```

  `engine/include/sdk/impl/mmdb_internal.h:46`：

  ```c
  void* vec_index;          /* 内存向量索引指针（精确扁平索引） */
  ```

  ⚠️ 改字段名会牵出一串引用。**用 grep 驱动，不要凭记忆**：

  ```powershell
  Select-String -Path engine/src/sdk/*/*.c,engine/include/sdk/*.h,engine/include/sdk/impl/*.h `
    -Pattern 'hnsw|HNSW'
  ```

  逐条判断：
  - `hnsw_wrapper_t` → `index_wrapper_t`
  - `hnsw_id_map_*` / `sdk_id_hash_*` → 保留原名（它们与索引类型无关，是 ID 映射）
  - `hnsw_count_vectors` → `vec_count_vectors`
  - `hnsw_filter_predicate` / `hnsw_filter_ctx_t` → **保留原名**（它们是"过滤谓词"，
    与索引类型无关；改名属于"顺手优化相邻代码"）
  - `HNSW_DEFAULT_M` / `HNSW_DEFAULT_EF_C` → **删除**（扁平索引用不到）
  - `HNSW_BUILD_THRESHOLD` → `VEC_INDEX_BUILD_THRESHOLD`
  - `mmdb_metrics_record_hnsw_build` → **保留原名**（公开 API）

- [ ] **3.6 改 `engine/src/sdk/CMakeLists.txt`：摘 `faiss_hnsw` / `mm_vector_index` + 删 include 路径**

  ```cmake
  target_link_libraries(mmsdk PUBLIC sqlite3)
  ```

  ```cmake
  target_include_directories(mmsdk PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/../../include
      ${CMAKE_CURRENT_SOURCE_DIR}/../include
  )
  ```

  ⚠️ 删掉 `${CMAKE_CURRENT_SOURCE_DIR}/..`（它是为了 `<db/index/vector_index/faiss_hnsw/faiss_hnsw.h>` 加的）。
  删完必须全量构建通过 —— 如果还有文件靠这条路径找头，编译器会报出来。
  逐个按报错处理，**不许把这条路径加回来**。

- [ ] **3.7 删除 `engine/test/sdk/hnsw/`（2 个文件）**

  - `engine/test/sdk/hnsw/hnsw_filter_test.cpp` —— 测的是冻结轨的 `faiss_hnsw`
  - `engine/test/sdk/integration/selector_integration_test.cpp` —— 测的是冻结轨的 `vector_index_selector`

  并删掉 `engine/test/sdk/CMakeLists.txt:29` 的
  `target_link_libraries(hnsw_filter_test PRIVATE faiss_hnsw)` 与对应的
  `add_subdirectory(hnsw)`。

  ⚠️ 这两个文件测的功能**不是消失了，是换了实现** —— 3.8 用新测试补上等价覆盖。

- [ ] **3.8 新建 `engine/test/sdk/vectors/index_flat_test.cpp` + CMakeLists**

  覆盖 6 个接口 + 过滤语义：

  ```cpp
  #include <gtest/gtest.h>
  #include <cmath>
  #include <vector>

  #include "sdk/impl/index_flat.h"

  TEST(FlatIndex, CreateAndDrop) {
      auto* idx = mmdb_flat_index_create(4);
      ASSERT_NE(idx, nullptr);
      EXPECT_EQ(mmdb_flat_index_size(idx), 0);
      mmdb_flat_index_drop(idx);
      mmdb_flat_index_drop(nullptr);   /* 传 NULL 不崩 */
  }

  TEST(FlatIndex, ReserveAndAdd) {
      auto* idx = mmdb_flat_index_create(2);
      ASSERT_NE(idx, nullptr);
      ASSERT_EQ(mmdb_flat_index_reserve(idx, 3), 0);
      float v[6] = {0,0, 1,1, 2,2};
      EXPECT_EQ(mmdb_flat_index_add(idx, 3, v), 3);
      EXPECT_EQ(mmdb_flat_index_size(idx), 3);
      mmdb_flat_index_drop(idx);
  }

  TEST(FlatIndex, SearchReturnsNearestFirst) {
      auto* idx = mmdb_flat_index_create(2);
      ASSERT_NE(idx, nullptr);
      float v[6] = {0,0, 10,10, 1,1};
      mmdb_flat_index_add(idx, 3, v);
      float q[2] = {0, 0};
      int32_t ids[3];
      float dist[3];
      int32_t n = mmdb_flat_search_filtered(idx, q, 3, nullptr, nullptr, ids, dist);
      ASSERT_EQ(n, 3);
      EXPECT_EQ(ids[0], 0);
      EXPECT_EQ(ids[1], 2);
      EXPECT_EQ(ids[2], 1);
      EXPECT_NEAR(dist[0], 0.0f, 1e-6);
      EXPECT_NEAR(dist[1], 2.0f, 1e-6);   /* 1²+1² */
      EXPECT_NEAR(dist[2], 200.0f, 1e-6); /* 10²+10² */
      mmdb_flat_index_drop(idx);
  }

  /* 过滤：filter 返回 0 的候选被丢弃 */
  static int reject_even(int32_t vec_id, void*) { return (vec_id % 2) != 0; }

  TEST(FlatIndex, FilterExcludesCandidates) {
      auto* idx = mmdb_flat_index_create(2);
      ASSERT_NE(idx, nullptr);
      float v[6] = {0,0, 10,10, 1,1};
      mmdb_flat_index_add(idx, 3, v);
      float q[2] = {0, 0};
      int32_t ids[3];
      float dist[3];
      int32_t n = mmdb_flat_search_filtered(idx, q, 3, reject_even, nullptr, ids, dist);
      ASSERT_EQ(n, 2);
      EXPECT_EQ(ids[0], 0);
      EXPECT_EQ(ids[1], 2);
      mmdb_flat_index_drop(idx);
  }
  ```

  `engine/test/sdk/vectors/CMakeLists.txt`：

  ```cmake
  # SDK 内存向量索引（精确扁平索引）测试
  add_project_test(index_flat_test VAR
      SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/index_flat_test.cpp)
  target_link_libraries(index_flat_test PRIVATE mmsdk project_includes)
  ```

  并在 `engine/test/sdk/CMakeLists.txt` 加 `add_subdirectory(vectors)`。

- [ ] **3.9 构建 + 验证**

  ```powershell
  cmake -B build/engine -S engine -DBUILD_TESTING=ON
  cmake --build build/engine --parallel 4
  ctest --test-dir build/engine -R "flat|vector|sdk" --output-on-failure
  ```

  预期：`index_flat_test` PASS；SDK 全部用例 PASS。

- [ ] **3.10 机械验收 + 全量回归 + 提交**

  ```powershell
  Select-String -Path engine/src/sdk/*/*.c,engine/include/sdk/*.h,engine/include/sdk/impl/*.h `
    -Pattern 'faiss_hnsw|vector_index_selector|db/index/vector_index'
  ```

  预期：**零命中**。

  ```powershell
  ctest --test-dir build/engine --output-on-failure
  git add engine/include/sdk/impl/index_flat.h engine/include/sdk/impl/vectors.h `
          engine/include/sdk/impl/mmdb_internal.h engine/src/sdk/vectors/ `
          engine/src/sdk/CMakeLists.txt engine/test/sdk/
  git add -u engine/test/sdk/hnsw 2>$null
  git commit -m "feat(sdk): 向量索引脱离冻结内核（HNSW 换成精确扁平索引）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

  ⚠️ **确认用例数没掉**：本 Task 只新增 1 个用例（`index_flat_test`）、
  删 2 个（`hnsw_filter_test` / `selector_integration_test`）。
  与 Task 1 的 N 比对，差值应为 **−1**。

---

### Task 4：摘掉冻结轨（一个原子提交）
  ⚠️ **（复审修正）本 Task 有一处与设计文档的偏离，必须记录在案**：

  | 项 | 设计文档 | 本计划实测 | 处置 |
  |---|---|---|---|
  | `engine/test/index/` 删几个 | D-10 写 **4 个目录**（`brin` / `bitmap` / `gin` / `diskann`） | **8 个目录全部**都 include `<index/...>`，而 `index/` 目录随冻结轨一起没了 ⇒ 留任何一个都编不过 | **按 8 个删**；设计 D-10 的「4 个」是错的，回填订正 |
  | `engine/test/db/` | 289 文件 | **294** 个 `.c/.cpp/.h`（357 个含其它类型） | 以实测为准，设计数字回填订正 |

  ⇒ 这两条**必须同步回设计文档**，不能只在计划里改 —— 否则设计与实施长期不一致。

**Files**
- 删除：见「文件结构 › 删除」全部
- 修改：`engine/src/CMakeLists.txt`、`engine/test/CMakeLists.txt`、`engine/CMakeLists.txt`、`engine/test/sdk/CMakeLists.txt`

**Interfaces**
- 删除的 CMake 目标：`sql_engine`、`faiss_hnsw`、`mm_vector_index`（三个都定义在冻结轨里）
- 删除的 CMake 选项：`ENABLE_FROZEN_SRC_DB_TESTS`
- 保留：`sqlite3`、`mmsdk`、`project_includes`、`multimodal_config_gen` ← **注意**：后者由
  `engine/cmake/MultimodalConfig.cmake` 生成，而那个文件在删除清单里（D-5）——
  **必须同时处理**（见 4.3）。

**⚠️ 这是本计划风险最高的一个 Task：删了就得同时把构建接线修好，不允许出现"删了但 build 不过"的提交。**

#### 与设计文档的偏离（复审补入，必须回填设计文档）



- [ ] **4.1 删除前最后一次确认主核是绿的**

  ```powershell
  cd C:\code\projects\engine\db
  mingw32-make clean && mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：95 通过 / 0 失败。

- [ ] **4.2 删目录**

  ```powershell
  cd C:\code\projects
  git rm -r --quiet engine/src/db engine/test/db engine/include/db `
                      engine/include/rag engine/include/tree_index `
                      engine/test/smoke engine/test/index `
                      engine/tools/benchmark/c_api
  Remove-Item -Recurse -Force engine/cmake/ModalityProfiles.cmake, engine/cmake/MultimodalConfig.cmake
  ```

  ⚠️ **`engine/db/` 不在删除清单里** —— 它是主核。删完必须确认它还在：

  ```powershell
  Test-Path engine/db/Makefile    # 必须是 True
  ```

- [ ] **4.3 修 `engine/CMakeLists.txt`**

  删掉：

  ```cmake
  # ========================================================================
  # 模态开关：按 MMDB_MODALITY 推导全部 MMDB_ENABLE_* 缓存变量
  # ========================================================================
  include("${CMAKE_CURRENT_LIST_DIR}/cmake/ModalityProfiles.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/cmake/MultimodalConfig.cmake")
  ```

  以及：

  ```cmake
  option(ENABLE_FROZEN_SRC_DB_TESTS "Build/register tests for frozen engine/src/db" OFF)
  ```

  以及：

  ```cmake
  # 生成配置头全局可见（模态开关宏的唯一来源）
  target_link_libraries(project_includes INTERFACE multimodal_config_gen)
  ```

  ⚠️ `multimodal_config_gen` 是 `MultimodalConfig.cmake` 造出来的目标。
  删了那个 cmake 文件就必须同时删这条链接 —— 否则 configure 阶段报
  "Target links to target multimodal_config_gen that does not exist"。

  ⚠️ 删完之后要确认**没有别的目标依赖 `multimodal_config_gen`**：

  ```powershell
  Select-String -Path engine/**/CMakeLists.txt,engine/CMakeLists.txt -Pattern 'multimodal_config_gen|MMDB_ENABLE_|ModalityProfiles'
  ```

  预期：零命中。

- [ ] **4.4 修 `engine/src/CMakeLists.txt`**

  删掉 `add_subdirectory(db)`（L21）。

- [ ] **4.5 修 `engine/test/CMakeLists.txt`**

  删掉：

  ```cmake
  if(ENABLE_FROZEN_SRC_DB_TESTS)
      add_subdirectory(db)
      add_subdirectory(db/consensus)   # Raft 共识模块测试（Phase11）
      # add_subdirectory(db/index/vector_index)  # 向量索引测试（暂时跳过，add_subdirectory 冲突）
  endif()
  ```

  删掉：

  ```cmake
  add_subdirectory(smoke)           # 模态独立冒烟（Task 7 起，每模态一个）
  ```

  ⚠️ `add_subdirectory(kbase)`（L32）：`engine/test/kbase/CMakeLists.txt` 的
  L4~L43 **全部是注释掉的**，零注册目标。删掉目录还是保留？
  **保留引用但删掉这行引用**更干净 —— 因为 kbase 目录里还有 `.cpp` 源文件，
  将来要用可以重新挂上。本次只删 `add_subdirectory(kbase)` 这一行。

  清理 `_test_targets`（L46-82）里的冻结轨目标名：

  ```cmake
  set(_test_targets
      self_made_cpp_test
      test_hilbert
      mmdb_core_test
      mmdb_vectors_test
      mmdb_graph_test
      mmdb_timeseries_test
      mmdb_text_test
      mmdb_filter_test
      cross_lang_consistency_test
      vectorized_exec_test
  )
  ```

  删掉的：`raft_test`、`test_db`、`test_mvcc_test`、`test_optimizer_test`、
  `test_replication_test`、`test_sql`、`test_storage_all`、`test_concurrency`、
  `test_mm_pool`、`test_mvcc`、`test_optimizer`、`test_replication`、`test_rest_api`、
  `test_sla`、`test_sql_parser`、`test_storage_kv`、`test_storage_kv_ttl`、
  `test_storage_main`、`test_storage_wal`、`test_vector_api`、`test_vector_persist`、
  `test_vector_query`、`vector_delete_test`、`vector_reranker_test`、
  `distributed_txn_test`。

  ⚠️ **逐个核对**：`foreach` 里的 `if(TARGET ${_t})` 是**静默跳过** ——
  目标不存在时不报错，只是没挂上。所以删多了不会立刻暴露。
  **必须逐个用 `ctest -N` 对照**：Task 1 的基线里记了每个用例的名字。

- [ ] **4.6 修 `engine/test/sdk/CMakeLists.txt`**

  确认 `add_subdirectory(hnsw)` 已随 Task 3 删掉。

- [ ] **4.7 删 `engine/tools/benchmark/CMakeLists.txt` 的 c_api 引用**

  ```cmake
  # VectorBench CMakeLists.txt
  # VectorBench 基准测试框架

  # add_subdirectory(c_api)   # 已随冻结轨退役删除（依赖 mm_vector_index / faiss_hnsw）
  ```

  ⚠️ `engine/tools/benchmark/` 下若只剩空的 c_api 目录，一并删掉。

- [ ] **4.8 配置期 + 全量构建**

  ```powershell
  cd C:\code\projects
  Remove-Item -Recurse -Force build/engine -ErrorAction SilentlyContinue
  cmake -B build/engine -S engine -DBUILD_TESTING=ON
  cmake --build build/engine --parallel 4
  ```

  预期：configure 与 build **零 error**。

  ⚠️ **必须删掉整个 `build/engine` 重建** —— CMakeCache 里还留着冻结轨的路径，
  增量构建会掩盖问题。

- [ ] **4.9 全量 ctest + 用例数核对**

  ```powershell
  ctest --test-dir build/engine --output-on-failure
  ctest --test-dir build/engine -N | Select-String 'Total Tests'
  ```

  预期：**用例数 == Task 1 基线里的 N**（不多不少）。
  若多了 → 有冻结轨的用例残留（某个 target 没删干净）。
  若少了 → 误删了不该删的。

- [ ] **4.10 主核仍然是绿的**

  ```powershell
  cd C:\code\projects\engine\db
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  mingw32-make clean && mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：`✓ 依赖方向正确`；95 通过 / 0 失败。

- [ ] **4.11 机械验收：冻结轨真的没了**

  ```powershell
  cd C:\code\projects
  Test-Path engine/src/db          # 必须是 False
  Test-Path engine/test/db         # 必须是 False
  Test-Path engine/include/db      # 必须是 False
  Test-Path engine/db/Makefile     # 必须是 True
  Select-String -Path engine/CMakeLists.txt,engine/src/CMakeLists.txt,engine/test/CMakeLists.txt `
    -Pattern 'ENABLE_FROZEN_SRC_DB_TESTS|sql_engine|faiss_hnsw|mm_vector_index|multimodal_config_gen'
  ```

  预期：三条 `Test-Path` 符合预期；`Select-String` **零命中**。

- [ ] **4.12 提交（一个原子提交）**

  ```powershell
  cd C:\code\projects
  git add -A engine/
  git status --short | Select-Object -First 20
  git commit -m "refactor(冻结轨): 退役 engine/src/db 主线，SDK 与工程轨完成去冻结化"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

  ⚠️ 提交前 `git status --short` 里**必须没有**：
  `apps/study-hub/**`、`StillEvolving/`、根目录的 `b.txt` 等越界文件。

---

### Task 5：文档处置

**Files**
- 修改：`engine/CMakeLists.txt`（注释里的冻结轨说明）
- 修改：`CLAUDE.md`（父仓根，「数据库存储引擎」章节整段描述的是冻结轨）
- 修改：`AGENTS.md`（父仓根，「子项目规则 › 主数据库内核」里的冻结轨说明）
- 修改：`docs/storage-architecture.md`（若存在对冻结轨的描述）
- 修改：`engine/db/README.md`（若有指向冻结轨的引用）

**Interfaces**
- 无代码接口变更。

**为什么单独一个 Task**：删轨是"行为变更"，文档是"契约变更"。混在一起评审会看不清。

- [ ] **5.1 找出所有还指向冻结轨的文档表述**

  ```powershell
  cd C:\code\projects
  Select-String -Path CLAUDE.md,AGENTS.md,README.md,docs/*.md,docs/*/*.md,engine/db/README.md `
    -Pattern 'engine/src/db|冻结内核|冻结轨|PostgreSQL 风格|pg_wire|mysql_wire|initdb|pg_ctl' |
    Select-Object Path, LineNumber, Line
  ```

- [ ] **5.1b 逐类落点表（设计 §5.4 的八类，一个都不能漏）**

  ⚠️ **（复审修正）原计划只把 `CLAUDE.md` / `AGENTS.md` 落到了步，
  设计 §5.4 列的另外六类只有"搜一下"、没有"改"的落点** ——
  做完后仍会留下 869 + 32 + 8 + 3 处死链。

  | # | 目标 | 处数 | 动作 | 本计划落点 |
  |---|---|---|---|---|
  | 1 | `docs/analysis/` | **869** | **整批归档**：每篇顶部加横幅「本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考」，文件 `git mv` 进 `docs/analysis/archive/` | 5.1b.1 |
  | 2 | `docs/arch/` | **32** | 逐条改：改成主核 `engine/db/` 的说法 | 5.1b.2 |
  | 3 | `docs/storage/` | **8** | 逐条改 | 5.1b.2 |
  | 4 | `openspec/specs/` | **3** | 逐条改/删 | 5.1b.2 |
  | 5 | 仓级 `README.md` | — | 定位表 / 核心能力 §1–§4 / 结构树 / 测试覆盖表 / 踩坑红线 **五处** | 5.1b.3 |
  | 6 | `CLAUDE.md` | — | 五节整段重写 | 5.2 |
  | 7 | `AGENTS.md` | — | 库清单 | 5.3 |
  | 8 | `engine/README.md` | — | 同仓级 README | 5.1b.3 |
  | 9 | `docs/memory-context-*.md` ×3 | — | ⚠️ **（复审补入）** 这三份讲的是**冻结轨的 memctx**，memctx 移植后必然过期 | 5.1b.2 |

  > **复审补入的第 9 类**：设计 §5.4 没列这三份，但它们讲的就是 memctx ——
  > 移植之后「冻结内核的内存上下文」这套说法全是错的。顺带说明：
  > 冻结轨的 `include/db/sql/memctx.h` 本身就要删，这三份文档不跟着改就是死链。

- [ ] **5.1b.1 归档 `docs/analysis/`（27 篇 / 869 处引用）**

  ```powershell
  cd C:\code\projects
  New-Item -ItemType Directory -Force docs/analysis/archive | Out-Null
  Get-ChildItem docs/analysis -File -Filter '*.md' |
    Where-Object { $_.DirectoryName -notlike '*archive*' } |
    ForEach-Object {
      # ① 加横幅（幂等：已有横幅就跳过）
      $c = Get-Content $_.FullName -Raw -Encoding UTF8
      if ($c -notmatch '本文分析对象') {
        $banner = "> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。`n> 当前主核是 `engine/db/`，见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。`n`n"
        Set-Content -Path $_.FullName -Value ($banner + $c) -Encoding UTF8 -NoNewline
      }
      # ② 移入 archive/
      git mv $_.FullName (Join-Path (Resolve-Path docs/analysis/archive) $_.Name)
    }
  ```

  ⚠️ **`git mv` 保留历史** —— 这批文档是历史分析，删掉不如归档。

- [ ] **5.1b.2 逐条改 `docs/arch/` / `docs/storage/` / `openspec/specs/` / `docs/memory-context-*.md`**

  ```powershell
  cd C:\code\projects
  Select-String -Path docs/arch/*.md,docs/storage/*.md,openspec/specs/*.md,docs/memory-context-*.md `
    -Pattern 'src/db/|include/db/|db/sql/memctx|冻结内核'
  ```

  ⚠️ **`docs/storage-architecture.md` 这个路径不存在** ——（复审修正）
  实际目录是 `docs/storage/`，CLAUDE.md 里链接的那个文件也要一起改。
  逐条改的规则：
  - `engine/src/db/...` → `engine/db/...`（并核对新路径真的存在）
  - `engine/include/db/...` → 对应主核头
  - `engine/test/db/...` → 主核测试目录
  - 「冻结内核」「PostgreSQL 风格内核」等说法 → 改成「已退役，仅作历史参考」

- [ ] **5.1b.3 改仓级 `README.md` 与 `engine/README.md`**

  两处都要改这五项：
  1. **定位表**（仓库是干什么的）
  2. **核心能力 §1–§4**
  3. **结构树**（`engine/src/db/` 整棵消失）
  4. **测试覆盖表**（含 912 处 `src/db/` 引用里的一部分）
  5. **踩坑红线**（冻结轨相关的红线条目）

  ```powershell
  cd C:\code\projects
  Select-String -Path README.md,engine/README.md -Pattern 'src/db/|include/db/|冻结内核|engine/test/db'
  ```

  ⚠️ **`engine/README.md` 原来根本没进修改清单**（复审修正）——
  它与仓级 README 讲的是同一套事，不改就是两套矛盾的说法。

- [ ] **5.2 改 `CLAUDE.md`**
- [ ] **5.2 改 `CLAUDE.md`**

  把「数据库存储引擎」整章（从「冻结内核参考」那行到「API 使用示例」结束）替换为：

  ```markdown
  ### 数据库存储引擎

  > **本仓只有一套数据库内核**：`engine/db/`（从零手写的多模态数据库内核）。
  > 它自带 `Makefile`，**不在 CMake 构建图内**（根 `CMakeLists.txt` 只引入 `engine/`，
  > `engine/CMakeLists.txt` 只引入 `src/` `tools/` `test/`，均不含 `db/`）——
  > 这是有意的例外，因为它有自己独立的分层门禁、编译门禁与测试协议。
  >
  > 原先并列的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，
  > 源码、公开头、测试与构建接线全部删除。冻结期内它只是机制研究与历史参考；
  > 现在那个位置**没有内核**，不要按旧文档去找它。

  主核的分层、纪律与构建命令见「子项目规则 › 主数据库内核（`engine/db/`）」。
  ```

  ⚠️ 下面那段「多模态存储引擎」的表格（kv / vector / timeseries / doc / spatial / graph / yang）
  描述的是**冻结轨的 API**（`engine/include/db/mm_storage.h`）—— 那个头已经删了。
  **整段删掉**，不要改写成"待实现"，因为主核的模态 API 形态不同（`db_<模态>_<动作>`）。

  ⚠️ 「GUC 配置系统」「initdb / pg_ctl / db_server」「PostgreSQL Wire 协议」三节
  全部是冻结轨的东西 —— **整段删**。

- [ ] **5.3 改 `AGENTS.md`**

  「子项目规则 › 主数据库内核」里那句：

  > 并列的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已**冻结保留**：目录、源码、CMake、测试与设计文档原位保存，仅作机制研究与历史参考。

  改成：

  > 并列的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**（源码、公开头、测试、构建接线全部删除）。**本仓现在只有一个数据库内核** —— `engine/db/`。

- [ ] **5.4 改 `engine/CMakeLists.txt` 里的注释**

  删掉 `ENABLE_FROZEN_SRC_DB_TESTS` 那段说明（选项本身在 Task 4.3 已删）。

- [ ] **5.5 检查 `engine/db/` 内的文档有没有指向冻结轨**

  ```powershell
  Select-String -Path engine/db/*.md,engine/db/docs/*.md -Pattern 'engine/src/db'
  ```

  有命中的就改掉（改成"已退役"或直接删掉该句）。

- [ ] **5.6 订正仓库文档里主核的过期数字**（**计划编写期补入**）

  三处数字都是**实测已过期**的，而它们恰好是"验收基线"性质的描述 —— 错了会让人对不上账：

  | 位置 | 现状 | 实测 | 改成 |
  |---|---|---|---|
  | `CLAUDE.md`「主数据库内核」节的构建命令注释 | `sh test/run.sh  # 55 个测试文件，73 处断言` | **95** 个测试文件（`test/unit/` 64 + `test/integration/` 31） | `# 95 个测试文件` |
  | `CLAUDE.md` 同节的「单文件 ≤ 800 行」条 | 「既有超限文件 `src/storage/heap.c`、`src/db.c`、`src/storage/pager.c` 记在 v1.3 账上」 | 三个文件实测 **614 / 766 / 795** 行，**都已 ≤ 800** | 删掉这个「既有超限文件」从句（或改成「v1.3 账上的历史超限已全部消除」） |
  | `engine/db/test/run.sh:15` 头部注释 | 「实测 93 个测试里没有任何两个共用同一个 .db 路径」 | **95** 个 | 改成 95 |

  ⚠️ **`run.sh` 那行注释是并发安全性的证据**（"各测试互不干扰"）——
  数字错一点不影响结论，但复核者会拿它跟 `run.sh` 的实际收集数对账，对不上就会怀疑别的。

  ```powershell
  cd C:\code\projects
  Select-String -Path CLAUDE.md -Pattern '55 个测试文件|既有超限文件'
  Select-String -Path engine/db/test/run.sh -Pattern '93 个测试'
  ```

- [ ] **5.7 提交**

  ```powershell
  git add CLAUDE.md AGENTS.md engine/CMakeLists.txt docs/ engine/db/
  git commit -m "docs: 冻结轨退役后的文档同步（引擎章节改为只描述主核，并订正主核的测试数与超限清单）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

### Task 6：验收（§5.5 全套）

**Files**
- 新建：`docs/superpowers/plans/2026-11-18-frozen-track-retirement-acceptance.md`

**Interfaces**
- 无代码变更。

- [ ] **6.1 A1：冻结轨目录全部消失，主核完整**
  ⚠️ **（复审修正）grep 的范围要按设计 §5.5 第 1 条的完整口径** ——
  原计划只查了 3 个 `CMakeLists.txt`，查不到散落在 `.md` / `.cmake` / `.py` 里的引用。

  ```powershell
  cd C:\code\projects
  $dirs = 'engine/src','engine/include','engine/test','engine/tools','engine/cmake'
  foreach ($d in $dirs) {
    Select-String -Path "$d/*" -Recurse -Include '*.c','*.h','*.cpp','*.cmake','CMakeLists.txt' `
      -Pattern 'src/db/|include/db/' -ErrorAction SilentlyContinue
  }
  ```

  预期：**零命中**（`engine/db/` 目录本身除外 —— 它是主核，不是冻结轨）。



  ```powershell
  cd C:\code\projects
  foreach ($p in 'engine/src/db','engine/test/db','engine/include/db','engine/include/rag',
                 'engine/include/tree_index','engine/test/smoke','engine/test/index',
                 'engine/tools/benchmark/c_api','engine/cmake/ModalityProfiles.cmake',
                 'engine/cmake/MultimodalConfig.cmake') {
      "{0,-45} {1}" -f $p, (Test-Path $p)
  }
  Test-Path engine/db/Makefile
  ```

  预期：前 10 行全是 `False`，最后一行是 `True`。

- [ ] **6.2 A2：工程轨从零构建 + 全量测试绿**

  ```powershell
  Remove-Item -Recurse -Force build/engine -ErrorAction SilentlyContinue
  cmake -B build/engine -S engine -DBUILD_TESTING=ON
  cmake --build build/engine --parallel 4
  ctest --test-dir build/engine --output-on-failure
  ```

  预期：0 failed。

- [ ] **6.3 A3：失败用例名集合不新增 + 主核 95 个测试全绿**

  ```powershell
  ctest --test-dir build/engine -N | Select-String 'Total Tests'
  ```

  与 Task 1.2 的 N 比对。⚠️ 注意 Task 3 加了 1 个（`index_flat_test`）、
  删了 2 个 → **N − 1**。

- [ ] **6.4 A4：SDK 不再有任何冻结轨依赖**

  ```powershell
  Select-String -Path engine/src/sdk/*/*.c,engine/include/sdk/*.h,engine/include/sdk/impl/*.h `
    -Pattern 'db/sql/|db/parser/|db/index/|faiss_hnsw|vector_index_selector|sql_engine'
  ```

  预期：**零命中**。

- [ ] **6.5 A5：主核零变化**

  ```powershell
  cd C:\code\projects
  git diff --stat <Task1 的提交>..HEAD -- engine/db/
  ```

  预期：**空**（主核一行都没动）。

- [ ] **6.6 A6：主核构建与测试仍然绿**

  ```powershell
  cd C:\code\projects\engine\db
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  mingw32-make clean && mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：`✓ 依赖方向正确`；95 通过 / 0 失败。

- [ ] **6.7 A7：SDK 内存上下文 6 个用例全绿**

  ```powershell
  ctest --test-dir build/engine -R "memctx|leak|request_scope|root_context|vectors_memctx" --output-on-failure
  ```

  预期：6 个用例全 PASS。

- [ ] **6.8 A8：写验收报告**

  `docs/superpowers/plans/2026-11-18-frozen-track-retirement-acceptance.md`：

  ```markdown
  # 冻结轨退役验收报告（2026-11-18）

  | # | 验收项 | 判据 | 实测 | 结论 |
  |---|---|---|---|---|
  | A1 | 冻结轨目录消失 / 主核完整 | 10 个路径全 False，`engine/db/Makefile` 为 True | | |
  | A2 | 工程轨从零构建 + 全测绿 | 0 failed | | |
| A3 | 失败用例名集合 ⊆ 基线集合（不新增） | | | |
  | A4 | SDK 无冻结轨引用 | grep 零命中 | | |
  | A5 | 主核零变化 | `git diff --stat` 空 | | |
  | A6 | 主核构建 + 95 测试绿 | ✓ 依赖方向正确 | | |
  | A7 | SDK 内存上下文 6 用例 | 全 PASS | | |
  | A8 | 依赖方向门禁 | `check_deps.sh` ✓ | | |
| **A9** | **引用校验门禁** | `python scripts/check-wiki-refs.py` 退出码 0 | | |
| **A10** | **SDK 无冻结轨符号残留**（`nm -u`） | `nm -u libmmsdk.a \| grep -Ei 'faiss\|hnsw\|sql_engine\|memctx'` **零命中** | | |

> **（复审补入）A9 / A10** —— 设计 §5.5 的验收判据有 5 条，
> 原计划的 A1–A8 只覆盖了 4 条半，漏了「引用校验门禁」这一条，
> 并且 `nm` 只查了 memctx 符号的 T/U，**没查 `faiss` / `hnsw` / `sql_engine` 的未定义引用**。

> A10 的命令：
> ```bash
> nm -u build/engine/src/sdk/libmmsdk.a | grep -Ei 'faiss|hnsw|sql_engine|memctx'
> ```
> 预期：**零命中**。命中 `faiss` / `hnsw` / `sql_engine` ⇒ 还有冻结轨的符号没摘干净。

  ## 删除量
  | 项 | 行数 |
  |---|---|
  | engine/src/db/ | 209,907 |
  | engine/include/db/ | 409 头 |
  | engine/test/db/ | 289 文件 / ~587 用例 |
  ```

- [ ] **6.9 提交 + 父仓指针**

  ```powershell
  cd C:\code\projects
  git add docs/superpowers/plans/2026-11-18-frozen-track-retirement-acceptance.md
  git commit -m "docs(冻结轨): 补退役验收报告"
  git push origin main 2>&1 | Out-String | Write-Output

  cd C:\code
  git add projects
  git commit -m "chore(submodule): 更新 projects 指针（冻结轨退役）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

## 交付说明（执行完本计划后写给用户）

必须包含（见 `CLAUDE.md` 任务交付铁律）：

1. **完成了什么** —— 冻结轨退役的具体范围、SDK 自持的两个替代实现。
2. **变更范围** —— 新增/修改/删除的文件清单与规模。
3. **验证方式** —— A1~A8 每一项的实测数字（不是"应该没问题"）。
4. **后续建议** —— 例如：主核的 `MM=` 编译期裁剪（W-B）、八模态对称化（W-C）尚未开始；
   根目录越界文件仍未清理。

## Self-Review 记录

**Spec 覆盖**：§5.1 删除清单 → Task 3（先摘 SDK 依赖）+ Task 4（一个原子提交删轨）；
§5.2 保留清单 → Task 4 的 Interfaces 段；§5.3 必须同步修的构建点 → Task 4（4.1~4.6）；
§5.4 文档处置（912 处引用） → Task 5 的 5.1b 逐类落点表（**9 类**，含设计未列的
`docs/memory-context-*.md` 三份）；§5.5 退役验收判据 → Task 6（A1~A10）；
§5.6 SDK 内存上下文去冻结化 → Task 2（memctx 移植到 `engine/include/sdk/impl/memctx.h` +
`engine/src/sdk/core/memctx.c`）；§7 风险 → Task 1.1b（`pre-frozen-track-removal` tag）、
Task 1.2（ctest **失败用例名集合**基线）、Task 3（SDK 检索降级说明三件套）；
§9 交付物 → 全部落地。

**类型一致性**：`memctx.h` 的落点 `engine/include/sdk/impl/memctx.h` 与设计 §5.6 方案 A 一致；
`mmdb_vectors_hnsw_*` → `mmdb_vectors_index_*` 是**实现级**重命名，
公开头 `engine/include/sdk/mmdb_vectors.h` 的**声明**不动（只加注释，见 Task 3）；
八个 memctx 符号（`palloc` / `pfree` / `AllocSetContextCreate` / `MemoryContextReset` /
`MemoryContextDelete` / `MemoryContextCheckThread` / `mmdb_mem_register_resource` /
`mmdb_mem_unregister_resource`）在 R1 判据、Task 2 的 `nm -g` 命令、附录里三处写法一致；
CMake 目标的「删三个 / 留四个」（`sql_engine` / `faiss_hnsw` / `mm_vector_index` 删，
`sqlite3` / `mmsdk` / `project_includes` / `multimodal_config_gen` 留）在
§5.1、Task 4 的 Interfaces 段、验收表里三处一致 —— 名字无漂移。

**比例**：本计划 1416 行 / 设计文档 1192 行 ≈ 1.2:1。
其中约 40% 是两轮复审的「（复审修正）」批注 —— 它们是**要照着做的指令**，
不是背景叙述，删掉会导致执行者踩坑。
**复审来源**：本计划经过两轮独立 subagent 复审 ——
① 规格覆盖复审（对照设计 §1~§9 逐条查落点）与 ② 跨计划一致性复审
（31 个交叉点、契约冲突、文件归属冲突、顺序矛盾、数字口径）。
两轮的 blocking 级发现**已全部并入本计划正文**，每处均以
「⚠️ **（复审修正）**」标出，并写明**为什么错**与**改成什么** ——
这样执行者遇到分歧时能就地判断，而不是回头查聊天记录。
