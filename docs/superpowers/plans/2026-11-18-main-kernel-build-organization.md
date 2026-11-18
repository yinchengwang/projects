# 主核工程组织 —— CI 门禁 + `MM=` 编译期模态裁剪 + 每模态链接期证明（工作流 W-B）

> 依据：`docs/superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md` §4（设计 A）/ §6.5
> 落地日期：2026-11-18
> 执行顺序：**P0 先行**（其余全部依赖它）；**P1 必须在 W-C 的 C-1、C-2 之后**（它们与 P1 改同一批文件）。
> 互不冲突：W-A 动 `engine/src/` 与 `engine/include/`；本计划只动 `engine/db/` 与 `.github/`。

---

## Goal

给主核 `engine/db/` 补上三样它现在**没有**的东西：

1. **CI 门禁** —— 主核有 99 个测试文件（68 unit + 31 integration）、零 CI 覆盖。改动可能在本地绿、CI 红，也可能反过来。
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
| **8 个 profile** | `full`（全部）、`relational`、`blob`、`graph`、`vector`、`ft`、`geo`、`ts`、`doc` |
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

## Review Focus

| # | 关注点 | 判据 |
|---|---|---|
| R1 | 裁剪是不是真的裁了 | `make MM=vector` 产出的二进制里，`nm` 查不到 `db_ft_open` / `db_graph_open` / `db_ts_open` 等符号 |
| R2 | 关系模态有没有被误裁 | `make MM=vector` 的二进制里 `heap_am_build` / `catalog_find_table` 仍在 |
| R3 | 5 处裁剪点够不够 | 裁剪后**链接通过**（少一处 `#if` 就是链接错误，不会静默通过） |
| R4 | `MM=full` 与今天等价 | `make MM=full` 的 `build/db.exe` 与改动前功能一致；98 个测试全绿 |
| R5 | smoke 是不是"真证明" | smoke 的链接集合里**没有**别的模态的符号（`nm` 验证） |
| R6 | CI 是不是真的在跑主核 | CI 日志里能看到 `check_deps.sh` 与 `test/run.sh` 的输出 |
| R7 | 门禁有没有被绕过 | `check_deps.sh` 新增的规则在**故意违规**时必须红（要有反例验证） |

---

## 文件结构

### 新增

| 文件 | 作用 |
|---|---|
| `engine/db/tools/mm_profiles.mk` | 8 个 profile 的唯一真相源：模态清单、源文件归属、依赖、宏后缀 |
| `engine/db/tools/measure_build.sh` | 构建度量：按 profile 统计编译文件数 / 目标文件数 / 链接后符号数 / 耗时 |
| `engine/db/test/smoke/smoke_relational.c` | 关系模态 smoke |
| `engine/db/test/smoke/smoke_blob.c` | blob 模态 smoke |
| `engine/db/test/smoke/smoke_graph.c` | graph 模态 smoke |
| `engine/db/test/smoke/smoke_vector.c` | vector 模态 smoke |
| `engine/db/test/smoke/smoke_ft.c` | ft 模态 smoke |
| `engine/db/test/smoke/smoke_geo.c` | geo 模态 smoke |
| `engine/db/test/smoke/smoke_ts.c` | ts 模态 smoke |
| `engine/db/test/smoke/smoke_doc.c` | doc 模态 smoke |
| `engine/db/docs/MM-PROFILES.md` | 8 个 profile 的说明与实测数字 |

### 修改

| 文件 | 改什么 |
|---|---|
| `engine/db/Makefile` | 加 `MM` 开关、裁剪源文件、生成 `-DDB_HAS_*`、8 个 smoke 目标 |
| `engine/db/tools/check_deps.sh` | 加「query/ 不许 include 模态头」这条门禁 |
| `.github/workflows/ci.yml` | 加主核的 Linux / Windows 两个 job |

---

### Task B0：主核进 CI（先有红灯，才谈得上门禁）

**Files**
- 修改：`.github/workflows/ci.yml`

**Interfaces**
- 新增两个 CI job：`build-kernel-linux`、`build-kernel-windows`。
- 两者都跑：`make all` → `sh tools/check_deps.sh` → `sh test/run.sh`。

**为什么第一个做**：后面每个 Task 的验收都要靠 CI 抓"本地绿、CI 红"的情况。

