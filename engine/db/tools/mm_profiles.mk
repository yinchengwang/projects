# mm_profiles.mk —— MM 编译期裁剪的唯一真相源
#
# 【这个文件为什么是"唯一真相源"】
# 「一个模态的代码边界在哪」这件事必须**只在一个地方**写。
# 它同时被三处消费：
#   1. Makefile        —— 据此裁剪源文件、生成 -DDB_HAS_* 宏
#   2. tools/check_deps.sh —— 据此校验模态之间不许 include（B1.4 起）
#   3. engine/db/docs/MM-PROFILES.md —— 人工可读的说明（B1.4 起）
# 三处各写一份 = 迟早不一致 = 裁剪出来的二进制缺东西，链接才报错。
#
# 【为什么不用 $(shell) 推导】
# Windows 下 make 的 $(shell ...) 走 cmd.exe（见 Makefile L4-20 的注释），
# tr / sed 之类不可用 ⇒ 大写转换一律手写。
#
# 【路径口径】
# 全部相对 engine/db/（make 的工作目录）。

# ---------------------------------------------------------------------
# 模态清单
# ---------------------------------------------------------------------
# 全部 8 个模态。
MM_ALL := relational blob graph vector ft geo ts doc

# 可裁剪的模态（7 个）。
# 关系模态不在此列 —— 它是**底座型模态**，恒常驻（见下）。
MM_CROPPABLE := blob graph vector ft geo ts doc

# 模态 → -DDB_HAS_* 宏的后缀（大写，手写；不能用 $(shell tr)）。
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
# 关系模态是底座型模态：实现横跨 L1–L3。
#   堆实现      src/storage/heap*.c
#   记录编解码   src/storage/record.c、value.c
#   catalog     src/catalog/catalog.c —— 表项里**直接持有 Heap***
#   执行器适配   src/modalities/relational/heap_am*.c、btree_am.c
# catalog.h:8 与 vacuum.h:6 都持有 Heap* ⇒ 关掉关系模态 = catalog 编译不过。
# 结论：**MM 裁的是"模态"，不是"层"**。每个 profile 都含关系模态。

# ---------------------------------------------------------------------
# 每个模态占用的源文件（相对 engine/db/）
# ---------------------------------------------------------------------
# ⚠️ 这些路径是当前实际布局；W-C 的目录搬家尚未执行时不要提前改名。
#
# doc 目前只有底层 JSON/JSONB 实现，尚未接 db_doc.c / SQL 入口；
# 仍然把它作为可裁剪 profile 纳入真相源，避免未来接入时再改 profile 机制。
MM_FILES_blob   := $(wildcard src/modalities/blob/*.c) src/db_blob.c src/repl_blob.c
MM_FILES_graph  := $(wildcard src/modalities/graph/*.c) src/db_graph.c src/db_graph_walk.c src/repl_graph.c
MM_FILES_vector := $(wildcard src/modalities/vector/*.c) src/db_vec.c src/repl_vec.c
MM_FILES_ft     := $(wildcard src/modalities/ft/*.c) src/db_ft.c src/db_ft_txn.c src/repl_ft.c
MM_FILES_geo    := $(wildcard src/modalities/geo/*.c) src/db_geo.c src/db_geo_query.c src/repl_geo.c
MM_FILES_ts     := $(wildcard src/modalities/ts/*.c) src/db_ts.c src/repl_ts.c
MM_FILES_doc    := $(wildcard src/modalities/doc/*.c)
MM_FILES_relational := src/storage/heap.c src/storage/heap_compact.c src/storage/heap_scan.c \
                       src/storage/record.c src/storage/value.c \
                       $(wildcard src/catalog/*.c) $(wildcard src/index/*.c) \
                       $(wildcard src/modalities/relational/heap_am*.c) src/modalities/relational/btree_am.c

# ---------------------------------------------------------------------
# 模态依赖（闭包用）
# ---------------------------------------------------------------------
# 当前没有可变模态之间的依赖：
#   doc 依赖的是关系模态（恒常驻，不进闭包计算）；
#   ft 的倒排索引不依赖 blob（段内容在磁盘页里，不在对象存储里）。
# 机制先立起来 —— 将来有人写 MM_DEP_vector := blob，缺依赖会被立刻抓出来。
MM_DEP_relational :=
MM_DEP_blob       :=
MM_DEP_graph      :=
MM_DEP_vector     :=
MM_DEP_ft         :=
MM_DEP_geo        :=
MM_DEP_ts         :=
MM_DEP_doc        := relational

# ---------------------------------------------------------------------
# 8 个 profile
# ---------------------------------------------------------------------
# 每个 profile 显式列出打开的模态集合（可变模态部分）。
# full = 全部 7 个可变模态；relational = 一个都不开。
MM_SET_full       := $(MM_CROPPABLE)
MM_SET_relational :=
MM_SET_blob       := blob
MM_SET_graph      := graph
MM_SET_vector     := vector
MM_SET_ft         := ft
MM_SET_geo        := geo
MM_SET_ts         := ts
MM_SET_doc        := doc

# 全部 profile 名（给错误提示用）。
MM_PROFILES := full relational blob graph vector ft geo ts doc
