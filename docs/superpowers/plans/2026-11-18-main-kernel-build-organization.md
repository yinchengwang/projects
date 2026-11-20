# 主核工程组织 —— CI 门禁 + `MM=` 编译期模态裁剪 + 每模态链接期证明（工作流 W-B）

> 依据：`docs/superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md` §4（设计 A）/ §6.5
> 落地日期：2026-11-18
> 执行顺序（**唯一权威是设计 §9，本行只是它的摘要**）：**P0 先行**（其余全部依赖它）；W-A 全部；**W-C 的 C1 ~ C5 全部完成之后才进 P1** —— P1 的 B1 会**第一次创建** `tools/mm_profiles.mk`，那时模态文件已在最终位置，**一次建对**；若 C2/C3 排在 P1 之后，`MM_FILES_blob` 的路径会全部失效 ⇒ `MM=vector` 裁不掉 blob ⇒ P1 验收必失败。**C6（8 profile + 8 smoke 重跑）在 P1/P2 之后。** 详见设计 §9 的顺序块（含规则一到规则六）。
> 互不冲突：W-A 动 `engine/src/` 与 `engine/include/`；本计划只动 `engine/db/` 与 `.github/`。

---

## Goal

给主核 `engine/db/` 补上三样它现在**没有**的东西：

1. **CI 门禁** —— 主核有 95 个测试文件（64 unit + 31 integration）、零 CI 覆盖。改动可能在本地绿、CI 红，也可能反过来。
2. **编译期模态裁剪** —— 一个 `MM=` 变量，把不需要的模态**在编译期**裁掉，
   产出"只有这一个模态"的二进制。这是"每个模态可单独部署"的**技术前提**。
3. **链接期独立证明** —— 8 个 smoke 可执行文件，每个只链接"底座 + 一个模态"。
   链接能过 = 该模态**没有**偷偷依赖别的模态。

## Architecture

```
engine/db/
├── Makefile                 ← 加 MM 开关（裁剪源文件 + 生成 -DDB_HAS_*）
├── tools/
│   ├── mm_profiles.mk       ← 【新增】8 个 profile 的唯一真相源
│   ├── check_deps.sh        ← 加一条：query/ 不许 include 模态头
│   └── measure_build.sh     ← 【新增】裁剪前后的构建度量
└── test/
    └── smoke/               ← 【新增】8 个 smoke 的 main
```

`MM` 的语义（关键）：

| 概念 | 内容 |
|---|---|
| **常驻集** | L0–L3 底座 + **关系模态**（关系模态是底座型模态，见下） |
| **可变集** | blob / graph / vector / ft / geo / ts / doc 七个模态 |
| **8 个模态 profile + `full` = 9 个 profile** | `full`（全部）、`relational`、`blob`、`graph`、`vector`、`ft`、`geo`、`ts`、`doc` |
| **裁剪方式** | ① 源文件过滤（Makefile）② `-DDB_HAS_<MOD>=0/1` ③ 5 处 `#if` 裁剪点 |

> **关系模态为什么恒常驻**：它的实现横跨 L1–L3 —— 堆在 `src/storage/heap*.c`、
> 记录编解码在 `src/storage/record.c` / `value.c`、catalog 的表项里**直接持有 `Heap*`**
> （`catalog.h:8`）、执行器适配在 `src/spi/heap_am*.c` / `btree_am.c`。
> 关掉关系模态 = catalog 编译不过。**所以 `MM` 裁的是"模态"，不是"层"。**

## Tech Stack

- C11、GNU Make（`engine/db/` 自带 Makefile，**不在 CMake 图内**）
- POSIX sh（`tools/check_deps.sh`、`test/run.sh` 已是 sh）
- GitHub Actions（`.github/workflows/ci.yml`，在 `projects` 仓里）
- Linux：gcc + `make`；Windows：MinGW-w64 gcc + `mingw32-make` + Git Bash 的 `sh`

## Global Constraints

1. **提交信息用中文**；每个 Task 结束立即 `git push origin main`。
2. **主核的硬规则不许破**：`-Wall -Wextra -Werror` 零警告；新文件 ≤ 800 行；
   注释用中文；坑用 `M<层>-<序号>` 编号。
3. **分层门禁每次改完必跑**：`sh tools/check_deps.sh` ⇒ `✓ 依赖方向正确`。
4. **Windows 上 `sh` 不在 PATH** —— 用绝对路径 `& "C:\Program Files\Git\usr\bin\sh.exe"`；
   Makefile 自己已经处理了这件事（`SHELL :=` 绝对路径），不要在 Makefile 里用 `$(shell ...)`。
5. **`MM` 不许改变默认行为** —— 不带 `MM=` 的构建必须与今天**逐字节等价**。
6. 不碰 `engine/src/`、`engine/include/`（那是 W-A 的地盘）。

## 术语约定（复审补入）

⚠️ **本仓里「裁剪」已经有两个相反的含义**，混用会出理解事故：

| 词 | 在本仓指什么 | 出处 |
|---|---|---|
| **模态裁剪**（本计划统一用这个词） | **编译期**按 `MM` profile 把不用的模态从构建里去掉 | 本计划 |
| **裁剪**（`MODALITIES.md` 的用法） | **运行期**分区裁剪 / 谓词下推裁剪（"裁剪只能少读，不能少给"） | `engine/db/docs/MODALITIES.md` |

⇒ 本计划（以及 W-A / W-C）里凡指编译期的那一处，一律写 **「模态裁剪」**或 **「MM 裁剪」**。

## Review Focus
## Review Focus

| # | 关注点 | 判据 |
|---|---|---|
| R1 | 裁剪是不是真的裁了 | `make MM=vector` 产出的二进制里，`nm` 查不到 `db_ft_open` / `db_graph_open` / `db_ts_open` 等符号 |
| R2 | 关系模态有没有被误裁 | `make MM=vector` 的二进制里 `heap_am_build` / `catalog_find_table` 仍在 |
| R3 | 5 处裁剪点够不够 | 裁剪后**链接通过**（少一处 `#if` 就是链接错误，不会静默通过） |
| R4 | `MM=full` 与今天等价 | `make MM=full` 的 `build/db.exe` 与改动前功能一致；95 个测试全绿 |
| R5 | smoke 是不是"真证明" | smoke 的链接集合里**没有**别的模态的符号（`nm` 验证） |
| R6 | CI 是不是真的在跑主核 | CI 日志里能看到 `check_deps.sh` 与 `test/run.sh` 的输出 |
| R7 | 门禁有没有被绕过 | `check_deps.sh` 新增的规则在**故意违规**时必须红（要有反例验证） |

---

## 文件结构

### 新增

| 文件 | 作用 |
|---|---|
| `engine/db/tools/mm_profiles.mk` | 8 个 profile 的唯一真相源：模态清单、源文件归属、依赖、宏后缀 |
| `engine/db/tools/mm_profiles_test.sh` | **（新增）** `mm_profiles.mk` 的正确性测试：profile 模态集合 / 文件存在性 / 依赖闭包前置条件 |
| `engine/db/tools/measure_build.sh` | 构建度量：按 profile 统计编译文件数 / 目标文件数 / 链接后符号数 / 耗时 |
| `engine/db/test/smoke/smoke_relational.c` | 关系模态 smoke |
| `engine/db/test/smoke/smoke_blob.c` | blob 模态 smoke |
| `engine/db/test/smoke/smoke_graph.c` | graph 模态 smoke |
| `engine/db/test/smoke/smoke_vector.c` | vector 模态 smoke |
| `engine/db/test/smoke/smoke_ft.c` | ft 模态 smoke |
| `engine/db/test/smoke/smoke_geo.c` | geo 模态 smoke |
| `engine/db/test/smoke/smoke_ts.c` | ts 模态 smoke |
| `engine/db/test/smoke/smoke_doc.c` | doc 模态 smoke |
| `engine/db/docs/MM-PROFILES.md` | 8 个模态 profile + `full` 的说明与实测数字（**含门禁值列**） |
| `docs/arch/db/multimodal-build-baseline.md` | **（设计 §9 指定路径）** 跨模态构建基线 —— 与 MM-PROFILES.md 内容一致，但按设计的路径落一份，作为 CI 门禁的对照基准 |

### 修改

| 文件 | 改什么 |
|---|---|
| `engine/db/Makefile` | 加 `MM` 开关、裁剪源文件、生成 `-DDB_HAS_*`、8 个 smoke 目标 |
| `engine/db/tools/check_deps.sh` | 加「query/ 不许 include 模态头」这条门禁 |
| `.github/workflows/ci.yml` | 加主核的 Linux / Windows 两个 job |

---

### Task B0：主核进 CI（**已完成**，提交 `3bca8a751` + `d1f0e52e2` + `e8b98685c`）

**Files（实际）**
- 修改：`.github/workflows/ci.yml`（+96 行，两个 job）
- 修改：`engine/db/Makefile`（`-lm`）、`engine/db/test/run.sh`（`-D_POSIX_C_SOURCE`）
- 修改：10 个源/测试文件（两个真缺陷 + 三处平台差异，见下）

**Interfaces（实际）**
- 新增两个 CI job：`build-kernel-linux`、`build-kernel-windows`。
- 两者都跑：`make all` → `sh tools/check_deps.sh` → `JOBS=4 sh test/run.sh`。

> ⚠️ **Windows 上命令是 `mingw32-make` 而不是 `make`**：
> MSYS2 的 MINGW64 里 `make` 是 MSYS 版，会去用 `/usr/bin/sh`；
> 而主核 Makefile 的 SHELL 探法是 `where sh` / 硬编码 Git 的 sh.exe。

**为什么第一个做**：后面每个 Task 的验收都要靠 CI 抓"本地绿、CI 红"的情况。

- [x] **B0.1 先确认 Linux 上主核能不能编（本地预演）**

  本地没有 Docker，但有 WSL Ubuntu（gcc 15.2）。于是把 144 个源文件全部
  编译、105 个测试全部跑一遍。**这是主核第一次在 Linux 上跑。**

  结果（修好之后）：库 0 失败、测试 **105 通过**。修之前撞出的东西见 B0.5。

- [x] **B0.2 在 `.github/workflows/ci.yml` 里加 Linux job**

  ```yaml
  build-kernel-linux:
    name: 主核（Linux/GCC）
    runs-on: ubuntu-22.04
    steps:
    - uses: actions/checkout@v4
      with:
        submodules: false
        persist-credentials: false
    - run: sudo apt-get update && sudo apt-get install -y build-essential
    - working-directory: engine/db
      run: make all
    - working-directory: engine/db
      run: sh tools/check_deps.sh
    - working-directory: engine/db
      run: JOBS=4 sh test/run.sh
  ```

  > **不需要拉 uthash 子模块** —— 那是 `engine/src/db/` 冻结轨的依赖，主核不用。

- [x] **B0.3 加 Windows job**

  照抄 `build-engineering-windows` 的 `msys2/setup-msys2@v2` 头（同一个
  `MINGW64` 环境），只装 `gcc + make`，命令换成 `mingw32-make`。

- [x] **B0.4 推上去看 CI**

  已提交并推送（`3bca8a751`）。GitHub Actions 的实际结果只能在它跑完之后看 ——
  本地能做到的是**把三条命令都在本机跑一遍**（Windows + Linux 都跑了，全绿）。

- [x] **B0.5 CI 红了怎么办 —— 实际撞到的 5 类问题**

  计划当初列了一张"Linux 平台差异清单"（GetCurrentThreadId / clock_gettime /
  mkdir / 路径分隔符 / clock() 分辨率）。**实际一个都不是那些**。真实撞到的是：

  | 症状 | 性质 | 处置 |
  |---|---|---|
  | `blob.c` 栈溢出（`*** stack smashing detected ***`，4 个测试同时炸） | **真缺陷** | 索引块按页内槽数写（1019）而不是按覆盖帧数（1024） |
  | `test_page.c` 栈溢出 | **真缺陷**（测试 bug） | `u8 rec[100]` → `[128]`（要装 120 字节记录） |
  | `-Werror=format-truncation`（parser.c 两处） | GCC 15 新诊断 | `copy_name()` 显式长度运算，截断行为不变 |
  | `-Werror=unused-result`（ftruncate） | glibc 标了 `warn_unused_result` | 接返回值再丢（`(void)` 强转不算使用） |
  | `undefined reference to 'log'` / `pthread_barrier_*` | 构建面缺失 | Makefile 加 `-lm`；run.sh 加 `-D_POSIX_C_SOURCE=200809L` |
  | `system()` 找不到 `./build/xxx`（POSIX 分支写了 `.exe`） | 测试脚本 bug | 去掉 `.exe` 后缀 |
  | `stack-use-after-scope`（`&(WalCount){0}`，10 处） | **真缺陷**（生命周期） | 改具名局部变量 |

  ⚠️ **两个"真缺陷"在 Windows 上一直藏着**：MinGW 的栈布局宽松、`_FORTIFY` 不查、
  栈保护也没抓到。这正是 B0 的价值 —— 它不是"补一条流水线"，
  而是**第一次让主核在另一个平台上被检查**。

  ⚠️ **还有一条差点漏掉的**：`test_catalog_lock.c` 的 `.exe` 后缀让 system() 失败，
  而 `rc != 0` 那条断言**照样通过**（假绿），只有标记文件那条才红。
  **两条断言合起来才看得出问题 —— 少一条就变成"看起来在跑，其实没跑"。**

- [x] **B0.6 CI 绿了再提交 B0.5 的修复（若有）**

  修复单独提交（`d1f0e52e2` 代码 + `e8b98685c` 构建面），
  与 CI 配置（`3bca8a751`）分开 —— 这样"CI 首次跑"对应的才是真实状态。

  > 一个本地交叉验证踩到的坑：`build/` 是两端**共享**的产物目录。
  > 在 Linux 上编完再回 Windows 编，MinGW 的 ld 会读到 Linux 的 `.o`，
  > 报一堆 `undefined reference to '__snprintf_chk'` / `relocation truncated`。
  > 清掉 `build/` 重编即恢复。CI 每个 job 都是干净检出，不会遇到；本地会。