- [ ] **B0.1 先确认 Linux 上主核能不能编（本地预演）**

  CI 的 Linux job 是第一次在 Linux 上编主核。**大概率会红** —— 那正是这个 job 的价值。

  能本地预演的办法（如果有 WSL / Docker）：

  ```bash
  docker run --rm -v "$(pwd)":/k -w /k/engine/db gcc:13 sh -c "make all && sh tools/check_deps.sh && sh test/run.sh"
  ```

  没有 Docker/WSL 就**直接推上去让 CI 跑**（B0.4），红了就按 B0.5 修。

- [ ] **B0.2 在 `.github/workflows/ci.yml` 里加 Linux job**

  ```yaml
  # ========================================================================
  # 主核（engine/db/）：自带 Makefile，不在 CMake 图内
  #
  # 主核有 98 个测试文件（66 unit + 32 integration），此前**零 CI 覆盖** ——
  # 改动可能在本地绿、在别的平台红。这里补上。
  # ========================================================================
  build-kernel-linux:
    name: 主核构建（Linux）
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: 编译主核
        working-directory: engine/db
        run: make all
      - name: 依赖方向门禁
        working-directory: engine/db
        run: sh tools/check_deps.sh
      - name: 运行主核测试
        working-directory: engine/db
        run: sh test/run.sh
  ```

- [ ] **B0.3 加 Windows job**

  ⚠️ **不要另起炉灶**：现有 `build-engineering-windows` 已经用了
  `msys2/setup-msys2@v2` + `MINGW64`。**照抄那个 job 的 steps 头**，
  只把 `working-directory` 与 `run` 换掉。

  ```yaml
  build-kernel-windows:
    name: 主核构建（Windows）
    runs-on: windows-latest
    defaults:
      run:
        shell: msys2 {0}
    steps:
      - uses: actions/checkout@v4
      - uses: msys2/setup-msys2@v2
        with:
          msystem: MINGW64
          update: true
          install: >-
            git
            mingw-w64-x86_64-gcc
            make
      - name: 编译主核
        working-directory: engine/db
        run: mingw32-make all
      - name: 依赖方向门禁
        working-directory: engine/db
        run: sh tools/check_deps.sh
      - name: 运行主核测试
        working-directory: engine/db
        run: sh test/run.sh
  ```

