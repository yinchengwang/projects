# 冻结轨退役验收报告（2026-11-18）

> W-A 计划的 Task 6 产出。计划原文的 A1–A8 已按**实际执行结果**重排为 A1–A11，
> 每一条都附实测值与取证命令。**没有任何一条是「应该没问题」。**

## 一、验收结果总表

| # | 验收项 | 判据 | 实测 | 结论 |
|---|---|---|---|---|
| A1 | 冻结轨目录与构建助手消失 | 15 个路径全 `False` | **15/15 消失** | ✅ |
| A1b | 代码与 CMake 里无冻结轨路径 | 零命中 | **0 处** | ✅ |
| A2 | 工程轨从零构建 | configure + build 零 error | **146 步，零 error** | ✅ |
| A3a | ctest 用例数 | 76（可算术推导） | **76** | ✅ |
| A3b | 失败用例名集合不新增 | ⊆ Task 1 基线集合 | **失败 11 项，新增 0 项** | ✅ |
| A4 | SDK 无冻结轨引用 | grep 零命中 | **0 处** | ✅ |
| A5 | 主核代码零变化 | `git diff --stat` 空 | **代码 diff 空**（仅文档有改动，Task 5 有意为之） | ✅ |
| A6 | 主核门禁仍绿 | `check_deps.sh` ✓ + 105/105 | **✓ 依赖方向正确；105 通过 / 0 失败** | ✅ |
| A7 | SDK 内存上下文用例 | 全 PASS | **44/44 通过（6 个测试二进制）** | ✅ |
| A8 | 主核完整 | `engine/db/Makefile` 存在 | **True** | ✅ |
| A9 | 引用校验门禁 | 退出码 0 | **退出码 1**（基线即失败，见 §三） | ⚠️ 不属本次 |
| A10 | SDK 无冻结轨符号残留 | `nm` 零命中 + 自持符号全 `T` | **冻结轨嫌疑 0；23 个自持符号全 T** | ✅ |
| A11 | 构建机检（`check_deps` 之外） | — | 见 A6 | ✅ |

**11 项中 10 项通过，1 项（A9）为基线遗留、不在本次退役范围。**

## 二、删除量

Task 4 的一个原子提交（`562808f31`）：

| 目录 | 文件 | 行数 |
|---|---|---|
| `engine/src/db/` | 676 | 216,629 |
| `engine/test/db/` | 357 | 94,551 |
| `engine/include/db/` | 409 | 83,125 |
| `engine/test/index/` | 15 | 2,255 |
| `engine/include/rag/` + `tree_index/` | 10 | 1,028 |
| `engine/tools/benchmark/c_api/` | 5 | 708 |
| `engine/cmake/` 旧轨助手（7 个） | 7 | 488 |
| `engine/test/smoke/` | 15 | 482 |
| **合计** | **1,494** | **399,266** |

`git show --stat` 的官方口径：**1,498 files changed, 2 insertions(+), 399,336 deletions(-)**
（差 4 个文件是本提交同时修改的 4 个 `CMakeLists.txt`）。

## 三、两条必须记下来的「非本次结论」

### A9：`scripts/check-wiki-refs.py` 失败是基线遗留

```
FAIL docs/db-wiki/embedded/boltdb.md: 文件不存在
… 共 10 项
```

**取证**：`git ls-tree pre-frozen-track-removal:docs` 输出的子目录是
`analysis arch diagrams process storage superpowers` —— **没有 `db-wiki`**。
也就是说基线 tag 时这个目录就不存在；脚本里那 14 条硬编码 REQUIRED 路径
（`git show pre-frozen-track-removal:scripts/check-wiki-refs.py` 可验）早就对不上。

⇒ **本次退役不认领这条账**。修复方式是把 `docs/db-wiki/*` 那份迁移补完，
或者调整脚本的 REQUIRED 清单，两者都超出本计划范围。

### A5 的精确口径

计划写「`git diff --stat <Task1>..HEAD -- engine/db/` 预期空」。
实测**非空** —— 因为 Task 5 **有意**改了 `engine/db/README.md`
（「冻结保留」→「已退役」）。把范围收窄到代码与构建文件后为**空**：

```
git diff --stat pre-frozen-track-removal..HEAD -- \
  engine/db/src engine/db/include engine/db/test engine/db/tools \
  engine/db/bench engine/db/Makefile
→ （空）
```

主核**一行代码都没动**，这是要保的判据；文档同步不违反它。

## 四、用例数为什么会变少（这是设计意图，不是丢失覆盖）

| 阶段 | ctest 条目 | 说明 |
|---|---|---|
| Task 1 基线 | **81** | 含 7 个 `mm_*_smoke`（来自 `engine/test/smoke`）与 2 个冻结轨测试 |
| Task 3 增删 | **83** | +4（`index_flat_test` 的 4 个 TEST）−2（删 `hnsw_filter_test` / `selector_integration_test`，各只注册 1 条） |
| Task 4 删 `smoke/` | **76** | −7（7 个 `mm_*_smoke`） |

`engine/test/index/`（15 文件 / 2,255 行）**贡献 0 条** —— `engine/test/CMakeLists.txt`
里从来没有 `add_subdirectory(index)`。把它算进差值判据就永远对不上账。

## 五、失败集合（11 项，全部在基线里）

```
mmdb_text_test                     mmdb_raii_test
hybrid_search_test                 xquery_test
xquery_id_len_test                 rag_test
rerank_test                        rag_embedding_test
hybrid_dual_channel_test           hybrid_dual_channel_enabled_test
backup_test
```