---

### Task B1：`tools/mm_profiles.mk` —— profile 的唯一真相源

**Files**
- 新建：`engine/db/tools/mm_profiles.mk`

**Interfaces**
- 该文件被 `Makefile` `include`；供 `check_deps.sh` 与文档引用。
- 它定义：`MM_ALL`、`MM_CROPPABLE`、`MM_UP_<m>`、`MM_FILES_<m>`、`MM_DEP_<m>`、`MM_SET_<profile>`。

**⚠️ 为什么必须是纯 make（不能用 `$(shell)`）**：
Windows 下 make 的 `$(shell ...)` 走 cmd.exe（见 `Makefile` L4-20 的注释），
`tr` / `sed` 不可用 ⇒ **大写转换一律手写**。

- [ ] **B1.1 建 `engine/db/tools/mm_profiles.mk`**

  ```make
  # mm_profiles.mk —— MM 编译期裁剪的唯一真相源
  #
  # 【这个文件为什么是"唯一真相源"】
  # 「一个模态的代码边界在哪」这件事必须**只在一个地方**写。
  # 它同时被三处消费：
  #   1. Makefile        —— 据此裁剪源文件、生成 -DDB_HAS_* 宏
  #   2. tools/check_deps.sh —— 据此校验模态之间不许互相 include
  #   3. engine/db/docs/MM-PROFILES.md —— 人工可读的说明
  # 三处各写一份 = 迟早不一致 = 裁剪出来的二进制缺东西，链接才报错。
  #
  # 【为什么不用 $(shell) 推导】
  # Windows 下 make 的 $(shell ...) 走 cmd.exe（见 Makefile L4-20 的注释），
  # tr / sed 之类不可用 ⇒ 大写后缀一律手写在这里。
  #
  # 【路径口径】
  # 全部相对 engine/db/（make 的工作目录）。

  # ---------------------------------------------------------------------
  # 模态清单
  # ---------------------------------------------------------------------
  # 全部 8 个模态
  MM_ALL := relational blob graph vector ft geo ts doc

  # 可裁剪的模态（7 个）。
  # 关系模态不在此列 —— 它是**底座型模态**，恒常驻（见下）。
  MM_CROPPABLE := blob graph vector ft geo ts doc

  # 模态 → -DDB_HAS_* 宏的后缀（大写，手写；见文件头说明）
  MM_UP_relational := RELATIONAL
  MM_UP_blob       := BLOB
  MM_UP_graph      := GRAPH
  MM_UP_vector     := VECTOR
  MM_UP_ft         := FT
  MM_UP_geo        := GEO
  MM_UP_ts         := TS
  MM_UP_doc        := DOC

  # ---------------------------------------------------------------------
  # 关系模态为什么恒常驻
  # ---------------------------------------------------------------------
  # 关系模态是底座型模态：实现横跨 L1–L3
  #   堆实现      src/storage/heap*.c
  #   记录编解码   src/storage/record.c、value.c
  #   catalog     src/catalog/catalog.c —— 表项里**直接持有 Heap***
  #   执行器适配   src/spi/heap_am*.c、src/spi/btree_am.c
  # catalog.h:8 与 vacuum.h:6 都持有 Heap* ⇒ 关掉关系模态 = catalog 编译不过。
  #
  # 结论：**MM 裁的是"模态"，不是"层"**。每个 profile 都含关系模态。

  # ---------------------------------------------------------------------
  # 每个模态占用的源文件（相对 engine/db/）
    # ⚠️ **本文件在 W-C 的 C-2/C-3/C-4/C-5 之后才创建** ——
    #    所以这里写的路径**已经是搬家后的新路径**（blob 在 src/modalities/blob/、
    #    关系适配器在 src/modalities/relational/、文档模态在 src/modalities/doc/）。
    #    不存在「先写旧的、之后再同步」的第二步。

  # ---------------------------------------------------------------------
  # ⚠️ src/storage/gstore*.c **不属于 graph**：
  #    gstore 是"通用磁盘存储"，graph 与 vector **共用**
  #    （vector_am.c:125 调 gstore_cursor_init(vstore_meta(...))）。
  #    所以它是底座，任何 profile 都在。

  MM_FILES_blob   := $(wildcard src/modalities/blob/*.c) src/db_blob.c
  MM_FILES_graph  := $(wildcard src/modalities/graph/*.c) src/db_graph.c src/db_graph_walk.c
MM_FILES_vector := $(wildcard src/modalities/vector/*.c) src/db_vec.c src/repl_vec.c
  MM_FILES_ft     := $(wildcard src/modalities/ft/*.c) src/db_ft.c src/repl_ft.c
  MM_FILES_geo    := $(wildcard src/modalities/geo/*.c) src/db_geo.c src/db_geo_query.c src/repl_geo.c
  MM_FILES_ts     := $(wildcard src/modalities/ts/*.c) src/db_ts.c src/repl_ts.c
  MM_FILES_doc    := $(wildcard src/modalities/doc/*.c)

  # blob 的实现还住在 L1 与 L3（W-C 的 C-3 会把它们搬进 src/modalities/blob/，
  # 届时同步改这一行）。
  MM_FILES_blob += src/storage/blob.c src/storage/blob_hdr.c src/spi/blob_am.c

  # ---------------------------------------------------------------------
  # 模态依赖（闭包用）
  # ---------------------------------------------------------------------
  # 当前**没有可变模态之间的依赖**：
  #   doc 依赖的是关系模态（恒常驻，不进闭包计算）；
  #   ft 的倒排索引不依赖 blob（段内容在磁盘页里，不在对象存储里）。
  # 机制先立起来 —— 将来有人写 MM_DEP_vector := blob，缺依赖会被立刻抓出来。
  MM_DEP_blob   :=
  MM_DEP_graph  :=
  MM_DEP_vector :=
  MM_DEP_ft     :=
  MM_DEP_geo    :=
  MM_DEP_ts     :=
MM_DEP_doc    := relational   # 文档模态是寄生模态：JSONB 存在关系模态的 heap 行里 ⇒ 依赖关系模态

  # ---------------------------------------------------------------------
  # 8 个 profile
  # ---------------------------------------------------------------------
  # 每个 profile 显式列出打开的模态集合（可变模态部分）。
  # full = 全部 7 个可变模态；relational = 一个都不开。
  MM_SET_full      := $(MM_CROPPABLE)
  MM_SET_relational :=
  MM_SET_blob      := blob
  MM_SET_graph     := graph
  MM_SET_vector    := vector
  MM_SET_ft        := ft
  MM_SET_geo       := geo
  MM_SET_ts        := ts
  MM_SET_doc       := doc

  # 全部 profile 名（给错误提示用）
  MM_PROFILES := full relational blob graph vector ft geo ts doc
  ```

- [ ] **B1.2 自检：这个文件能被 make 正确解析**

  在 `engine/db/Makefile` 末尾临时加一行（验完删掉）：

  ```make
  mm-selftest:
      @echo "MM_ALL=$(MM_ALL)"
      @echo "MM_CROPPABLE=$(MM_CROPPABLE)"
      @echo "MM_FILES_vector=$(MM_FILES_vector)"
  ```

  ```powershell
  cd C:\code\projects\engine\db
  mingw32-make mm-selftest
  ```

  预期：
  ```
  MM_ALL=relational blob graph vector ft geo ts doc
  MM_CROPPABLE=blob graph vector ft geo ts doc
  MM_FILES_vector=src/modalities/vector/hnsw.c src/modalities/vector/hnsw_am.c ... src/db_vec.c
  ```

⚠️ `MM_FILES_vector` 应该有 **10 项**（8 个 `modalities/vector/*.c` + `db_vec.c` + `repl_vec.c`）。
  对不上就说明路径写错了。

- [ ] **B1.3 删掉临时目标，提交**

  ```powershell
  cd C:\code\projects
  git add engine/db/tools/mm_profiles.mk
  git commit -m "feat(kernel): 新增 MM profile 真相源（8 个 profile 的模态边界）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

- [ ] **B1.4 写 `engine/db/tools/mm_profiles_test.sh`（先写、跑红）**

⚠️ **（可执行性修正）B1 原来完全没有测试** —— `mm_profiles.mk` 是 8 个 profile 的
**唯一真相源**，却没有一条断言能验证「某 profile 的文件清单正确」。
B1.2 只是「用 `make -p` 打印出来用眼睛看」，那不是测试。

⇒ 新建 `engine/db/tools/mm_profiles_test.sh`，对**每个** profile 断言三件事：

```sh
#!/bin/sh
# mm_profiles.mk 的正确性测试。
#
# 【为什么要有这个测试】
# mm_profiles.mk 是 8 个 profile 的"唯一真相源"——
# 它错了，`make MM=xxx` 要么编出完整二进制（裁不掉），
# 要么 undefined reference（裁多了）。两种都只有到编译期才暴露。
# 这个脚本把两类错误提前到"改完文件立刻"就能发现。
set -u
cd "$(dirname "$0")/.." || exit 1
fail=0

# ---- 断言 1：每个 profile 的模态集合符合预期 ----
check_set() {
    got=$(mingw32-make -f tools/mm_profiles.mk --eval 'print:' \
          --eval "\t@echo \$(MM_SET_$1)" print 2>/dev/null | tr -d '\r')
    [ "$got" = "$2" ] || { echo "FAIL MM_SET_$1: 期望 [$2] 实际 [$got]"; fail=1; }
}
check_set relational ""
check_set blob       "blob"
check_set graph      "graph"
check_set ft         "ft"
check_set geo        "geo"
check_set ts         "ts"
check_set vector     "vector"
check_set doc        "doc"
check_set full       "blob graph vector ft geo ts doc"

# ---- 断言 2：MM_FILES_<m> 列出的文件全部真实存在 ----
# （列了不存在的文件 ⇒ make 直接 'No rule to make target'）
for m in blob graph ft geo ts vector doc relational; do
    for f in $(mingw32-make -f tools/mm_profiles.mk --eval 'print:' \
               --eval "\t@echo \$(MM_FILES_$m)" print 2>/dev/null | tr -d '\r'); do
        [ -f "$f" ] || { echo "FAIL MM_FILES_$m 列了不存在的文件: $f"; fail=1; }
    done
done

# ---- 断言 3：MM_DEP_<m> 的每个依赖都在 MM_ALL 里 ----
# （依赖闭包校验器的前置条件：闭包算法只认 MM_ALL 里的名字）
all=$(mingw32-make -f tools/mm_profiles.mk --eval 'print:' \
      --eval '\t@echo $(MM_ALL)' print 2>/dev/null | tr -d '\r')
for m in $all blob graph ft geo ts vector doc; do
    for d in $(mingw32-make -f tools/mm_profiles.mk --eval 'print:' \
               --eval "\t@echo \$(MM_DEP_$m)" print 2>/dev/null | tr -d '\r'); do
        case " $all " in
            *" $d "*) ;;
            *) echo "FAIL MM_DEP_$m 依赖了不在 MM_ALL 里的模态: $d"; fail=1 ;;
        esac
    done
done