- [ ] **B0.4 推上去看 CI**

  ```powershell
  cd C:\code\projects
  git add .github/workflows/ci.yml
  git commit -m "ci: 主核 engine/db 进 CI（Linux + Windows 两个 job）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

- [ ] **B0.5 CI 红了怎么办（Linux 平台差异清单）**

  按这个顺序查，**不要瞎改**：

  | 症状 | 原因 | 处置 |
  |---|---|---|
  | `implicit declaration of function 'GetCurrentThreadId'` | Windows 专有 API 少了 `#ifdef _WIN32` 保护 | 补 `#ifdef _WIN32` / `#else` 分支 |
  | `clock_gettime` 链接失败 | Linux 需要 `-lrt`（老 glibc） | `LDLIBS` 加 `-lrt` |
  | `mkdir: cannot create directory` | 用了 Windows 的 `mkdir` 语义 | 改 `mkdir -p` |
  | 路径分隔符 `\` | 代码里手写了反斜杠 | 改成 `/`（Windows 的 C 运行时也吃 `/`） |
  | 测试跑起来时间不对 | `clock()` 在 Linux 上分辨率不同 | 放宽测试里的时间比值断言（并在注释里写清原因） |

  ⚠️ **改主核代码只为兼容 Linux 时，要单独一个提交**，commit message 里写明
  「fix(kernel): 修 Linux 平台差异」。**不要**把这个修复混进后面的裁剪任务里。

- [ ] **B0.6 CI 绿了再提交 B0.5 的修复（若有）**

  ```powershell
  git add engine/db/
  git commit -m "fix(kernel): 修 Linux 平台差异（CI 首次跑主核暴露）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

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
  #   3. docs/MM-PROFILES.md —— 人工可读的说明
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
  # ---------------------------------------------------------------------
  # ⚠️ src/storage/gstore*.c **不属于 graph**：
  #    gstore 是"通用磁盘存储"，graph 与 vector **共用**
  #    （vector_am.c:125 调 gstore_cursor_init(vstore_meta(...))）。
  #    所以它是底座，任何 profile 都在。

  MM_FILES_blob   := $(wildcard src/modalities/blob/*.c) src/db_blob.c
  MM_FILES_graph  := $(wildcard src/modalities/graph/*.c) src/db_graph.c src/db_graph_walk.c
  MM_FILES_vector := $(wildcard src/modalities/vector/*.c) src/db_vec.c
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
  MM_DEP_doc    :=

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

  ⚠️ `MM_FILES_vector` 应该有 10 项（9 个 `modalities/vector/*.c` + `db_vec.c`）。
  对不上就说明路径写错了。

- [ ] **B1.3 删掉临时目标，提交**

  ```powershell
  cd C:\code\projects
  git add engine/db/tools/mm_profiles.mk
  git commit -m "feat(kernel): 新增 MM profile 真相源（8 个 profile 的模态边界）"
  git push origin main 2>&1 | Out-String | Write-Output
  ```

---

### Task B2：Makefile 接入 `MM`（裁剪源文件 + 生成宏）

**Files**
- 修改：`engine/db/Makefile`

**Interfaces**
- 新增命令行变量：`MM`（默认 `full`）、`MM_OBJ_DIR`（派生于 `OBJ_DIR`）。
- 新增宏：`DB_HAS_RELATIONAL` / `DB_HAS_BLOB` / `DB_HAS_GRAPH` / `DB_HAS_VECTOR` /
  `DB_HAS_FT` / `DB_HAS_GEO` / `DB_HAS_TS` / `DB_HAS_DOC`（恒定定义，值为 0 或 1）。

- [ ] **B2.1 在 `Makefile` 的 `OBJ_DIR ?= build`（L62）之后插入 MM 块**

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

  MM_OBJ_DIR := $(if $(filter full,$(MM)),$(OBJ_DIR),$(OBJ_DIR)_mm_$(MM))

  # ---- profile 校验 ----
  ifeq ($(filter $(MM),$(MM_PROFILES)),)
    $(error 未知的 MM profile '$(MM)'。可选：$(MM_PROFILES))
  endif

  # ---- 依赖闭包（展开到不动点；三轮足够 —— 模态只有 8 个）----
  MM_CLOSED := $(MM_SET_$(MM))
  MM_CLOSED := $(MM_CLOSED) $(foreach m,$(MM_SET_$(MM)),$(MM_DEP_$(m)))
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
  OBJ_DIR := $(MM_OBJ_DIR)
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

- [ ] **B2.3 把宏加进 CFLAGS**

  L57：

  ```make
  CFLAGS  := $(CSTD) $(WARN) $(OPT) $(HARDEN) $(INC) $(SAN) $(MM_FLAGS)
  ```

- [ ] **B2.4 验证：默认构建不变**

  ```powershell
  cd C:\code\projects\engine\db
  mingw32-make clean
  mingw32-make all
  & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
  ```

  预期：编译零警告；99 通过 / 0 失败。

  ⚠️ **此时还没有 `#if` 裁剪点**，所以 `make MM=vector` 会在链接阶段报
  "undefined reference to db_ft_open" —— **这是预期的**，B3 修。

- [ ] **B2.5 验证：裁剪确实生效（只看源文件层面）**

  ```powershell
  mingw32-make MM=vector -n 2>&1 | Select-String 'ft_am|db_ft' | Measure-Object
  ```

  预期：`0`（`make -n` 打印的命令里**没有**任何 ft 模态的编译行）。

  再看反向：

  ```powershell
  mingw32-make MM=vector -n 2>&1 | Select-String 'vector_am' | Measure-Object
  ```

  预期：`> 0`。

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

- [ ] **B3.1b `src/db_internal.h`：模态函数的**声明**也要守卫**

  ⚠️ **只守 include 和字段不够** —— `db_internal.h` 里还有一堆**模态专属的函数声明**：
  `db_vec_register` / `db_vec_open` / `db_vec_close`、
  `db_ft_replay_add` / `db_ft_replay_free` / `db_ft_recovery_hook` /
  `db_ft_register` / `db_ft_open` / `db_ft_close`、
  `db_ts_register` / `db_ts_open` / `db_ts_close`、
  `db_geo_flatten` / `db_geo_tab_free` / `db_geo_register` / `db_geo_open` / `db_geo_close`、
  `db_graph_register`，
  以及 geo 专属的类型 `GeoPtEntry` / `GeoTab`。

  这些声明**不守卫不会编译失败**（声明一个不存在的函数是合法的），
  但会留下"裁剪后的二进制里还有 ft 的声明"这种**误导性残留**，
  而且 `db_ft_recovery_hook` 的**注册点**在 `db.c` 里（`recovery_set_hook(db_ft_recovery_hook, inst)`），
  那处不守卫就会**链接失败**。

  处理：

  ```c
  #if DB_HAS_VECTOR
  int db_vec_register(DbInstance *inst);
  int db_vec_open(DbInstance *inst);
  void db_vec_close(DbInstance *inst);
  #endif

  #if DB_HAS_FT
  int db_ft_replay_add(FtReplaySet *set, u32 doc_id);
  void db_ft_replay_free(FtReplaySet *set);
  int db_ft_recovery_hook(void *ctx, u8 type, u32 xid, u32 page_id, u32 lsn,
                          const u8 *payload, u16 payload_len, RecHookPhase phase);
  int db_ft_register(DbInstance *inst);
  int db_ft_open(DbInstance *inst);
  void db_ft_close(DbInstance *inst);
  #endif

  #if DB_HAS_TS
  int db_ts_register(DbInstance *inst);
  int db_ts_open(DbInstance *inst);
  void db_ts_close(DbInstance *inst);
  #endif

  #if DB_HAS_GEO
  typedef struct { float x; float y; bool dead; } GeoPtEntry;
  typedef struct { GeoPtEntry *p; u32 cap; u32 n; u32 live; u32 dead_n; } GeoTab;
  int  db_geo_flatten(DbInstance *inst, GeoTab *tab);
  void db_geo_tab_free(GeoTab *tab);
  int  db_geo_register(DbInstance *inst);
  int  db_geo_open(DbInstance *inst);
  void db_geo_close(DbInstance *inst);
  #endif

  #if DB_HAS_GRAPH
  int db_graph_register(DbInstance *inst);
  #endif
  ```

  ⚠️ **`FtReplaySet` 类型本身**也要守卫（它只被 ft 用）：

  ```c
  #if DB_HAS_FT
  typedef struct {
      u32 *ids;
      u32  n;
      u32  cap;
  } FtReplaySet;
  #endif
  ```

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
      /* geo 模态的实例字段（原样搬过来） */
      ...
  #endif

  #if DB_HAS_TS
      /* ts 模态的实例字段（原样搬过来） */
      ...
  #endif

      /* ... 其余字段不变 ... */
  } DbInstance;
  ```

  ⚠️ **geo 与 ts 的字段块要从原文件里原样搬**（本次读取被截断，没拿到 geo/ts 字段的确切名字）。
  搬的时候**一个字都不要改**，只加 `#if` / `#endif`。