Task 1 基线共 14 项失败/超时，其中 3 项已随对应测试文件一起删除
（`hnsw_filter_test` / `selector_integration_test` / 以及 `smoke` 目录里的对应项），
剩下这 11 项**一条不差地都在基线里**。

⚠️ 判据是「**失败名集合不新增**」，不是「全绿」。这 11 项与本计划无关 ——
它们在 Task 1 采基线时就已经红了（rag / hybrid / xquery / text / raii / backup 各自的
既有缺陷）。**修它们是另一件事，不在 W-A 范围。**

## 六、SDK 自持能力清单（`nm` 取证）

`nm -g build/engine/src/sdk/libmmsdk.a` 里以下 23 个符号全部为 `T`（已定义），
且归档内**不存在** `faiss` / `hnsw` / `sql_engine` 的未解析引用：

| 类别 | 符号 |
|---|---|
| 内存上下文 | `palloc` `palloc0` `pfree` `AllocSetContextCreate` `MemoryContextReset` `MemoryContextDelete` `MemoryContextCheckThread` `MemoryContextCurrent` `MemoryContextSwitchTo` `MemoryContextGetGeneration` `mmdb_mem_register_resource` `mmdb_mem_unregister_resource` |
| 读写锁 | `mmdb_rwlock_init` `mmdb_rwlock_rdlock` `mmdb_rwlock_wrlock` `mmdb_rwlock_unlock` `mmdb_rwlock_destroy` |
| 扁平索引 | `mmdb_flat_index_create` `mmdb_flat_index_add` `mmdb_flat_index_size` `mmdb_flat_index_drop` `mmdb_flat_index_reserve` `mmdb_flat_search_filtered` |

`mmsdk` 的链接行现在只剩 **`sqlite3 algo-prod`**：
`sql_engine` / `faiss_hnsw` / `mm_vector_index` 三个冻结轨目标全部摘除。

## 七、取证命令一览

```powershell
# A1
foreach ($p in 'engine/src/db','engine/test/db','engine/include/db','engine/include/rag',
               'engine/include/tree_index','engine/test/smoke','engine/test/index',
               'engine/tools/benchmark/c_api','engine/cmake/ModalityProfiles.cmake',
               'engine/cmake/MultimodalConfig.cmake','engine/cmake/mm_smoke.cmake',
               'engine/cmake/gen_builtin_engines.cmake','engine/cmake/mm_smoke_expect.h.in',
               'engine/cmake/multimodal_config.h.in','engine/cmake/mm_builtin_engines.c.in') {
    "{0,-45} {1}" -f $p, (Test-Path $p)
}
Test-Path engine/db/Makefile

# A2
Remove-Item -Recurse -Force build/engine
cmake -B build/engine -S engine -DBUILD_TESTING=ON
cmake --build build/engine --parallel 4

# A3a / A3b
ctest --test-dir build/engine -N | Select-String 'Total Tests'
ctest --test-dir build/engine --output-on-failure

# A4
python scripts/verify-sdk-no-frozen.py   # 新增门禁，见 §十

# A5
git diff --stat pre-frozen-track-removal..HEAD -- engine/db/src engine/db/include `
    engine/db/test engine/db/tools engine/db/bench engine/db/Makefile

# A6
cd engine/db
& "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
& "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh

# A7
ctest --test-dir build/engine -R "MmdbMemctx|MmdbRootContext|MmdbRequestScope|SdkCoreMemctx|SdkVectorsMemctx|MemoryLeakDetection"

# A10
nm -g build/engine/src/sdk/libmmsdk.a | Select-String -Pattern 'T palloc|T pfree|T AllocSetContextCreate'
```

## 八、回滚方式

```bash
git reset --hard pre-frozen-track-removal   # tag 仍在，指向 Task 1 完成点
```

⚠️ 该 tag 的对象为 `c1dd9789572683168864e02c3bbdc2bbb63ad071`，已推到远端。
只有在明确决定放弃退役时才用；**force-push 需单独批准**。

## 九、后续事项（不在 W-A 范围）

1. **A9 的 `docs/db-wiki` 缺口** —— 要么补完迁移，要么调整 `check-wiki-refs.py` 的 REQUIRED 清单。
2. **11 项基线失败用例** —— rag / hybrid / xquery / text / raii / backup 各有既有缺陷，需逐项诊断。
3. **`staircase_benchmark` / `cross_lang_consistency_test`** —— 长时间占用 CPU（前者 >400 CPU 秒），
   应加超时或标记为 `LABELS "slow"` 从默认门禁剔除。
4. **主核的 `MM=` 编译期裁剪（W-B 已完成）与八模态对称化（W-C）** —— 下一个工作流。

## 十、新增的机械门禁：scripts/verify-sdk-no-frozen.py

退役之后，**没有任何机制阻止别人重新把 SDK 挂回冻结轨**。所以本次补了一道门禁脚本。

检查三项：

1. engine/src/sdk 与 engine/include/sdk 的源文件里不得出现 db/sql/ db/parser/
   db/index/ db/storage/ db/concurrency/ 这类路径
2. 不得出现 aiss_hnsw / ector_index_selector / sql_engine / mm_vector_index
   这类已删目标名
3. engine/src/sdk/CMakeLists.txt 的**未注释代码行**不得重新挂上这三个目标

`powershell
python scripts/verify-sdk-no-frozen.py   # 期望：OK（73 个源文件均无已退役依赖），退出码 0
`

⚠️ **做过负例自检**：故意注入一个 #include "db/sql/memctx.h" 的探针头文件，
脚本立刻报 FAIL（1 项） 并给出文件与行号；删除探针后回到 OK、退出码 0。
**只测正例的门禁等于没测** —— 一条永远绿的脚本不能证明任何事。