[ $fail -eq 0 ] && echo "✓ mm_profiles.mk 全部断言通过"
exit $fail
```

**TDD 顺序（硬要求）**：
1. **先写这个脚本**，在 `mm_profiles.mk` 还**没建**的时候跑 → 必须**红**（`make` 报找不到文件）；
2. 建 `mm_profiles.mk` → 脚本转绿；
3. 故意把 `MM_FILES_blob` 里的一个文件名改错 → 脚本必须**红**（断言 2 抓到）；
4. 改回来 → 绿。

第 3 步是**反向验证** —— 没有它，你不知道这个测试是真在跑还是空转。

运行方式（Windows 上没有 `sh` 的 PATH 入口）：

```powershell
cd C:\code\projects\engine\db
& "C:\Program Files\Git\usr\bin\sh.exe" tools/mm_profiles_test.sh
```

⚠️ 这个脚本要加进 `Makefile` 的 `check` 目标，与 `check_deps.sh` 并列：

```make
check:
\tsh tools/check_deps.sh
\tsh tools/mm_profiles_test.sh
```

---
---

### Task B2：Makefile 接入 `MM`（裁剪源文件 + 生成宏）

**Files**
- 修改：`engine/db/Makefile`

**Interfaces**
- 新增命令行变量：`MM`（默认 `full`）、`MM_OBJ_DIR`（派生于 `OBJ_DIR`）。
- 新增宏：`DB_HAS_RELATIONAL` / `DB_HAS_BLOB` / `DB_HAS_GRAPH` / `DB_HAS_VECTOR` /
  `DB_HAS_FT` / `DB_HAS_GEO` / `DB_HAS_TS` / `DB_HAS_DOC`（恒定定义，值为 0 或 1）。

- [ ] **B2.1 在 `Makefile` 的 `OBJ_DIR ?= build`（L62）之后、`SRCS :=`（L81）之前插入 MM 块**

  ⚠️ **（可执行性修正）插入点的下界必须写死** ——
  `Makefile` 里 `SRCS :=`（**L81**）、`OBJS :=`（L82）、`LIB_OBJS :=` 都是
  **`:=` 立即赋值**：求值那一刻就把右边的 `$(wildcard ...)` 展开并**定死**。
  ⇒ MM 块若被追加到**文件末尾**，`SRCS` 里仍然是**全量源文件**
  ⇒ `make MM=vector` 会**静默地链接出完整二进制** ——
  裁剪「看起来成功」（构建不报错、不报错才最坏），实际**一点都没裁**。

  ⇒ 插入点必须**同时**满足两条：
  1. **在 `OBJ_DIR ?= build`（L62）之后** —— MM 块要用 `$(OBJ_DIR)`
     （`OBJ_BASE ?= $(OBJ_DIR)`），上移到 L62 之前会反过来坏掉；
  2. **在 `SRCS :=`（L81）之前** —— 让 `SRCS := $(filter-out $(MM_CROP_SRCS),$(SRCS_ALL))`
     在**定义时**就能看到已经算好的 `MM_CROP_SRCS`。

  ⚠️ **`CFLAGS` 是另一条独立的时序**：`Makefile` L57 是 `CFLAGS := ...`
  （立即赋值）⇒ MM 块必须**晚于 L57**（本插入点满足），
  且 `CFLAGS` 必须改成**延迟赋值** `=`（B2.3）。

  ```make
  # ---------------------------------------------------------------------
  # MM：编译期模态裁剪
  # ---------------------------------------------------------------------
  # 用法：
  #   make MM=full          # 全部模态（默认，与不带 MM 完全等价）
  #   make MM=vector        # 只编 vector 模态（+ 恒常驻的关系模态 + 底座）
  #   make MM=doc           # 只编文档模态
  #
  # 【MM 裁的是"模态"，不是"层"】
  # 每个 profile 都含 L0–L3 底座**与关系模态**（关系模态的实现横跨 L1–L3，
  # catalog 直接持有 Heap* ⇒ 关不掉）。详见 tools/mm_profiles.mk。
  #
  # 【为什么非 full 的 profile 用独立的对象目录】
  # 裁剪后的 .o 绝不能和 full 的 .o 混在一处：
  # 混了之后 `make MM=full` 会因为"目标文件比源文件旧"而跳过重编，
  # 于是跑的是上一次的二进制 —— 这正是本文件头部注释里说的那个阴险坑
  # （改了源码、编译"成功"、跑的是旧二进制）。
  # 做法与 BIGLOCK_DIR 一致：各写各的目录。
  MM ?= full
  include tools/mm_profiles.mk

  MM_OBJ_DIR := $(if $(filter full,$(MM)),$(OBJ_BASE),$(OBJ_BASE)_mm_$(MM))

  # ---- profile 校验 ----
  ifeq ($(filter $(MM),$(MM_PROFILES)),)
    $(error 未知的 MM profile '$(MM)'。可选：$(MM_PROFILES))
  endif

  # ---- 依赖闭包（展开到不动点；三轮足够 —— 模态只有 8 个）----
  # ⚠️ **（可执行性修正）关系模态恒常驻 ⇒ 永远在闭包里** ——
  # 原来写的是 `MM_CLOSED := $(MM_SET_$(MM))`，而 `MM_SET_relational` 是**空的**
  # （关系模态恒常驻，不进「可变集」）⇒ `MM=relational` 时闭包里**没有** relational
  # ⇒ `DB_HAS_RELATIONAL=0` ⇒ 与「关系模态恒常驻」直接矛盾，
  # 而且所有 `#if DB_HAS_RELATIONAL` 的代码会被**整块裁掉**。
  # ⇒ 闭包的种子必须**无条件带上** relational。
  MM_CLOSED := relational $(MM_SET_$(MM))
  MM_CLOSED := $(MM_CLOSED) $(foreach m,$(MM_CLOSED),$(MM_DEP_$(m)))
  MM_CLOSED := $(MM_CLOSED) $(foreach m,$(MM_CLOSED),$(MM_DEP_$(m)))
  MM_CLOSED := $(sort $(MM_CLOSED))

  # ---- 校验：依赖名必须是已知模态 ----
  $(foreach m,$(MM_CLOSED),$(foreach d,$(MM_DEP_$(m)),\
    $(if $(filter $(d),$(MM_ALL)),,$(error 模态 $(m) 的依赖 $(d) 不是已知模态（已知模态：$(MM_ALL)）))))

  # ---- 未激活模态的源文件（要裁掉的）----
  MM_CROP_SRCS :=
  $(foreach m,$(MM_CROPPABLE),\
    $(if $(filter $(m),$(MM_CLOSED)),,$(eval MM_CROP_SRCS += $(MM_FILES_$(m)))))

  # ---- DB_HAS_* 宏：每个模态恒定定义（开=1，关=0）----
  MM_FLAGS :=
  $(foreach m,$(MM_ALL),\
    $(eval MM_FLAGS += -DDB_HAS_$(MM_UP_$(m))=$(if $(filter $(m),$(MM_CLOSED)),1,0)))

  # ---- 实际使用的对象目录 ----
  # ⚠️ **（复审修正）命令行的 `OBJ_DIR=` 会覆盖 makefile 里的赋值** ——
  # GNU Make 里「命令行变量 > makefile 赋值」，所以
  #   make MM=vector OBJ_DIR=out
  # 会让 `OBJ_DIR` 停在 `out` ⇒ **和 `make MM=full` 共用同一个目录** ⇒
  # 裁剪的 .o 与 full 的 .o 混在一处 ⇒ 跑的是旧二进制（正是本文件头部警告的坑）。
  #
  # ⇒ 引入 `OBJ_BASE` 承接「用户想放的根目录」，再用 `override` 落最终目录。
  OBJ_BASE ?= $(OBJ_DIR)
  override OBJ_DIR := $(MM_OBJ_DIR)
  ```

  ⚠️ **`clean` 目标必须跟着改**。现有 `clean:` 是
  `rm -rf $(OBJ_DIR) $(BIGLOCK_DIR) $(TARGET)` —— 只删 `build/`，不删 `build_mm_vector/`。
  裁剪 profile 的残留 .o 会被下一次 `make MM=vector` 直接复用（时间戳比源文件新 ⇒ 不重编）
  ⇒ 跑的是上一次的二进制。改成：

  ```make
  clean:
      rm -rf $(OBJ_DIR) $(OBJ_DIR)_mm_* $(BIGLOCK_DIR) $(TARGET)
  ```

  ⚠️ **`bench` 目标只在 `MM=full` 下有意义**。`make bench` 要链 `vec_bench` / `graph_bench`
  / `ft_bench` / `geo_bench` / `ts_bench`，它们依赖各自模态的对象。
  裁剪 profile 下这些目标会**缺符号**。
  处理：在 `bench` 目标上加守卫

  ```make
  bench: $(VEC_BENCH_EXE) $(GRAPH_BENCH_EXE) $(REF_EXE) $(EXEC_BENCH_EXE) $(LATCH_BENCH_EXE) $(LATCH_BENCH_BIGLOCK_EXE) $(FT_BENCH_EXE) $(GEO_BENCH_EXE) $(TS_BENCH_EXE) $(INS_BATCH_BENCH_EXE)
  ```
  保持不变，但**在 `tools/mm_profiles.mk` 顶部加一行注释**说明
  「`make bench` 仅在 `MM=full` 下可用」；`$(error ...)` 守卫放在 Makefile 里：

  ```make
  ifneq ($(filter full,$(MM)),)
  bench: $(VEC_BENCH_EXE) $(GRAPH_BENCH_EXE) $(REF_EXE) $(EXEC_BENCH_EXE) $(LATCH_BENCH_EXE) $(LATCH_BENCH_BIGLOCK_EXE) $(FT_BENCH_EXE) $(GEO_BENCH_EXE) $(TS_BENCH_EXE) $(INS_BATCH_BENCH_EXE)
  else
  bench:
  	@echo "make bench 只在 MM=full 下可用（当前 MM=$(MM)）"; exit 1
  endif
  ```

- [ ] **B2.2 裁剪源文件**

  现有 L81：

  ```make
  SRCS := $(wildcard $(SRC_DIR)/*/*.c $(SRC_DIR)/*/*/*.c $(SRC_DIR)/*.c)
  ```

  改成：

  ```make
  SRCS_ALL := $(wildcard $(SRC_DIR)/*/*.c $(SRC_DIR)/*/*/*.c $(SRC_DIR)/*.c)
  # 裁掉未激活模态的源文件（MM=full 时 MM_CROP_SRCS 为空 ⇒ 与原来完全等价）
  SRCS := $(filter-out $(MM_CROP_SRCS),$(SRCS_ALL))
  ```

  ⚠️ **（可执行性修正）这一行必须留在 L81 原位（MM 块之下）** ——
  `SRCS :=` 是**立即赋值**：它在被读到的那一瞬间就把 `$(MM_CROP_SRCS)` 定死。
  ⇒ MM 块若落在**文件末尾**（`SRCS :=` 之后），此刻 `MM_CROP_SRCS` 还是**空**
  ⇒ `SRCS` = 全量 ⇒ **裁剪静默失效**（构建照样绿，二进制却是全量的）。
  ⇒ 所以 B2.1 的插入点是「**L62 之后、L81 之前**」，不是「文件末尾」。

- [ ] **B2.3 把宏加进 CFLAGS（并且把 `CFLAGS` 改成延迟赋值）**

  ⚠️ **（复审修正）这一步原来会写出「宏永远到不了编译器」的版本** ——
  `Makefile:57` 是 `CFLAGS := ...`（**立即赋值**），而 MM 块插在 `OBJ_DIR ?= build`
  （L62）**之后** ⇒ 求值 `CFLAGS` 时 `$(MM_FLAGS)` **还是空的** ⇒
  `-DDB_HAS_*` 一个都没进编译命令行 ⇒ 所有 `#if DB_HAS_*` 恒为 0。

  ⇒ **两处都要改**：

  1. **`Makefile:57` 的 `CFLAGS :=` 改成 `CFLAGS =`**（延迟赋值）。
     它在**使用时**才展开，那时 MM 块已经跑完、`MM_FLAGS` 已经填好。
     ⚠️ 不能把 MM 块整体上移 —— MM 块要用 `$(OBJ_DIR)`（L62 才定义），上移会反过来坏掉。

  2. **B2.3 改写后的那行也必须是 `=` 而不是 `:=`**：

  ```make
  # 注意是 = 不是 := —— 延迟赋值，才能拿到后面 MM 块算出来的 $(MM_FLAGS)
  CFLAGS = $(CSTD) $(WARN) $(OPT) $(HARDEN) $(INC) $(SAN) $(MM_FLAGS)
  ```

  ✅ **验收（必做）** —— 确认宏真的进了编译命令行：

  ```powershell
  cd C:\code\projects\engine\db
  mingw32-make -n MM=vector | Select-String 'DB_HAS_VECTOR=1'
  mingw32-make -n MM=vector | Select-String 'DB_HAS_FT=0'
  ```

  预期：两条都**有命中**。若 `DB_HAS_VECTOR=1` 零命中 ⇒ `MM_FLAGS` 没接上，
  **此时所有 `#if DB_HAS_*` 都是 0，但编译照样通过** —— 这是最阴险的一类错，
  必须显式验。

- [ ] **B2.4 验证：默认构建不变**

  ```powershell
  cd C:\code\projects\engine\db
  mingw32-make clean
  mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：编译零警告；95 通过 / 0 失败。

  ⚠️ **此时还没有 `#if` 裁剪点**，所以 `make MM=vector` 会在链接阶段报
  "undefined reference to db_ft_open" —— **这是预期的**，B3 修。

- [ ] **B2.5 验证：裁剪确实生效（源文件层面 + 链接产物的反向断言）**

  ```powershell
  mingw32-make MM=vector -n 2>&1 | Select-String 'ft_am|db_ft' | Measure-Object
  ```

  预期：`0`（`make -n` 打印的命令里**没有**任何 ft 模态的编译行）。

  再看反向：

  ```powershell
  mingw32-make MM=vector -n 2>&1 | Select-String 'vector_am' | Measure-Object
  ```

  预期：`> 0`。

  ⚠️ **（可执行性修正）只验 `make -n` 是不够的，必须补一条 `nm` 反向断言** ——
  `make -n` 只证明**命令行**里没有 ft 的编译行，
  证明不了**链接产物**里没有 ft 的符号：
  若 `SRCS` 的 `:=` 时序错了（MM 块被追加到文件末尾，落在 L81 之下），
  `make -n` 看不出问题，而 `make MM=vector` 会**静默链接出完整二进制** ——
  裁剪「看起来成功」，实际没裁。

  ⇒ 补一条**真实构建 + 符号反向断言**：

  ```powershell
  cd C:\code\projects\engine\db
  mingw32-make MM=vector clean
  mingw32-make MM=vector all
  nm build_mm_vector/db.exe | Select-String 'db_ft_'
  ```

  预期：**零命中**（`db_ft_*` 一个都不在二进制里）⇒ 证明 ft **真的**被裁掉了。
  ⇒ 只要有一处命中 ⇒ 裁剪没生效 ⇒ 回去查 B2.1/B2.2 的**插入点与 `:=` 时序**。
  （`build_mm_vector` 是 `MM=vector` 的产物目录：`OBJ_BASE` 默认 = `build`，
  非 full 的 profile 落在 `$(OBJ_BASE)_mm_$(MM)`。）

- [ ] **B2.6 提交**

  ```powershell
  cd C:\code\projects
  git add engine/db/Makefile engine/db/tools/mm_profiles.mk
  git commit -m "feat(kernel): Makefile 接入 MM 编译期模态裁剪（源文件过滤 + DB_HAS_* 宏）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

### Task B3：五处 `#if` 裁剪点（组合根与实例关闭路径）

**Files**
- 修改：`engine/db/src/db_internal.h`、`engine/db/src/db.c`、`engine/db/src/db_instance.c`、`engine/db/src/main.c`

**Interfaces**
- 每个模态的**实例字段**、**open 调用**、**close 调用**、**REPL 入口**都被 `#if DB_HAS_<MOD>` 包起来。

**⚠️ 实测发现：设计文档说的"4 处裁剪点"漏了 `main.c`** ——
`main.c` 直接调用 `handle_vec` / `handle_ft` / `handle_geo` / `handle_ts`
（`repl_*.c` 里）与 `handle_blob` / `handle_graph`（**定义在 main.c 自己里**）。
不裁它，裁剪出来的二进制链接不过。本 Task 把它作为**第 5 处**。

- [ ] **B3.1 `src/db_internal.h`：include 守卫**

  把 L17–L38 的 include 块改成：

  ```c
  #include "catalog/catalog.h"
  #if DB_HAS_FT
  #include "modalities/ft/ft_am.h"
  #include "modalities/ft/ft_index.h"
  #endif
  #if DB_HAS_GEO
  #include "modalities/geo/geo_am.h"
  #include "modalities/geo/geo_state.h"
  #endif
  #if DB_HAS_GRAPH
  #include "modalities/graph/graph.h"
  #include "modalities/graph/graph_am.h"
  #endif
  #if DB_HAS_TS
  #include "modalities/ts/ts_am.h"
  #include "modalities/ts/ts_am_ext.h"
  #include "modalities/ts/tsstore.h"
  #endif
  #if DB_HAS_VECTOR
  #include "modalities/vector/hnsw_am.h"
  #include "modalities/vector/vector_am.h"
  #include "modalities/vector/vector_state.h"
  #endif
  #include "query/exec.h"
  #if DB_HAS_BLOB
  #include "spi/blob_am.h"
  #endif
  #include "spi/registry.h"
  #if DB_HAS_BLOB
  #include "storage/blob.h"
  #endif
  #include "storage/pager.h"
  #include "txn/mvcc.h"
  #include "txn/txn.h"
  #include "wal/recovery.h"
  #include "wal/wal.h"
  ```

  ⚠️ **`DB_HAS_*` 必须在 `db_internal.h` 可见**。它由 Makefile 的 `-D` 传进来，
  所以在 include 之前不需要额外处理；但**所有不经 Makefile 的编译路径**
  （比如 `test/run.sh` 自己编测试）也必须带上这些宏 —— 见 B3.6。

- [ ] **B3.1b `src/db_internal.h`：模态函数的**声明**也要守卫（原位包上，不是新增）**

  ⚠️ **（可执行性修正）这一步原来写的是「**新增**这些声明」—— 方向写反了** ——
  `db_internal.h` 里这些声明**本来就存在**（实测：`L233-L371` 连续一段 +
  `L395`），照抄一遍会变成**重复声明**（同签名重复声明在 C 里合法，
  但 `-Wall -Wextra -Werror` 下会报 `-Wredundant-decls` ⇒ **直接编译失败**）。
  更要命的是：原声明**没有被 `#if` 包住** ⇒
  计划想消除的「裁剪后仍有 ft 声明」这种**残留依然存在**。

  ⇒ **改成：把 `db_internal.h` 里**现有的**声明**原位**包上 `#if DB_HAS_*`。**
  一个字都不要新写 —— 只在原有声明的**前后**各加一行 `#if` / `#endif`。

  **实测清单（`src/db_internal.h`，共 18 个函数声明 + 2 个 typedef）：**

  | 模态 | 声明 | 行号 |
  |---|---|---|
  | vector | `db_vec_register` / `db_vec_open` / `db_vec_close` | L233 / L236 / L239 |
  | ft | `db_ft_replay_add` / `db_ft_replay_free` / `db_ft_recovery_hook` / `db_ft_register` / `db_ft_open` / `db_ft_close` | L246 / L249 / L265 / L274 / L281 / L284 |
  | ts | `db_ts_register` / `db_ts_open` / `db_ts_close` | L296 / L304 / L312 |
  | geo | `db_geo_flatten` / `db_geo_tab_free` / `db_geo_register` / `db_geo_open` / `db_geo_close` | L344 / L347 / L355 / L363 / L371 |
  | graph | `db_graph_register` | **L395**（在 `db_wal_hook` 之后，**别漏**） |
  | geo（类型） | `typedef ... GeoPtEntry` / `typedef ... GeoTab` | L329 / L335 |
  | ft（类型） | `typedef struct {...} FtReplaySet` | **L46-L50**（在 `DbInstance` 之前） |

  ⚠️ **`db_internal.h` 里没有 `db_blob_*` 的声明** —— blob 模态没有内部函数声明
  （只有 `db.c` / `db_instance.c` 里直接调的 `blob_store_*` / `blob_am_build_*`，
  它们的头是 `storage/blob.h` / `spi/blob_am.h`，已在 B3.1 里按 `DB_HAS_BLOB` 守 include）。
  ⇒ **不要往头里补 `db_blob_*` 声明**（补了就是「新增」，属于本步要避免的方向）。

  这些声明**不守卫不会编译失败**（声明一个不存在的函数是合法的），
  但会留下"裁剪后的二进制里还有 ft 的声明"这种**误导性残留**，
  而且 `db_ft_recovery_hook` 的**注册点**在 `db.c` 里（`recovery_set_hook(db_ft_recovery_hook, inst)`），
  那处不守卫就会**链接失败**。

  处理（**示意**：下面是「包完之后长什么样」，**不是要你照抄这段代码** ——
  实际动作是给 `db_internal.h` 里**现有的**那 18 个声明**原位**加上 `#if` / `#endif`）：

  ```c
  #if DB_HAS_VECTOR
  int db_vec_register(DbInstance *inst);   /* L233 */
  int  db_vec_open(DbInstance *inst);       /* L236 */
  void db_vec_close(DbInstance *inst);      /* L239 */
  #endif

  #if DB_HAS_FT
  int  db_ft_replay_add(FtReplaySet *set, u32 doc_id);        /* L246 */
  void db_ft_replay_free(FtReplaySet *set);                   /* L249 */
  int db_ft_recovery_hook(void *ctx, u8 type, u32 xid, u32 page_id, u32 lsn,
                          const u8 *payload, u16 payload_len, RecHookPhase phase);  /* L265 */
  int db_ft_register(DbInstance *inst);                      /* L274 */
  int db_ft_open(DbInstance *inst);                          /* L281 */
  void db_ft_close(DbInstance *inst);                        /* L284 */
  #endif

  #if DB_HAS_TS
  int db_ts_register(DbInstance *inst);   /* L296 */
  int db_ts_open(DbInstance *inst);       /* L304 */
  void db_ts_close(DbInstance *inst);     /* L312 */
  #endif

  #if DB_HAS_GEO
  typedef struct { float x; float y; bool dead; } GeoPtEntry;   /* L329 */
  typedef struct { GeoPtEntry *p; u32 cap; u32 n; u32 live; u32 dead_n; } GeoTab;  /* L335 */
  int  db_geo_flatten(DbInstance *inst, GeoTab *tab);   /* L344 */
  void db_geo_tab_free(GeoTab *tab);                   /* L347 */
  int  db_geo_register(DbInstance *inst);               /* L355 */
  int  db_geo_open(DbInstance *inst);                   /* L363 */
  void db_geo_close(DbInstance *inst);                  /* L371 */
  #endif

  #if DB_HAS_GRAPH
  int db_graph_register(DbInstance *inst);   /* L395 */
  #endif
  ```

  ⚠️ **`FtReplaySet` 类型本身**也要守卫（它只被 ft 用，**现有位置 L46-L50**）：

  ```c
  #if DB_HAS_FT
  typedef struct {
      u32 *ids;
      u32 n;
      u32 cap;
  } FtReplaySet;     /* L46-L50 */
  #endif
  ```

  ⚠️ **`FtReplaySet` 与 `db_ft_replay_free` 必须**同时**守卫** ——
  只守声明、不守调用点（或反过来）⇒ 非 ft profile 下
  `db.c:490` / `db.c:491` 会变成**隐式声明**（`-Werror` ⇒ 编译错误）。
  这两处的守卫见 B3.3。

  ⚠️ **`db_wal_hook` 不守卫** —— 它是 `db.c` 实现的、被多个模态共用，
  且 blob/graph/geo/ts/vector 都要它。留在公共区。

- [ ] **B3.2 `src/db_internal.h`：`DbInstance` 字段守卫**

  把 `DbInstance` 里各模态的字段块逐个包起来：

  ```c
  typedef struct DbInstance {
      Pager    *pager;
      Catalog  *cat;
      Wal      *wal;
      TxnTable  txns;

  #if DB_HAS_BLOB
      BlobStore *blobs;       /* blob 模态（实例级共享）*/
  #endif
      SpiRegistry *reg;

  #if DB_HAS_BLOB
      StorageAM  blob_am_frames;
      StorageAM  blob_am_objects;
      BlobAmCtx  blob_ctx_frames;
      BlobAmCtx  blob_ctx_objects;
  #endif

  #if DB_HAS_GRAPH
      Graph      *graph;
      bool        graph_ready;
      StorageAM   graph_am_nodes;
      StorageAM   graph_am_edges;
      GraphAmCtx  graph_ctx_nodes;
      GraphAmCtx  graph_ctx_edges;
  #endif

  #if DB_HAS_VECTOR
      VectorState *vstate;
      bool         vec_ready;
      StorageAM    vec_am;
      VectorAmCtx  vec_ctx;
      IndexAM      vec_index_am;
      HnswAmCtx    vec_index_ctx;
  #endif

  #if DB_HAS_FT
      FtIndex     *ft;
      bool         ft_ready;
      StorageAM    ft_am;
      FtAmCtx      ft_ctx;
      FtReplaySet  ft_replay_redo;
      FtReplaySet  ft_replay_undo;
  #endif

  #if DB_HAS_GEO
      /* geo 模态的实例字段 —— 原文抄自 db_internal.h:166-L170 */
      GeoState    *geo;
      bool         geo_ready;
      StorageAM    geo_am;
      GeoAmCtx     geo_ctx;
      u64          geo_tree_reads_seen;
  #endif

  #if DB_HAS_TS
      /* ts 模态的实例字段 —— 原文抄自 db_internal.h:192-L197 */
      TsStore     *ts;
      bool         ts_ready;
      StorageAM    ts_am_readings;
      StorageAM    ts_am_partitions;
      TsAmExt      ts_ext_readings;
      TsAmExt      ts_ext_partitions;
  #endif

      /* ... 其余字段不变 ... */
  } DbInstance;
  ```

  ⚠️ **（可执行性修正）geo 与 ts 的字段块原来写的是 `...`** ——
  计划自己都承认「本次读取被截断，没拿到确切名字」，
  而这正是执行者**最需要准确**的地方：`...` 一旦被照抄进代码，
  `DbInstance` 就**编译不过**（缺字段）或者**悄悄少了字段**（行为错）。

  ⇒ 上面已经**按 `db_internal.h` 的原文逐字填好**（geo = L166-L170，ts = L192-L197）：

  | 模态 | 行号 | 字段 |
  |---|---|---|
  | geo | `db_internal.h:166` | `GeoState    *geo;` |
  | geo | `db_internal.h:167` | `bool         geo_ready;` |
  | geo | `db_internal.h:168` | `StorageAM    geo_am;` |
  | geo | `db_internal.h:169` | `GeoAmCtx     geo_ctx;` |
  | geo | `db_internal.h:170` | `u64          geo_tree_reads_seen;` |
  | ts | `db_internal.h:192` | `TsStore     *ts;` |
  | ts | `db_internal.h:193` | `bool         ts_ready;` |
  | ts | `db_internal.h:194` | `StorageAM    ts_am_readings;` |
  | ts | `db_internal.h:195` | `StorageAM    ts_am_partitions;` |
  | ts | `db_internal.h:196` | `TsAmExt      ts_ext_readings;` |
  | ts | `db_internal.h:197` | `TsAmExt      ts_ext_partitions;` |

  ⚠️ **搬的时候**：
  - **字段名、类型、缩进一个字都不要改**，只加 `#if` / `#endif`；
  - geo / ts 字段块的**上面那段中文注释**（geo 在 L141-L165、ts 在 L172-L191）
    是**解释性注释**，可以**留在 `#if` 里面**（跟着字段一起裁掉），
    也可以留在外面 —— 但**别删**；
  - `GeoState` / `GeoAmCtx` / `TsStore` / `TsAmExt` 这几个类型来自
    `modalities/geo/geo_state.h` / `modalities/ts/ts_am_ext.h` / `tsstore.h`，
    已在 B3.1 里按 `DB_HAS_*` 守 include ⇒ 字段被裁掉时类型也跟着没了，**不会**留下未用类型。

- [ ] **B3.3 `src/db.c`：open 路径守卫**

  实测的触点，逐个包（⚠️ **行号以当前 `db.c` 为准**）：

  | 行 | 原来 | 处理 |
  |---|---|---|
  | L486 | `recovery_set_hook(db_ft_recovery_hook, inst);` | `#if DB_HAS_FT` … `#endif`（**不守卫会链接失败**：`db_ft_recovery_hook` 已被守卫掉） |
  | L488 | `recovery_set_hook(NULL, NULL);` | **不守卫** —— 注销空钩子无害 |
  | **L490** | `db_ft_replay_free(&inst->ft_replay_redo);` | `#if DB_HAS_FT` … `#endif`（⚠️ **原来漏了**） |
  | **L491** | `db_ft_replay_free(&inst->ft_replay_undo);` | `#if DB_HAS_FT` … `#endif`（⚠️ **原来漏了**） |
  | L506 | `rc = db_ft_open(inst);` | `#if DB_HAS_FT` … `#endif` |
  | L531 | `rc = blob_store_create(pg, db_wal_hook, inst, &inst->blobs);` | `#if DB_HAS_BLOB` … `#endif` |
  | **L556** | `rc = blob_am_build_frames(inst->blobs, &inst->blob_am_frames, &inst->blob_ctx_frames);` | `#if DB_HAS_BLOB` … `#endif`（⚠️ **边界原来没写清**） |
  | **L559** | `rc = blob_am_build_objects(inst->blobs, &inst->blob_am_objects, &inst->blob_ctx_objects);` | 同上（在同一个 `#if DB_HAS_BLOB` 块内） |
  | L562 | `rc = spi_registry_add(inst->reg, "blob_frames", &inst->blob_am_frames);` | `#if DB_HAS_BLOB`（与 L531/L556/L559/L564 同一块） |
  | L564 | `rc = spi_registry_add(inst->reg, "blob_objects", &inst->blob_am_objects);` | 同上 |
  | L596 | `rc = db_graph_register(inst);` | `#if DB_HAS_GRAPH` … `#endif` |
  | L622 | `int vrc = db_vec_open(inst);` | `#if DB_HAS_VECTOR` … `#endif` |
  | L641 | `rc = db_ft_register(inst);` | `#if DB_HAS_FT` … `#endif` |
  | L663 | `int grc = db_geo_open(inst);` | `#if DB_HAS_GEO` … `#endif` |
  | L687 | `int trc = db_ts_open(inst);` | `#if DB_HAS_TS` … `#endif` |

  ⚠️ **（可执行性修正）L490 / L491 原来根本没列进来** ——
  `db_ft_replay_free(&inst->ft_replay_redo)` / `..._undo` 这两处调用
  在 `db.c:490-491`（`recovery_run` 失败后的清理分支里）。
  B3.1b 已经把 `db_ft_replay_free` 的**声明**包进 `#if DB_HAS_FT`，
  而这两处**调用**没被守卫 ⇒ 非 ft profile 下：
  - 声明被裁掉 ⇒ 调用变成**隐式声明**（C11 下是错误，
    `-Wall -Wextra -Werror` ⇒ **直接编译失败**）；
  - 就算侥幸编过，链接期也找不到 `db_ft_replay_free` ⇒ **链接失败**。
  ⇒ **声明与调用必须同时守卫**，这两行就是那个配对。

  ⚠️ **（可执行性修正）blob 那一块的边界原来只写了「与 L531 同一块」** ——
  「同一块」是个**含糊**说法：执行者不知道该 `#if` 到哪一行结束。
  ⇒ 确切边界：**L556 到 L564**（`blob_am_build_frames` / `blob_am_build_objects`
  / `spi_registry_add("blob_frames")` / `spi_registry_add("blob_objects")`），
  其中 L556-L560 是一个 `if (rc == OK)` 链、L561-L564 是另一个 `if (rc == OK)` 链。
  ⇒ 整个 **L556-L564** 放进**同一个** `#if DB_HAS_BLOB` … `#endif`，
  `#endif` 放在 L564 之后、L565 `if (rc != OK) {` **之前**。

  ⚠️ **被 `#if` 掉的分支里如果有变量声明，被跳过后就"未使用" ⇒ `-Werror` 报错。**
  处理办法二选一：
  - 把该分支里的局部变量**一起**放进 `#if` 块（推荐）；
  - 或者在 `#else` 分支里补 `int vrc = 0;` 之类（不推荐，容易漏）。

  **逐个分支检查**：编译一遍，编译器会精确指出哪个变量没用了。

  ⚠️ **不要按行号区间替换** —— 行号会随前面的改动漂移。
  ⇒ **按「函数 + 语句内容」定位**（用 `Select-String` 找那一行），
  再把 `#if` / `#endif` 插在它前后。

- [ ] **B3.4 `src/db.c`：close 路径守卫**

  ⚠️ **（可执行性修正）原来写的是「L200–L205」，实际是「L200–L206」** ——
  `open_fail_cleanup()` 里连续 7 行（L200-L206），中间夹着
  **两行不是模态专属的**、**不能被裁掉**的调用：

  ```
  L200     db_ft_close(inst);
  L201     db_geo_close(inst);
  L202     db_ts_close(inst);
  L203     graph_destroy(inst->graph);
  L204     spi_registry_destroy(inst->reg);     ← 底座（注册表），不守卫
  L205     blob_store_destroy(inst->blobs);
  L206     catalog_close(inst->cat);            ← 底座（catalog），不守卫
  ```

  ⇒ 原计划只列了 5 行、且行号区间也少了 1 ——
  **照着改会把 L204 / L206 两行删掉**（注册表与 catalog 泄漏、catalog 句柄悬空）。

  ⇒ **正确做法**：把 L200-L206 整块抄下来，只给**模态专属的那 5 行**加守卫，
  L204 / L206 **原样保留**：

  ```c
  #if DB_HAS_FT
      db_ft_close(inst);
  #endif
  #if DB_HAS_GEO
      db_geo_close(inst);
  #endif
  #if DB_HAS_TS
      db_ts_close(inst);
  #endif
  #if DB_HAS_GRAPH
      graph_destroy(inst->graph);
  #endif
      spi_registry_destroy(inst->reg);     /* L204 —— 底座，不守卫，原样保留 */
  #if DB_HAS_BLOB
      blob_store_destroy(inst->blobs);
  #endif
      catalog_close(inst->cat);            /* L206 —— 底座，不守卫，原样保留 */
  ```

  ⚠️ **不要按行号区间整体替换** —— 行号会随前面的改动漂移。
  ⇒ 按 `open_fail_cleanup()` 这个**函数**定位，往里逐行加守卫。

- [ ] **B3.5 `src/db_instance.c`：两处关闭路径守卫**

  `instance_close()`（L55 / L67 / L78 / L90 / L125 / L126）与
  `db_abandon()`（L186 / L187 / L196-198 / L207 / L213 / L219）：

  ```c
  #if DB_HAS_VECTOR
      db_vec_close(inst);
  #endif
  #if DB_HAS_FT
      db_ft_close(inst);
  #endif
  #if DB_HAS_GEO
      db_geo_close(inst);
  #endif
  #if DB_HAS_TS
      db_ts_close(inst);
  #endif
  #if DB_HAS_BLOB
      blob_store_destroy(inst->blobs);
  #endif
  #if DB_HAS_GRAPH
      graph_destroy(inst->graph);
  #endif
  ```

  `db_abandon()` 里那段释放向量内存的（L193–L201）：

  ```c
  #if DB_HAS_VECTOR
          if (inst->vstate) {
              VectorState *st = inst->vstate;
              if (st->hnsw)
                  hnsw_destroy(st->hnsw);
              vindex_destroy(st->vindex);
              vstore_destroy(st->vs);
              free(st);
              inst->vstate = NULL;
          }
  #endif
  ```

- [ ] **B3.6 `src/main.c`：第 5 处裁剪点**

  - include 守卫（L62–L65）：

    ```c
    #if DB_HAS_FT
    #include "repl_ft.h"
    #endif
    #if DB_HAS_GEO
    #include "repl_geo.h"
    #endif
    #if DB_HAS_TS
    #include "repl_ts.h"
    #endif
    #if DB_HAS_VECTOR
    #include "repl_vec.h"
    #endif
    ```

  - `handle_blob`（L113 起）与 `handle_graph`（L393 起）是 **static 函数，定义在 main.c 里** ——
    整个函数体包进 `#if DB_HAS_BLOB` / `#if DB_HAS_GRAPH`。
    ⚠️ 包住之后**不能有未使用的 static 函数**（`-Werror` 的 `-Wunused-function`）——
    所以是整个函数连同它的 static 辅助函数一起包。

  - dispatch 守卫（L594 / L600 / L606 / L612 / L618 / L630）：

    ```c
                #if DB_HAS_BLOB
                if (strncmp(p, "BLOB", 4) == 0) {
                    handle_blob(db, line);
                    len = 0;
                    sql[0] = '\0';
                    continue;
                }
                #endif
    ```

    其余 5 处同理。

- [ ] **B3.7 让 `test/run.sh` 也带上 `-DDB_HAS_*`**

  ⚠️ **`test/run.sh` 自己编测试**（不走 Makefile），所以它必须也传这些宏 ——
  否则测试文件编译时会因为 `DB_HAS_FT` 未定义而报错
  （`#if DB_HAS_FT` 在未定义时求值为 0 ⇒ 所有模态都被裁掉 ⇒ 测试跑的是残缺的内核）。

  `test/run.sh` 的 CFLAGS 改成：

  ```sh
  CFLAGS="-std=c11 -Wall -Wextra -Werror -O1 -g -fno-strict-aliasing \
  $SAN -Iinclude -Isrc"
  ```

  →

  ```sh
  # 模态开关宏：与 Makefile 的 MM_FLAGS 保持一致（全开）。
  # test/run.sh 自己编测试，不走 Makefile ⇒ 宏必须在这里补上，
  # 否则 #if DB_HAS_* 在未定义时全为 0 ⇒ 测试跑的是残缺内核。
  MM_FLAGS="-DDB_HAS_RELATIONAL=1 -DDB_HAS_BLOB=1 -DDB_HAS_GRAPH=1 \
  -DDB_HAS_VECTOR=1 -DDB_HAS_FT=1 -DDB_HAS_GEO=1 -DDB_HAS_TS=1 -DDB_HAS_DOC=1"
  CFLAGS="-std=c11 -Wall -Wextra -Werror -O1 -g -fno-strict-aliasing \
  $SAN $MM_FLAGS -Iinclude -Isrc"
  ```

  ⚠️ **更好的做法**：让 `run.sh` 从 `tools/mm_profiles.mk` 读同一份定义，
  免得两处手写、迟早不一致。可以在 `run.sh` 里加：

  ```sh
  # 从 mm_profiles.mk 派生 -DDB_HAS_*（与 Makefile 同一份真相源）
  MM_FLAGS=""
  for m in relational blob graph vector ft geo ts doc; do
      up=$(printf '%s' "$m" | tr '[:lower:]' '[:upper:]')
      MM_FLAGS="$MM_FLAGS -DDB_HAS_$up=1"
  done
  ```

- [ ] **B3.7b `test/run.sh`：对象目录也不能写死 `build`**

  ⚠️ `run.sh` 里现在**硬编码**了 `build`：

  ```sh
  mkdir -p build test/tmp build/test-logs
  OBJS=$(find build -name '*.o' ! -name 'main.o' ! -path 'build/bench/*' 2>/dev/null || true)
  ```

  裁剪 profile 的对象在 `build_mm_<m>/` 里 ⇒ `run.sh` 会链到**上一轮 full 的旧对象**
  （或者空目录 ⇒ 链接失败）。改成从环境变量取：

  ```sh
  OBJ_DIR=${OBJ_DIR:-build}
  mkdir -p "$OBJ_DIR" test/tmp "$OBJ_DIR/test-logs"
  # 排除 bench/ —— 基准程序自己带 main，链进来会重复定义。
  OBJS=$(find "$OBJ_DIR" -name '*.o' ! -name 'main.o' ! -path "$OBJ_DIR/bench/*" 2>/dev/null || true)
  ```

  ⚠️ **同一处还有三处 `build/` 要跟着改**（否则裁剪 profile 下测试会跑不起来）：
  - `log="build/test-logs/$name.log"` → `log="$OBJ_DIR/test-logs/$name.log"`
  - `rc="build/test-logs/$name.rc"` → `rc="$OBJ_DIR/test-logs/$name.rc"`
- `-o "build/$name"` → `-o "$OBJ_DIR/$name"`
- ⚠️ **（复审修正）漏了 L92 的执行路径** —— `if "./build/$name" >>"$log" 2>&1; then`
  → `if "./$OBJ_DIR/$name" >>"$log" 2>&1; then`
  ⚠️ **不漏这一行的后果特别阴**：编译产物已经改到 `$OBJ_DIR/$name`，
  执行路径却还指向 `./build/$name` ⇒ 裁剪 profile 下**跑的是上一次 full 的旧二进制** ——
  正是本文件头部注释里点名的那个坑（改了源码、编译「成功」、跑的是旧二进制）。
  ⇒ 改完必须**逐条核对 7 处**（`Select-String -Path test/run.sh -Pattern 'build/'`）：
  L60 `mkdir -p "build"`、L64 `OBJS=$(find "build" …)`、L74 `log="build/…"`、
  L75 `rc="build/…"`、L87 `-o "build/$name"`、**L92 `"./build/$name"`**、L124 `rm -f build/…`
  （另有 L179 / L196 / L198-202 的汇总与失败详情）。
  - `rm -f build/test-logs/*.rc` → `rm -f "$OBJ_DIR"/test-logs/*.rc`
  - 汇总里的 `"build/test-logs/$name.rc"` → `"$OBJ_DIR/test-logs/$name.rc"`
  - 失败详情里的 `build/test-logs/$name.log` → `"$OBJ_DIR/test-logs/$name.log"`

  ⚠️ **`Makefile` 的 `test` 目标要把 `OBJ_DIR` 传下去**：

  ```make
  test: $(TARGET)
  	@OBJ_DIR=$(OBJ_DIR) sh test/run.sh
  ```

  ⚠️ **默认行为不变**：`OBJ_DIR` 未设置时 `run.sh` 用 `build`，
  与今天完全一致。

  ⚠️ **裁剪 profile 下跑全量测试没有意义**（大部分测试依赖被裁掉的模态）——
  `make MM=vector test` 会大面积链接失败，这是**预期的**。
  裁剪 profile 的验证手段是 B4 的 smoke，不是全量测试。

  （`tr` 在 Git Bash 与 Linux 上都有；`run.sh` 只在 sh 环境跑，不受 Makefile 的
  `$(shell)` 限制。）

- [ ] **B3.8 逐个 profile 编译验证**

  ```powershell
  cd C:\code\projects\engine\db
  foreach ($m in 'full','relational','blob','graph','vector','ft','geo','ts','doc') {
      "--- MM=$m ---"
      mingw32-make MM=$m clean
      mingw32-make MM=$m all 2>&1 | Select-Object -Last 5
  }
  ```

  预期：9 个 profile **全部编译 + 链接成功**。

  ⚠️ **少一处 `#if` 就会在这里报 undefined reference** —— 这正是这个 Task 的验证点。

- [ ] **B3.9 全量测试 + 符号验证**

  ```powershell
  mingw32-make clean
  mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：95 通过 / 0 失败。

  裁剪验证：

  ```powershell
  mingw32-make MM=vector clean
  mingw32-make MM=vector all
  nm build_mm_vector/db.exe | Select-String 'db_ft_open|db_graph_open|db_ts_open|db_geo_open'
  ```

  预期：**零命中**（被裁掉的模态的符号不在二进制里）。

  ```powershell
  nm build_mm_vector/db.exe | Select-String 'heap_am_build|catalog_find_table'
  ```

  预期：**有命中**（关系模态恒常驻）。

- [ ] **B3.10 提交**

  ```powershell
  cd C:\code\projects
  git add engine/db/src/db_internal.h engine/db/src/db.c engine/db/src/db_instance.c `
          engine/db/src/main.c engine/db/test/run.sh
  git commit -m "feat(kernel): 五处 DB_HAS_* 裁剪点（实例字段 / open 路径 / close 路径 / REPL 入口）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

### Task B4：8 个 smoke —— 链接期的模态独立证明

**Files**
- 新建：`engine/db/test/smoke/smoke_*.c`（8 个）
- 修改：`engine/db/Makefile`

**Interfaces**
- 新增 make 目标：`smoke`（一次构建 8 个）、`smoke_<m>`（单个）。
- 新增产物：`build/smoke_<m>.exe`。

**这个 Task 证明什么**：链接通过 ⇒ 模态 `<m>` 的代码**没有**引用任何别的模态的符号。
这是"模态之间零耦合"的**机器可验证**证据。

- [ ] **B4.1 决定链接集合**

  smoke `<m>` 的链接集合必须是：

  ```
  底座（common/storage/wal/index/txn/catalog/query/spi + db_ckpt/db_result/db_stmt）
  + 模态 <m> 的源文件
  + **只打开 <m> 的根**（db.o / db_instance.o，单独编译一份）
  + smoke_<m>.o（自带 main）
  ```

  ⚠️ **根为什么要单独编译**：组合根 `db.c` / `db_instance.c` 调用各模态的
  open/close。若直接用 full 的根对象文件，链接器会把**所有**模态的符号都拖进来
  ⇒ 证明作废。所以每个模态单独编一份根（`DB_HAS_*` 只开它自己）。

  ⚠️ **`main.o` 不进 smoke 链接集合**（smoke 自己带 main，会重复定义）。

- [ ] **B4.2 Makefile：加 smoke 目标**

  在 `Makefile` 的 `LIB_OBJS` 定义之后加：

  ```make
  # ---------------------------------------------------------------------
  # smoke：每个模态一个独立可执行文件
  # ---------------------------------------------------------------------
  # 【它证明什么】
  # smoke_<m> 的链接集合 = 底座 + 模态 <m> + 只开 <m> 的根。
  # 链接能过 ⇒ 模态 <m> **没有**引用任何别的模态的符号。
  # 这是"模态之间零耦合"的机器可验证证据。
  #
  # 【为什么根要单独编】
  # 见 B4.1。
  SMOKE_MODS := relational $(MM_CROPPABLE)

  # 只打开模态 $(1) 的 -DDB_HAS_* 组合
  # ⚠️ **（复审修正）原来的写法在 `define` 之外** —— `$(1)` 在顶层是**空的**，
  # 变量名会退化成 `MM_FLAGS_ONLY_` / `MOD_OBJS_`（所有模态共用一个名字）。
  # ⇒ 改成用 `$(foreach ... $(eval ...))` 逐模态求值，名字里带上模态名。
  $(foreach m,$(SMOKE_MODS),\
    $(eval MM_FLAGS_ONLY_$(m) := $(foreach x,$(MM_ALL),-DDB_HAS_$(MM_UP_$(x))=$(if $(filter $(x),$(m)),1,0))))

  # 底座对象（不含 main.o / bench / **任何模态对象**）
  # ⚠️ **（复审修正）原来排除的是 `$(MM_CROP_SRCS)`（只含被裁掉的模态）** ——
  # 在 `MM=full` 下 `MM_CROP_SRCS` 是空的 ⇒ 底座集合 = **全部源文件**（含 8 个模态）
  # ⇒ smoke 把所有模态都链进去了 ⇒「链接期独立证明」作废，
  # 而且 `MOD_OBJS_$(1)` 与 `SUBSTRATE_OBJS` 会**重复包含同一批对象文件**。
  # ⇒ 排除**全部**模态的文件（不管裁没裁）。
  MM_ALL_MOD_SRCS := $(foreach m,$(MM_ALL),$(MM_FILES_$(m)))
  # ⚠️ **（可执行性修正）底座集合必须把「根」也排除掉** ——
  # `$(SRCS_ALL)` 里**含** `src/db.c` 与 `src/db_instance.c`，
  # 而 smoke 的链接行**又单独加**了 `$(OBJ_DIR)/smoke_$(1)/db.o` 与
  # `$(OBJ_DIR)/smoke_$(1)/db_instance.o`（B4.1 已述：根要**单独编译**，
  # 只开一个模态的 `DB_HAS_*`）。
  # 两个 db.o / 两个 db_instance.o 同时进链接行 ⇒
  #   db.o         定义 `db_open`（db.c:218）、`db_connect`（db.c:728）
  #   db_instance.o 定义 `db_close`（db_instance.c:133）
  # ⇒ **multiple definition** ⇒ **8 个 smoke 全部链接失败**。
  # ⇒ 根既然要单独编译，就必须同时从底座集合里 filter-out。
  SUBSTRATE_SRCS := $(filter-out $(MM_ALL_MOD_SRCS) $(SRC_DIR)/main.c $(SRC_DIR)/db.c $(SRC_DIR)/db_instance.c,$(SRCS_ALL))
  SUBSTRATE_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SUBSTRATE_SRCS))

  # 模态 $(1) 的对象
  $(foreach m,$(SMOKE_MODS),\
    $(eval MOD_OBJS_$(m) := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(MM_FILES_$(m)))))
  ```

  再加 per-modality 规则：

  ```make
  # ⚠️ **（复审修正）smoke 用 `CFLAGS_BASE`，不用 `CFLAGS`** ——
  # `CFLAGS` 里已经含 `$(MM_FLAGS)`（全开组合），
  # 再叠加 `$(MM_FLAGS_ONLY_$(1))`（单模态组合）⇒ 同一个 `-DDB_HAS_X` 出现两次、
  # 值还不一样 ⇒ GCC 报**宏重定义**警告 ⇒ `-Werror` 下**直接编译失败**。
  # ⇒ 把基础 flags 单独拆一个变量 `CFLAGS_BASE`：
  #    CFLAGS_BASE = $(CSTD) $(WARN) $(OPT) $(HARDEN) $(INC) $(SAN)
  #    CFLAGS      = $(CFLAGS_BASE) $(MM_FLAGS)          # 正常构建用
  # smoke 的编译/链接行一律用 `$(CFLAGS_BASE)` + `$(MM_FLAGS_ONLY_$(m))`。

  define MM_SMOKE_RULE
  define MM_SMOKE_RULE
  # 只开模态 $(1) 的根
  $(OBJ_DIR)/smoke_$(1)/db.o: $(SRC_DIR)/db.c
  	@mkdir -p $$(@D)
  	$(CC) $(CFLAGS_BASE) $(MM_FLAGS_ONLY_$(1)) -c $$< -o $$@

  $(OBJ_DIR)/smoke_$(1)/db_instance.o: $(SRC_DIR)/db_instance.c
  	@mkdir -p $$(@D)
  	$(CC) $(CFLAGS_BASE) $(MM_FLAGS_ONLY_$(1)) -c $$< -o $$@

  # smoke 的 main
  $(OBJ_DIR)/smoke_$(1).o: test/smoke/smoke_$(1).c
  	@mkdir -p $$(@D)
  	$(CC) $(CFLAGS_BASE) -c $$< -o $$@

  # 链接
  $(OBJ_DIR)/smoke_$(1).exe: $(OBJ_DIR)/smoke_$(1).o $(OBJ_DIR)/smoke_$(1)/db.o \
                             $(OBJ_DIR)/smoke_$(1)/db_instance.o \
                             $(SUBSTRATE_OBJS) $(MOD_OBJS_$(1))
  	$(CC) $(CFLAGS_BASE) -o $$@ $$^ $(LDLIBS)
  endef

  $(foreach m,$(SMOKE_MODS),$(eval $(call MM_SMOKE_RULE,$(m))))

  smoke: $(foreach m,$(SMOKE_MODS),$(OBJ_DIR)/smoke_$(m).exe)
  .PHONY: smoke
  ```

  ⚠️ **`.PHONY` 清单（L272 附近）要加上 `smoke`。**

- [ ] **B4.3 写 8 个 smoke 的 main**

  每个 smoke 的形状：**开一个库 → 调该模态的一个公开 API → 关库 → 打印 OK**。
  它不需要测功能对不对（那是单元测试的事），它只需要**能链接 + 能跑完**。

  `test/smoke/smoke_relational.c`：

  ```c
  /*
   * smoke_relational.c —— 关系模态的链接期独立证明
   *
   * 【这个文件证明什么】
   * 它只链接"底座 + 关系模态"。链接能过 ⇒ 关系模态没有引用别的模态。
   * 它不测功能对不对（那是 test/unit/ 的事）。
   */
  #include <stdio.h>

  #include "db.h"

  int main(void)
  {
      DB *db = NULL;
      db_err rc = db_open("test/tmp/smoke_relational.db", &db);
      if (rc != DB_OK) {
          fprintf(stderr, "smoke_relational: db_open 失败: %d\n", rc);
          return 1;
      }

      Result *r = NULL;
      db_err e = db_exec(db, "CREATE TABLE t(a INT)", &r);
      if (e != DB_OK) {
          fprintf(stderr, "smoke_relational: CREATE TABLE 失败: %s\n", db_strerror(e));
          db_close(db);
          return 1;
      }
      db_result_free(r);

      e = db_exec(db, "INSERT INTO t VALUES (1)", &r);
      if (e != DB_OK) {
          fprintf(stderr, "smoke_relational: INSERT 失败: %s\n", db_strerror(e));
          db_close(db);
          return 1;
      }
      db_result_free(r);

      printf("smoke_relational: OK\n");
      db_close(db);
      return 0;
  }
  ```

  ⚠️ **（可执行性修正）`db_open` 的签名原来写错了** ——
  计划里写的是 `DB *db = db_open("test/tmp/smoke_relational.db");`，
  而 `include/db.h:28` 的**真实签名**是：

  ```c
  db_err db_open(const char *path, DB **out);
  ```

  ⇒ **返回值是 `db_err`，句柄通过出参 `DB **out` 回填** ——
  原来那行**根本编不过**（返回值类型不匹配 + 参数个数不对），
  而 `smoke_relational.c` 是**全计划里唯一的完整样例** ——
  它编不过 ⇒ 其余 7 个 smoke 会被**照着抄**，8 个全挂。

  ⇒ 正确写法（`db_err` + 出参 + `DB_OK` 比较）：

  ```c
  DB *db = NULL;
  db_err rc = db_open("test/tmp/smoke_relational.db", &db);
  if (rc != DB_OK) {
      fprintf(stderr, "db_open failed: %d\n", rc);
      return 1;
  }
  ```

  ⇒ 只有这**三行**成立之后，后面的 `db_exec(db, ...)` / `db_close(db)` 才成立。
  ⇒ 同理，**其余 7 个 smoke 也一律用这个开库形状**。
  ⚠️ `DB_OK` 是 `db.h:16` 的枚举值（`db_err` 枚举的第一项 = 0）；
  `db_strerror(db_err)`（`db.h:111`）可拿错误文案。

  ⚠️ **`db_open` 的路径参数**：已从 `include/db.h:28` 确认 ——
  是**单个库文件路径**（不是目录）。临时库放在 `test/tmp/`（`.gitignore` 已覆盖）。
  ⇒ 每个 smoke 用**自己的**文件名（`smoke_<m>.db`），别共用一个 ——
  共用会互相污染，且 `MM=vector` 与 `MM=blob` 的库结构不同。

  ⚠️ **（复审修正）smoke 的内容要求被削弱了** —— 设计 §4.3 要求
  「初始化 → 建最小对象 → **写一条 → 读回 → 断言** → 关闭」，
  原计划里除 `smoke_relational` 外只调 `db_*_exists(db)`，
  **既不建对象、也不写、也不读** —— 那样只能证明「库能链接、能初始化」，
  证不了「这个模态真的能用」。

  ⇒ **每个模态的 smoke 都要走完五步**：建一个最小对象 → 写一条 → 读回 →
  断言读回来的内容 == 写进去的内容 → 关闭。
  （`smoke_doc` 例外：文档模态是寄生模态、没有虚拟表，走
  `json_parse` + `jsonb_encode`/`jsonb_decode` 往返断言。）

其余 7 个照这个形状写，每个换一个模态：

  | smoke | 调什么（**只用 `include/db.h` 里真实存在的 API**） | 备注 |
  |---|---|---|
  | `smoke_blob.c` | `db_blob_stat(db, id, &nframes, &frame_size, &total_len)` + `db_blob_count(db)` + `db_blob_page_reads(db)` | ⚠️ **（可执行性修正）** `db_blob_exists` **全仓不存在**；`db_blob_stat` 的真实签名是 `db_err db_blob_stat(DB *, db_blob_id, uint64_t *nframes, uint32_t *frame_size, uint64_t *total_len)`（`db.h:180`）—— **4 个出参**，少一个就编不过 |
  | `smoke_graph.c` | `db_graph_exists(db)` + `db_graph_count(db, &nodes, &edges)` + `db_graph_reads(db, &node_reads, &edge_reads)` | ⚠️ **（可执行性修正）** `db_graph_stat` **全仓不存在**；`db_graph_exists` 在 `db_graph.c:239`（`db.h:274`），`db_graph_count` 在 `db.h:334`，`db_graph_reads` 在 `db.h:350` |
  | `smoke_vector.c` | `db_vec_exists(db)` | `db.h:388` |
  | `smoke_ft.c` | `db_ft_exists(db)` | `db.h:554` |
  | `smoke_geo.c` | `db_geo_exists(db)` | `db.h:610` |
  | `smoke_ts.c` | `db_ts_exists(db)` | `db.h:725` |
  | `smoke_doc.c` | `json_parse` + `jsonb_encode`/`jsonb_decode` 往返 | 文档模态**还没有公开 API**（W-C 才建 `db_json_*`），这里直接调内部 API 做一次往返 |

  ⚠️ **（可执行性修正）表里原来有两个**不存在的** API** ——
  `smoke_blob.c` 写的 `db_blob_exists(db)` 与 `smoke_graph.c` 写的
  `db_graph_stat(db, ...)` 在 `include/db.h` 里**都没有**（也不在别处）。
  ⇒ 照抄 ⇒ **链接期 undefined reference** ⇒ smoke 链接失败，
  而 smoke 的**全部价值**就是「能链接」⇒ 两个 smoke 直接失去意义。

  ⇒ **存在**的对应物（`include/db.h` 实测）：
  - blob：`db_blob_stat`（L180）/ `db_blob_count`（L189）/ `db_blob_page_reads`（L200）
    —— **没有** `db_blob_exists`；
  - graph：`db_graph_exists`（L274）/ `db_graph_count`（L334）/ `db_graph_reads`（L350）
    —— **没有** `db_graph_stat`。

  ⚠️ **写 smoke 时的硬规矩**：每个调用的 API 都必须在 `include/db.h` 里
  **grep 得到**；找不到就**换成一个找得到的**。
  ⇒ 写完 8 个 smoke 后跑一遍：

  ```powershell
  # 把 smoke 里调用的 db_* 全部列出来，逐个确认在 db.h 里有声明
  Select-String -Path test/smoke/*.c -Pattern '\bdb_[a-z_]+\s*\(' -AllMatches |
      ForEach-Object { $_.Matches.Value } | Sort-Object -Unique
  ```

  ⇒ 拿这个清单去 `include/db.h` 里逐个核对，**一个都不能缺**。

  ⚠️ **`smoke_doc.c` 要 include `modalities/doc/json_parse.h` 与 `modalities/doc/jsonb.h`**
  （内部头）。这**不违反分层** —— smoke 在 `test/` 下，不在 `src/query/` 里，
  `check_deps.sh` 的 `forbid src/query modalities/` 规则管不到它。

- [ ] **B4.4 构建并跑 8 个 smoke**

  ```powershell
  cd C:\code\projects\engine\db
  mingw32-make clean
  mingw32-make all
  mingw32-make smoke
  ```

  预期：8 个 `smoke_<m>.exe` 全部链接成功。

  逐个跑：

  ```powershell
  foreach ($m in 'relational','blob','graph','vector','ft','geo','ts','doc') {
      "--- $m ---"
      & ".\build\smoke_$m.exe"
  }
  ```

  预期：每个都打印 `smoke_<m>: OK`，退出码 0。

- [ ] **B4.5 验证 smoke 真的没拖进别的模态**

  ```powershell
  nm build/smoke_vector.exe | Select-String 'db_ft_open|db_graph_open|db_ts_open|db_geo_open|db_blob_open'
  ```

  预期：**零命中**。

  ```powershell
  nm build/smoke_vector.exe | Select-String 'db_vec_open|heap_am_build'
  ```

  预期：**有命中**。

- [ ] **B4.6 提交**

  ```powershell
  cd C:\code\projects
  git add engine/db/Makefile engine/db/test/smoke/
  git commit -m "feat(kernel): 8 个模态 smoke —— 链接期的模态独立证明"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

### Task B5：门禁加固 + 构建度量

**Files**
- 修改：`engine/db/tools/check_deps.sh`
- 新建：`engine/db/tools/measure_build.sh`
- 新建：`engine/db/docs/MM-PROFILES.md`

**Interfaces**
- `check_deps.sh` 新增一条规则：`src/query/` 不许 include 模态头。
- `measure_build.sh`：输出每个 profile 的编译文件数 / 目标文件数 / 二进制符号数 / 构建耗时。
  **外加一列「代码行数」** —— 设计 §2.13 R-06 要求：模态的权威行数由本脚本产出，
  **不手写数字**（树在长，手写的数字必然过期 —— 本项目已经吃过一次亏：
  设计文档里的体量数字曾整体偏小 20%）。

- [ ] **B5.1 `check_deps.sh` 加「query 不许碰模态」**

  在「L3 可以依赖 index/txn/catalog」那条之后加：

  ```sh
  # L3 的 query/ 不许依赖模态 —— 模态的知识只能经 spi/ 传进来。
  #
  # 【为什么单列一条】
  # 上面的 forbid() 只能挡"目录前缀"，挡不住
  #   #include "storage/heap.h"   （堆是关系模态的实现，但住在 storage/）
  #   #include "modalities/ft/ft_am.h"  （目录前缀能挡，但换个文件名就漏）
  # 所以这里按**模态标志词**扫一遍 include 行。
  forbid src/query heap blob graph vector hnsw ft_ geo ts_ json modalities
  ```

  ⚠️ **（复审修正）`forbid` 的实现是「子串匹配」，不是前缀匹配** ——
  `check_deps.sh:11-15` 的实现是 `#include "[^"]*${bad}` ——
  `bad` 前面**没有**路径分隔符约束，所以它匹配的是**整行里的任意子串**。

  ⇒ 结论（能挡住 `storage/heap.h`）**不变**，但**误伤面比"前缀匹配"大**：
  - `bad="json"` 会匹配任何含 `json` 的 include 行 —— 将来若出现
    `#include "common/jsonb.h"` 这类**合法的公共头**，这条规则会误杀。
  - `bad="ts_"` 会匹配任何含 `ts_` 的路径 —— 同理。
  - `bad="geo"` 较短，误伤面最大。

  ⚠️ **因此这份禁止词表必须随代码演进复核**，每加一个模态就要检查一次。

  ⇒ **更稳的做法**（可选，本轮不做）：把禁止词改成带路径分隔的形式，
  例如 `bad="/ft/"`、`bad="/blob_am.h"`，只挡**目录/文件名**，
  不挡公共头里的普通单词。

  ⚠️ 但 `ft_` / `ts_` / `geo` 这几个词要注意误伤：
  - `ts_` 会匹配 `#include "query/..."` 吗？不会（`query` 里没有 `ts_`）。
  - `ft_` 会匹配 `#include "storage/fast.h"` 吗？不存在这种文件。
  - `json` 会匹配 `#include "storage/json.h"` 吗？不存在。
  - 跑一遍就知道 —— **B5.3 有反例验证**。

- [ ] **B5.1b 加第二条门禁：模态适配器必须在 `src/modalities/` 下**（S9②）

  ⚠️ **（复审修正）设计 §6.3 的 S9 有两条子规则，原来只落了第一条** ——
  ① `src/query/` 禁 include 模态头（B5.1 已做）、
  ② **模态适配器必须在 `src/modalities/` 下**（原来没有可机械验证的落点）。

  ⇒ 在 `check_deps.sh` 里加一条**按文件名**的规则：

  ```sh
  # S9②：模态适配器（*_am.c）必须住在 src/modalities/<模态>/ 下，
  # 不能留在 src/spi/ 里 —— spi/ 只放**契约**，不放实现。
  bad_am=$(find src/spi -name '*_am.c' 2>/dev/null || true)
  if [ -n "$bad_am" ]; then
      echo "✗ 模态适配器实现不许留在 src/spi/（应移到 src/modalities/<m>/）："
      echo "$bad_am"
      exit 1
  fi
  ```

  ⚠️ **这条规则与 W-C C2 / C3 的关系**：
  `heap_am.c` / `heap_am_batch.c` / `btree_am.c` / `blob_am.c`
  在 W-C C2/C3 里**搬到** `src/modalities/relational/` 与 `src/modalities/blob/`。
  ⇒ 这条门禁**在 W-C C2/C3 完成前必然红** —— 这是对的，
  它就是"C2/C3 没做完"的信号。
  ⇒ **执行顺序**：W-B P2 的这条门禁**必须排在 W-C C2/C3 之后**，
  或者本步只把规则写进 `check_deps.sh` 但**先不启用**，等 C2/C3 落地再开。

  ⚠️ **反例验证也要做**（同 B5.2 的做法）：
  临时在 `src/spi/` 放一个 `fake_am.c` ⇒ 门禁必须红 ⇒ 删掉。

- [ ] **B5.2 反例验证（门禁必须会红）**

  临时在 `src/query/exec_scan.c` 顶部加一行：

  ```c
  #include "modalities/ft/ft_am.h"
  ```

  ```powershell
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  ```

  预期：输出 `✗ src/query 不允许依赖 ft_` 并**退出码非 0**。

  ⚠️ **（复审修正）不要用 `git checkout` 撤销** ——
  `git checkout engine/db/src/query/exec_scan.c` 会把该文件**回退到 HEAD**，
  连带撤销 W-C C1.6 / C5.6 在它上面的改动（执行器脱钩 + `json_match` 接线）。
  ⇒ **改用精确删除那一行**：

  ```powershell
  # 加之前先记下行号
  Select-String -Path src/query/exec_scan.c -Pattern 'modalities/ft/ft_am.h'
  # 验证完之后，把这一行删掉（手工删，或用编辑器）
  ```

  ⚠️ **顺序要求**：B5.2 的反例验证必须排在 **W-C C-1 / C-5 之后**执行，
  否则「加一行再删一行」的操作会踩到它们的改动。

  ⚠️ **不做这一步就等于门禁没验过** —— 一个永远绿的检查等于没有检查。

- [ ] **B5.3 新建 `tools/measure_build.sh`**

  ```sh
  #!/bin/sh
  # measure_build.sh —— 按 profile 度量构建规模
  #
  # 【为什么要有这个】
  # "模态裁剪能省多少"是个需要数字回答的问题。
  # 没有数字 ⇒ 裁剪只是概念；有了数字 ⇒ 裁剪是工程事实。
  #
  # 用法：
  #   sh tools/measure_build.sh              # 量全部 9 个 profile
  #   sh tools/measure_build.sh vector ft    # 只量指定的
  #
  # 输出（每行一个 profile）：
  #   profile  源文件数  目标文件数  链接后符号数  构建秒数

  set -u
  ROOT=$(cd "$(dirname "$0")/.." && pwd) || exit 1
  cd "$ROOT" || exit 1

  CC=${CC:-gcc}
  # ⚠️ **（可执行性修正）Windows 上 PATH 里没有 `make`，只有 `mingw32-make`**
  # （MinGW-w64 的 make 二进制叫 `mingw32-make`；`make` 是 msys2 的那个）。
  # ⇒ 默认值改成 `mingw32-make`；Linux CI 上可以 `MAKE=make` 覆盖。
  MAKE=${MAKE:-mingw32-make}

  PROFILES=${*:-"full relational blob graph vector ft geo ts doc"}

  printf '%-12s %8s %8s %10s %8s\n' profile srcs objs symbols secs

  for m in $PROFILES; do
      # ⚠️ **（可执行性修正）产物目录必须与 Makefile 的规则对齐** ——
      # Makefile（B2.1）里是
      #   OBJ_BASE ?= $(OBJ_DIR)
      #   MM_OBJ_DIR := $(if $(filter full,$(MM)),$(OBJ_BASE),$(OBJ_BASE)_mm_$(MM))
      #   override OBJ_DIR := $(MM_OBJ_DIR)
      # ⇒ 传 `OBJ_BASE=build_measure` 时：
      #     MM=full   ⇒ 产物在 `build_measure`
      #     MM=vector ⇒ 产物在 `build_measure_mm_vector`
      # 所以目录名要**分 full / 非 full 两支**。
      case "$m" in
          full) objdir="build_measure" ;;
          *)    objdir="build_measure_mm_$m" ;;
      esac

      rm -rf "$objdir"

      start=$(date +%s)

      # 用 Makefile 的 MM 开关做一次真实构建（输出目录换掉）。
      # ⚠️ **必须传 `OBJ_BASE`，不能传 `OBJ_DIR`** —— `OBJ_DIR` 是**命令行变量**，
      # 在 GNU Make 里**命令行变量 > makefile 赋值**，`override` 也挡不住 ⇒
      # 传 `OBJ_DIR=xxx` 会把 `override OBJ_DIR := $(MM_OBJ_DIR)` 顶掉，
      # 真实目录变成 `xxx`（而不是 `xxx_mm_<m>`）⇒ 脚本按 `build_measure_mm_<m>`
      # 去找就**找不到 `.o`、`nm` 不到符号 ⇒ 度量列全是 0**。
      # `OBJ_BASE` 是本计划**新引入**的变量，它**不参与**那条覆盖链 ⇒ 由它决定最终目录。
      $MAKE MM="$m" OBJ_BASE=build_measure all >/dev/null 2>&1
      rc=$?
      if [ "$rc" -ne 0 ]; then
          printf '%-12s %8s\n' "$m" "BUILD-FAIL"
          continue
      fi

      secs=$(( $(date +%s) - start ))

      # ⚠️ **（可执行性修正）「源文件数」原来数的是**全仓**的 `.c`** ——
      # `find src -name '*.c' | wc -l` 对**每个 profile 都一样** ⇒ 这一列**没有区分度**，
      # 读者会误以为「裁剪没减少源文件」。
      # ⇒ 改成**该 profile 实际编译的**源文件数 = 产物目录里的 `.o` 个数
      #    （每个编译单元一个 `.o`，一一对应）。
      srcs=$(find "$objdir" -name '*.o' ! -name 'main.o' ! -path "$objdir/bench/*" 2>/dev/null | wc -l)
      objs=$(find "$objdir" -name '*.o' ! -name 'main.o' ! -path "$objdir/bench/*" 2>/dev/null | wc -l)

      # ⚠️ **（可执行性修正）`nm` 在 Windows 上只有 `nm.exe`（MinGW 自带），
      # Linux CI 上要换成 `nm -D`（动态符号表）或 `objdump -T`。
      # ⇒ 这里按平台自动选；两个都取不到就退化成 0（并在 stderr 上提示）。
      if [ -f "$objdir/db.exe" ]; then
          bin="$objdir/db.exe"
      elif [ -f "$objdir/db" ]; then
          bin="$objdir/db"
      else
          bin=""
      fi
      if [ -n "$bin" ]; then
          syms=$(nm "$bin" 2>/dev/null | grep -c ' [TtWw] ' || echo 0)
      else
          echo "warn: 找不到 $objdir 里的可执行文件（符号数记 0）" >&2
          syms=0
      fi

      printf '%-12s %8s %8s %10s %8s\n' "$m" "$srcs" "$objs" "$syms" "$secs"

      rm -rf "$objdir"
  done
  ```

  ⚠️ **（可执行性修正）原来的 `OBJ_DIR="$objdir" $MAKE MM="$m" all` 让度量**恒为 0** ——
  原因：GNU Make 的**命令行变量优先级最高**，`makefile` 里的
  `override OBJ_DIR := $(MM_OBJ_DIR)` 也**压不住**命令行给的 `OBJ_DIR`
  ⇒ 真实产物目录 = 脚本传的目录（而不是 `目录_mm_<m>`）
  ⇒ 脚本却按 `build_measure_mm_<m>` 去找 `.o` ⇒ `find` 找不到 ⇒ `.o` 数 = 0；
  `nm "$objdir/db.exe"` 也找不到文件 ⇒ 符号数 = 0。
  ⇒ **每一列都是 0，但脚本「成功」退出** —— 最坏的失败模式。

  ⇒ **改成传 `OBJ_BASE=build_measure`**（本计划新引入的变量，**不参与**那条覆盖链），
  由 Makefile 自己派生出 `build_measure`（full）/ `build_measure_mm_<m>`（其余）。

  ⚠️ **Windows 上跑这个脚本**：Git Bash 里 `make` 不在 PATH ⇒ 用

  ```powershell
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/measure_build.sh
  ```

  （脚本默认 `MAKE=mingw32-make`；Linux CI 上 `MAKE=make sh tools/measure_build.sh`。）

  ⚠️ **Linux CI 上的 `nm`**：GNU `nm` 默认只列**静态符号表**，
  动态符号表要用 `nm -D`（或 `objdump -T`）⇒ CI 里改成：

  ```sh
  MAKE=make NMCMD="nm -D" sh tools/measure_build.sh
  ```

- [ ] **B5.4 跑度量，写 `engine/db/docs/MM-PROFILES.md`**

  ```powershell
  cd C:\code\projects\engine\db
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/measure_build.sh
  ```

  预期：9 行输出。

  `engine/db/docs/MM-PROFILES.md`（下面用**四反引号**包住，因为模板里自己也有 ```bash 代码块）：

  ````markdown
  # MM 编译期模态裁剪 —— profile 说明与实测

  > 生成于 2026-11-18，数据由 `sh tools/measure_build.sh` 产出。
  > 真相源是 `tools/mm_profiles.mk`；本文件是它的人类可读副本。

  ## profile 一览

  | profile | 打开的模态 | 说明 |
  |---|---|---|
  | `full` | 全部 8 个 | 默认。`make` 等价于 `make MM=full` |
  | `relational` | 仅关系 | 底座型模态（关系）的单独形态 |
  | `blob` | 关系 + blob | 对象存储 |
  | `graph` | 关系 + graph | 图 |
  | `vector` | 关系 + vector | 向量 |
  | `ft` | 关系 + ft | 全文检索 |
  | `geo` | 关系 + geo | 地理空间 |
  | `ts` | 关系 + ts | 时序 |
  | `doc` | 关系 + doc | 文档 / JSON |

  > **每个 profile 都含关系模态** —— 它的实现横跨 L1–L3（catalog 直接持有
  > `Heap*`），关不掉。MM 裁的是「模态」，不是「层」。

  ## 实测数字与门禁值

  > **门禁 = 实测值 × 1.2**（设计 §4.4）。没有门禁值，CI 就只能「记录」不能「拦」。
  > CI 里比对：**实测 > 门禁值 ⇒ 红**。门禁值**只增不减**
  > （除非有人显式改基线文件并在 PR 里说明理由）。

  | profile | 编译源文件数 | 目标文件数 | 二进制符号数 | 代码行数 | 构建秒数 | 门禁值（每个指标 ×1.2） |
  |---|---|---|---|---|---|---|
  | `full` | （实测） | （实测） | （实测） | （实测） | （实测） | （实测 ×1.2） |
  | `relational` | | | | | | |
  | `blob` | | | | | | |
  | `graph` | | | | | | |
  | `vector` | | | | | | |
  | `ft` | | | | | | |
  | `geo` | | | | | | |
  | `ts` | | | | | | |
  | `doc` | | | | | | |

  ⚠️ **「代码行数」列要有区分度** —— 统计的是**该 profile 实际参与编译的源文件**的行数，
  不是 `src/` 全量（全量每个 profile 都一样，毫无意义）。

  ## 用法

  ```bash
  make MM=vector                  # 只编 vector 模态
  make MM=vector OBJ_BASE=out     # 换输出根目录（产物落在 out_mm_vector）
  make smoke                      # 构建 8 个 smoke
  ```
  ````

  ⚠️ **（可执行性修正）`OBJ_DIR=out` 改成了 `OBJ_BASE=out`** ——
  `OBJ_DIR` 是**命令行变量**，会**顶掉** makefile 里的
  `override OBJ_DIR := $(MM_OBJ_DIR)` ⇒ 所有 profile 会共用同一个 `out` 目录
  ⇒ 裁剪的 `.o` 与 full 的 `.o` 混在一处 ⇒ **跑的是旧二进制**
  （正是 Makefile 头部警告的那个坑）。要换目录就换 `OBJ_BASE`。

  ⚠️ **（可执行性修正）模板里原来有两个结构错误，已修**：
  ① 「门禁值」那一列被插到了表格**中间**，导致出现两行 `|---|---|` 分隔线
  （Markdown 表格只认第一条分隔线，后面那行会变成一行数据）；
  ② 模板里自带 ```bash 代码块，外层用三反引号包不住 ⇒ 围栏计数变奇数、
  后面整份文档的 Markdown 渲染全乱。**外层必须用四反引号**。

- [ ] **B5.5 提交**

  ```powershell
  cd C:\code\projects
  git add engine/db/tools/check_deps.sh engine/db/tools/measure_build.sh engine/db/docs/MM-PROFILES.md
  git commit -m "feat(kernel): 门禁加固（query 不许碰模态）+ 构建度量脚本与 profile 文档"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

### Task B6：收尾验收

**Files**
- 无代码变更。

- [ ] **B6.1 九个 profile 全部能编**

  ```powershell
  cd C:\code\projects\engine\db
  foreach ($m in 'full','relational','blob','graph','vector','ft','geo','ts','doc') {
      mingw32-make MM=$m clean
      mingw32-make MM=$m all 2>&1 | Select-String -Pattern 'error|warning' | Measure-Object | ForEach-Object { "$m : 问题数 $($_.Count)" }
  }
  ```

  预期：每个 profile 的问题数都是 `0`。

- [ ] **B6.2 主核测试仍然全绿（`MM=full`）**

  ```powershell
  mingw32-make clean && mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：95 通过 / 0 失败。

- [ ] **B6.3 依赖方向门禁绿**

  ```powershell
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  ```

  预期：`✓ 依赖方向正确`。

- [ ] **B6.4 8 个 smoke 全绿**

  ```powershell
  mingw32-make smoke
  foreach ($m in 'relational','blob','graph','vector','ft','geo','ts','doc') {
      & ".\build\smoke_$m.exe"
  }
  ```

  预期：8 个 `OK`。

- [ ] **B6.5 CI 两个 job 绿**

  推上去后看 GitHub Actions 的 `build-kernel-linux` 与 `build-kernel-windows`。

- [ ] **B6.7 评估 P3：seam 能不能做成可发布的头文件包**（D8 预备，**只评估、不实施**）

  ⚠️ **（复审修正）设计 §4.6 的 P3 阶段在三份计划里都没有落点** ——
  D8 判决是「方案 C 留预备，seam 固定为 §2.1 的那 13 个头 / 1,934 行」。

  ⇒ 本步**只做评估**，产出一段结论写进 `engine/db/docs/MM-PROFILES.md`：

  | 评估项 | 问题 | 结论 |
  |---|---|---|
  | seam 边界是否还干净 | 13 个头 / 1,934 行是否仍是模态可见的全部 | 是 / 否（列出新增的） |
  | 有没有模态头泄漏 | `grep -rn 'modalities/' src/common src/storage src/query src/spi` 是否零命中 | 是 / 否 |
  | 现在拆的代价 | 多进程需要跨进程协调、事务边界、错误码映射 | 估个工期 |
  | 什么时候值得拆 | 触发条件是什么（跨团队？独立发布？多语言运行时？） | 写成可判定的条件 |

  ⇒ **结论写「本轮不做」也要写**，并写清**下次评估的触发条件** ——
  「留预备」的意思是「有条件地不做」，不是「没人管」。

- [ ] **B6.6 父仓指针**

  ```powershell
  cd C:\code
  git add projects
  git commit -m "chore(submodule): 更新 projects 指针（主核 MM 裁剪与 CI 门禁）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

## 交付说明

1. **完成了什么** —— 主核进 CI；`MM=` 编译期裁剪（9 个 profile）；8 个 smoke 的链接期证明；门禁加固。
2. **变更范围** —— `engine/db/Makefile`、`engine/db/tools/`、`engine/db/test/smoke/`、`.github/workflows/ci.yml`。
3. **验证方式** —— B6.1~B6.5 的实测输出（不是"应该没问题"）。
4. **后续建议** —— W-C 的 C-1/C-2 完成后可把 `MM` 的裁剪点扩展到执行器；
   `MM-PROFILES.md` 的数字要在每次改过模态文件归属后重跑。

## Self-Review 记录

**Spec 覆盖**：§4.2 `MM` 编译期模态开关 → B1（`tools/mm_profiles.mk` 真相源）+
B2（Makefile 接入）+ B3（五处 `#if` 裁剪点）；§4.3 每模态 smoke → B4（8 个 smoke）；
§4.4 量化脚本 → B5.3（`measure_build.sh`）+ B5.4（`MM-PROFILES.md` +
`docs/arch/db/multimodal-build-baseline.md`）；§4.5 CI（P0 最高优先级）→ B0；
§4.6 阶段与顺序 → B0=P0、B1~B4=P1、B5~B6=P2；
§6.5 `MM` 开关与 smoke 覆盖 8 个模态 → B2/B3/B4 + B6.4；
§6.6 执行顺序 → 本计划表头已改为**引用设计 §9 的唯一权威顺序块**（不再各说一套）；
§7 风险 → B2.3 的 `CFLAGS` 延迟赋值 + `make -n` 验收（宏必须真的进命令行）、
B1c 的 `OBJ_BASE`/`override`（防命令行覆盖导致目录串台）、
B4.2 的 `SUBSTRATE_SRCS` 排除**全部**模态文件（否则链接期独立证明作废）、
B5.1b 的 S9② 门禁、B5.2 的反例验证、B6.7 的 P3 评估（**只评估、不实施**）。

**类型一致性**：`MM` / `OBJ_BASE` / `MM_OBJ_DIR` / `OBJ_DIR` 四个变量的职责在 B2.1 定义、
B2.3 / B3 / B4 使用，四处一致（`OBJ_BASE` 承接用户指定根目录，
`override OBJ_DIR` 落最终目录 —— 命令行覆盖不会让它串台）；
`MM_ALL` / `MM_CROPPABLE` / `MM_UP_<m>` / `MM_FILES_<m>` / `MM_DEP_<m>` / `MM_SET_<profile>`
六个变量名在 B1.1 定义、B2 的依赖闭包、B4 的 smoke 里写法一致；
`DB_HAS_<MOD>` 八个宏由 B2.1 生成、B3.3 在五处裁剪点使用、B4 用 `MM_FLAGS_ONLY_<m>` 单模态组合；
`CFLAGS_BASE` / `CFLAGS` / `MM_FLAGS` / `MM_FLAGS_ONLY_<m>` 四个 flag 变量的边界
在 B2.1 / B2.3 / B4.2 的注释里写明（`CFLAGS` 含全开组合、smoke 只用 `CFLAGS_BASE`，
否则 `-Werror` 下宏重定义直接编译失败）。名字无漂移。

**比例**：本计划 1652 行 / 设计文档 1192 行 ≈ 1.4:1。
其中约 30% 是两轮复审的「（复审修正）」批注 —— 它们是**要照着做的指令**，
不是背景叙述，删掉会导致执行者踩坑。
**复审来源**：本计划经过两轮独立 subagent 复审 ——
① 规格覆盖复审（对照设计 §1~§9 逐条查落点）与 ② 跨计划一致性复审
（31 个交叉点、契约冲突、文件归属冲突、顺序矛盾、数字口径）。
两轮的 blocking 级发现**已全部并入本计划正文**，每处均以
「⚠️ **（复审修正）**」标出，并写明**为什么错**与**改成什么** ——
这样执行者遇到分歧时能就地判断，而不是回头查聊天记录。