- [ ] **B3.3 `src/db.c`：open 路径守卫**

  实测的 8 个触点，逐个包：

  | 行 | 原来 | 处理 |
  |---|---|---|
  | L~487 | `recovery_set_hook(db_ft_recovery_hook, inst);` | `#if DB_HAS_FT` … `#endif`（**不守卫会链接失败**：`db_ft_recovery_hook` 已被守卫掉） |
  | L~489 | `recovery_set_hook(NULL, NULL);` | **不守卫** —— 注销空钩子无害 |
  | L506 | `rc = db_ft_open(inst);` | `#if DB_HAS_FT` … `#endif` |
  | L531 | `rc = blob_store_create(pg, db_wal_hook, inst, &inst->blobs);` | `#if DB_HAS_BLOB` … `#endif` |
  | L562 | `rc = spi_registry_add(inst->reg, "blob_frames", &inst->blob_am_frames);` | `#if DB_HAS_BLOB`（与 L531 同一块内） |
  | L564 | `rc = spi_registry_add(inst->reg, "blob_objects", &inst->blob_am_objects);` | 同上 |
  | L596 | `rc = db_graph_register(inst);` | `#if DB_HAS_GRAPH` … `#endif` |
  | L622 | `int vrc = db_vec_open(inst);` | `#if DB_HAS_VECTOR` … `#endif` |
  | L641 | `rc = db_ft_register(inst);` | `#if DB_HAS_FT` … `#endif` |
  | L663 | `int grc = db_geo_open(inst);` | `#if DB_HAS_GEO` … `#endif` |
  | L687 | `int trc = db_ts_open(inst);` | `#if DB_HAS_TS` … `#endif` |

  ⚠️ **被 `#if` 掉的分支里如果有变量声明，被跳过后就"未使用" ⇒ `-Werror` 报错。**
  处理办法二选一：
  - 把该分支里的局部变量**一起**放进 `#if` 块（推荐）；
  - 或者在 `#else` 分支里补 `int vrc = 0;` 之类（不推荐，容易漏）。

  **逐个分支检查**：编译一遍，编译器会精确指出哪个变量没用了。

- [ ] **B3.4 `src/db.c`：close 路径守卫**

  L200–L205：

  ```c
      db_ft_close(inst);
      db_geo_close(inst);
      db_ts_close(inst);
      graph_destroy(inst->graph);
      blob_store_destroy(inst->blobs);
  ```

  改成：

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
  #if DB_HAS_BLOB
      blob_store_destroy(inst->blobs);
  #endif
  ```

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

  预期：99 通过 / 0 失败。

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
  MM_FLAGS_ONLY_$(1) := $(foreach m,$(MM_ALL),-DDB_HAS_$(MM_UP_$(m))=$(if $(filter $(m),$(1)),1,0))

  # 底座对象（不含 main.o / bench / 任何模态对象）
  SUBSTRATE_SRCS := $(filter-out $(MM_CROP_SRCS) $(SRC_DIR)/main.c,$(SRCS_ALL))
  SUBSTRATE_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SUBSTRATE_SRCS))

  # 模态 $(1) 的对象
  MOD_OBJS_$(1) := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(MM_FILES_$(1)))
  ```

  再加 per-modality 规则：

  ```make
  define MM_SMOKE_RULE
  # 只开模态 $(1) 的根
  $(OBJ_DIR)/smoke_$(1)/db.o: $(SRC_DIR)/db.c
  	@mkdir -p $$(@D)
  	$(CC) $(CFLAGS) $(MM_FLAGS_ONLY_$(1)) -c $$< -o $$@

  $(OBJ_DIR)/smoke_$(1)/db_instance.o: $(SRC_DIR)/db_instance.c
  	@mkdir -p $$(@D)
  	$(CC) $(CFLAGS) $(MM_FLAGS_ONLY_$(1)) -c $$< -o $$@

  # smoke 的 main
  $(OBJ_DIR)/smoke_$(1).o: test/smoke/smoke_$(1).c
  	@mkdir -p $$(@D)
  	$(CC) $(CFLAGS) -c $$< -o $$@

  # 链接
  $(OBJ_DIR)/smoke_$(1).exe: $(OBJ_DIR)/smoke_$(1).o $(OBJ_DIR)/smoke_$(1)/db.o \
                             $(OBJ_DIR)/smoke_$(1)/db_instance.o \
                             $(SUBSTRATE_OBJS) $(MOD_OBJS_$(1))
  	$(CC) $(CFLAGS) -o $$@ $$^ $(LDLIBS)
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
      DB *db = db_open("test/tmp/smoke_relational.db");
      if (!db) {
          fprintf(stderr, "smoke_relational: db_open 失败\n");
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

  ⚠️ **`db_open` 的路径参数**：从 `include/db.h` 确认签名（是路径还是目录）。
  临时库放在 `test/tmp/`（`.gitignore` 已覆盖）。

  其余 7 个照这个形状写，每个换一个模态：

  | smoke | 调什么 | 备注 |
  |---|---|---|
  | `smoke_blob.c` | `db_blob_exists(db)` | 签名从 `include/db.h` 确认 |
  | `smoke_graph.c` | `db_graph_stat(db, ...)` | 同上 |
  | `smoke_vector.c` | `db_vec_exists(db)` | 同上 |
  | `smoke_ft.c` | `db_ft_exists(db)` | 同上 |
  | `smoke_geo.c` | `db_geo_exists(db)` | 同上 |
  | `smoke_ts.c` | `db_ts_exists(db)` | 同上 |
  | `smoke_doc.c` | `json_parse` + `jsonb_encode`/`jsonb_decode` 往返 | 文档模态**还没有公开 API**（W-C 才建 `db_json_*`），这里直接调内部 API 做一次往返 |

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

  ⚠️ **`forbid` 的实现是按前缀匹配的**（`#include "[^"]*${bad}`），
  而 `bad` 传的是 `heap`（不带斜杠）⇒ 它能匹配 `#include "storage/heap.h"`。
  ✅ 正好符合需要。

  ⚠️ 但 `ft_` / `ts_` / `geo` 这几个词要注意误伤：
  - `ts_` 会匹配 `#include "query/..."` 吗？不会（`query` 里没有 `ts_`）。
  - `ft_` 会匹配 `#include "storage/fast.h"` 吗？不存在这种文件。
  - `json` 会匹配 `#include "storage/json.h"` 吗？不存在。
  - 跑一遍就知道 —— **B5.3 有反例验证**。

- [ ] **B5.2 反例验证（门禁必须会红）**

  临时在 `src/query/exec_scan.c` 顶部加一行：

  ```c
  #include "modalities/ft/ft_am.h"
  ```

  ```powershell
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
  ```

  预期：输出 `✗ src/query 不允许依赖 ft_` 并**退出码非 0**。

  然后**撤销这行**（`git checkout engine/db/src/query/exec_scan.c`）。

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
  MAKE=${MAKE:-make}

  PROFILES=${*:-"full relational blob graph vector ft geo ts doc"}

  printf '%-12s %8s %8s %10s %8s\n' profile srcs objs symbols secs

  for m in $PROFILES; do
      objdir="build_measure_$m"
      rm -rf "$objdir"

      start=$(date +%s)

      # 用 Makefile 的 MM 开关做一次真实构建（输出目录换掉）
      OBJ_DIR="$objdir" $MAKE MM="$m" all >/dev/null 2>&1
      rc=$?
      if [ "$rc" -ne 0 ]; then
          printf '%-12s %8s\n' "$m" "BUILD-FAIL"
          continue
      fi

      secs=$(( $(date +%s) - start ))

      srcs=$(find src -name '*.c' | wc -l)
      objs=$(find "$objdir" -name '*.o' | wc -l)
      syms=$(nm "$objdir/db.exe" 2>/dev/null | grep -c ' [TtWw] ' || echo 0)

      printf '%-12s %8s %8s %10s %8s\n' "$m" "$srcs" "$objs" "$syms" "$secs"

      rm -rf "$objdir"
  done
  ```

  ⚠️ `OBJ_DIR="$objdir" $MAKE MM="$m" all` 里的 `OBJ_DIR` 覆盖 —— 本计划的
  `MM_OBJ_DIR` 派生规则是 `$(OBJ_DIR)_mm_$(MM)`，所以覆盖 `OBJ_DIR` 会得到
  `build_measure_vector_mm_vector`。**没关系**，脚本里 `rm -rf` 的目录名要跟着改：

  ```sh
      objdir="build_measure_$m"
      ...
      OBJ_DIR="$objdir" $MAKE MM="$m" all >/dev/null 2>&1
      # Makefile 里的实际目录是 $(OBJ_DIR)_mm_$(MM)（full 除外）
      case "$m" in
          full) realdir="$objdir" ;;
          *)    realdir="${objdir}_mm_$m" ;;
      esac
  ```

  然后 `objs=$(find "$realdir" -name '*.o' | wc -l)`、
  `syms=$(nm "$realdir/db.exe" ...)`。

- [ ] **B5.4 跑度量，写 `docs/MM-PROFILES.md`**

  ```powershell
  cd C:\code\projects\engine\db
  & "C:\Program Files\Git\usr\bin\sh.exe" tools/measure_build.sh
  ```

  预期：9 行输出。

  `engine/db/docs/MM-PROFILES.md`：

  ```markdown
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
  > `Heap*`），关不掉。MM 裁的是"模态"，不是"层"。

  ## 实测数字

  | profile | 源文件数 | 目标文件数 | 二进制符号数 | 构建秒数 |
  |---|---|---|---|---|
  | full | | | | |
  | relational | | | | |
  | ... | | | | |

  ## 用法

  ```bash
  make MM=vector          # 只编 vector 模态
  make MM=vector OBJ_DIR=out   # 换输出目录
  make smoke              # 构建 8 个 smoke
  ```
  ```

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

  预期：99 通过 / 0 失败。

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
