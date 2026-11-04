# 编译配置
# 关键：-fno-strict-aliasing 别省，页缓冲里大量类型双关，开优化后容易出诡异的错

# 【SHELL 必须给绝对路径 —— 这不是讲究，是修一个会静默吞掉构建的坑】
#
# Windows 下 make 的 $(shell ...) 与 recipe 默认都走 cmd.exe，
# 而 cmd.exe 里**单引号不是引用符**：'*.c' 会被原样传下去，
# 于是 $(shell find src -name '*.c') 落到 **Windows 自带的 find.exe**
# （文本搜索工具，不是 findutils）⇒ 报 "paths must precede expression"，
# SRCS 变成空 ⇒ **make all 什么都不编**，而且**不报错**。
#
# 后果非常阴险：源码改了、编译"成功"了，跑的却是**上一次的对象文件** ——
# 测出来的结论全是假的（v1.5 步骤 1 踩到过：改了 gorilla.c，make 没重编，
# 测试跑的是旧二进制，报出来的失败信息自相矛盾）。
#
# 修法：SHELL 给 Git 自带的 sh 的**绝对路径**（make 只认绝对路径），
# 这样 $(shell ...) 与 recipe 都用 sh ⇒ find 是 findutils 的 find。
#
# 为什么要 sh 而不是 cmd.exe：Makefile 里大量用 find / rm / mkdir -p，
# 这些在 cmd.exe 下要么没有、要么语法不同。
ifeq ($(OS),Windows_NT)
  ifeq ($(wildcard C:/Program Files/Git/usr/bin/sh.exe),)
    # ⚠️ 不要用 `2>NUL`：那是 cmd.exe 的语法，在 Git Bash 下会**在仓库目录里
    # 创建一个名为 NUL 的空文件**（每次构建都会长出来，污染 git status）。
    # `2>/dev/null` 两边都对（cmd.exe 也认 /dev/null，只是当成一个普通文件名）。
    GIT_SH := $(shell where sh 2>/dev/null | head -1)
  else
    GIT_SH := C:/Program Files/Git/usr/bin/sh.exe
  endif
  SHELL := $(GIT_SH)
else
  SHELL := sh
endif

# make 内建的 CC=cc 会盖住 ?=，所以按 origin 判断
ifeq ($(origin CC),default)
CC      := gcc
endif

CSTD    := -std=c11 -D_POSIX_C_SOURCE=200809L
WARN    := -Wall -Wextra -Werror
OPT     ?= -O2 -g
HARDEN  := -fno-strict-aliasing -fno-omit-frame-pointer
INC     := -Iinclude -Isrc
SAN     ?=

# 线程库（v0.10 起有并发测试）
#   MinGW-w64 的 winpthreads 已经在默认库里，不需要显式链接
#   Linux 上 pthread 是单独的库（glibc 2.34 之后合进了 libc，
#   但显式写上对所有版本都安全）
ifeq ($(OS),Windows_NT)
LDLIBS  :=
else
LDLIBS  := -lpthread
endif

CFLAGS  := $(CSTD) $(WARN) $(OPT) $(HARDEN) $(INC) $(SAN)

SRC_DIR := src
# OBJ_DIR 可用命令行覆盖（make OBJ_DIR=build_xxx）——
# 多个构建同时跑时各写各的目录，免得 gcc 写坏同一个 .o。
OBJ_DIR ?= build
# 【为什么用 wildcard 而不是 $(shell find ...)】
#
# Windows 上 make 的 $(shell ...) 走 cmd.exe：单引号不是引用符（'*.c' 被原样传下去），
# 而 cmd.exe 里 find 命中的是 **Windows 自带的 find.exe**（文本搜索工具），
# 它把 "paths must precede expression" 报出来 ⇒ SRCS 变成空
# ⇒ **make all 什么都不编，而且不报错**（源码改了，跑的还是旧对象文件）。
#
# $(wildcard ...) 是 make 的**内建函数**，不经过 shell ⇒ 没有引号问题、没有 find 版本问题。
# 【坑 M6-20 · Windows 上 make 的 $(shell find) 会静默返回空 ⇒ 什么都不编】
#
# 症状：改了源码，`mingw32-make all` 报 "BUILD OK"，
#       但 `build/*.o` 的时间戳**没变** ⇒ 跑的是旧二进制。
#       测试结论因此全是假的（"改了还是红"、"改了还是绿"这种自相矛盾的现象）。
# 原因：make 的 $(shell ...) 在 Windows 上走 cmd.exe，
#       cmd.exe 里单引号不是引用符 ⇒ find 落到 Windows 自带的 find.exe
#       （文本搜索工具，不是 findutils）⇒ SRCS 为空 ⇒ 目标为空 ⇒ 什么都不编。
# 修法：改用 make 内建的 $(wildcard ...)（不经过 shell，没有引号问题）。
# 验证：改一个源文件 → 对象文件时间戳必须跟着变（否则就是又踩了这个坑）。
SRCS    := $(wildcard $(SRC_DIR)/*/*.c $(SRC_DIR)/*/*/*.c $(SRC_DIR)/*.c)
OBJS    := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)

# Windows 下 gcc -o db 实际产出 db.exe，显式区分省得找不到文件
ifeq ($(OS),Windows_NT)
TARGET  := db.exe
else
TARGET  := db
endif

# 基准与参照工具（不进主二进制，也不参与 make check）
#   vec_bench    向量模态的验收基准
#   graph_bench  图模态的性能基准（v0.11 就有）
#   hnswlib_ref  用 hnswlib（业界事实标准）算同一个数据集做对照
#
# 【每个基准是独立的可执行文件】
#
# 它们各自带 main —— 链到一起会 multiple definition of main。
# （第一版用 wildcard 收 bench/*.c 一起链，直接撞了这个错；
#  幸好 build.sh 跑了一次才发现。）
CXX        ?= g++
BENCH_DIR  := bench

ifeq ($(OS),Windows_NT)
VEC_BENCH_EXE   := $(OBJ_DIR)/vec_bench.exe
GRAPH_BENCH_EXE := $(OBJ_DIR)/graph_bench.exe
REF_EXE         := $(OBJ_DIR)/hnswlib_ref.exe
EXEC_BENCH_EXE  := $(OBJ_DIR)/exec_bench.exe
LATCH_BENCH_EXE := $(OBJ_DIR)/latch_bench.exe
LATCH_BENCH_BIGLOCK_EXE := $(OBJ_DIR)/latch_bench_biglock.exe
FT_BENCH_EXE    := $(OBJ_DIR)/ft_bench.exe
GEO_BENCH_EXE   := $(OBJ_DIR)/geo_bench.exe
TS_BENCH_EXE    := $(OBJ_DIR)/ts_bench.exe
else
VEC_BENCH_EXE   := $(OBJ_DIR)/vec_bench
GRAPH_BENCH_EXE := $(OBJ_DIR)/graph_bench
REF_EXE         := $(OBJ_DIR)/hnswlib_ref
EXEC_BENCH_EXE  := $(OBJ_DIR)/exec_bench
LATCH_BENCH_EXE := $(OBJ_DIR)/latch_bench
LATCH_BENCH_BIGLOCK_EXE := $(OBJ_DIR)/latch_bench_biglock
FT_BENCH_EXE    := $(OBJ_DIR)/ft_bench
GEO_BENCH_EXE   := $(OBJ_DIR)/geo_bench
TS_BENCH_EXE    := $(OBJ_DIR)/ts_bench
endif

# ==================== A3：latch 吞吐基准（细粒度 vs 大锁基线） ====================
#
# 两个二进制，**同一份源码**，区别只有一个编译期宏：
#
#   latch_bench          当前源码（细粒度 latch）
#   latch_bench_biglock  同一份源码 + -DDB_BIGLOCK_BASELINE（把退役的大锁放回去）
#
# 【为什么大锁基线要重编一遍整个库】
# 那个开关是**编译期**的（见 src/db_stmt.c 里的 #ifdef DB_BIGLOCK_BASELINE）——
# 用运行期开关的话，两种模式会跑在同一个进程里：页缓存、缓冲池、WAL 状态
# 全部互相污染，比值就不干净了。两次独立进程、两次独立库文件，才是对照。
#
# 所以大锁版本有自己的一套目标文件（build_biglock/），不与主构建抢 .o。
#
# 【为什么基线是"整个语句持锁"】
# 因为那**就是**步骤 4d 退役之前的写法（锁的范围覆盖 parse / 开事务 /
# 执行 / 提交含 fsync）。少放一点就不是基线了。
BIGLOCK_DIR   ?= build_biglock
BIGLOCK_FLAGS := -DDB_BIGLOCK_BASELINE=1
BIGLOCK_OBJS  := $(patsubst $(SRC_DIR)/%.c,$(BIGLOCK_DIR)/%.o,$(SRCS))
BIGLOCK_DEPS  := $(BIGLOCK_OBJS:.o=.d)
BIGLOCK_LIB   := $(filter-out $(BIGLOCK_DIR)/main.o,$(BIGLOCK_OBJS))

BENCH_OBJS := $(patsubst $(BENCH_DIR)/%.c,$(OBJ_DIR)/bench/%.o,$(wildcard $(BENCH_DIR)/*.c))
BENCH_DEPS := $(BENCH_OBJS:.o=.d)

VEC_BENCH_OBJS   := $(OBJ_DIR)/bench/vec_bench.o $(OBJ_DIR)/bench/vec_bench_util.o
GRAPH_BENCH_OBJS := $(OBJ_DIR)/bench/graph_bench.o

# v1.1：对拍用的"改造前执行器"冻结副本。
#   它只链进 exec_bench，**不进主二进制**（主二进制只链 LIB_OBJS）。
#   对拍完成后即可删（PLAN §1.6 R5）。
#
# exec_agg_ref 是聚合的**逐行参照实现**：
#   冻结副本诞生于聚合之前，跑不了聚合查询；
#   聚合这一块改成"列批 + hash 分组" vs "逐行 + 线性分组"对拍。
EXEC_BENCH_OBJS  := $(OBJ_DIR)/bench/exec_bench.o $(OBJ_DIR)/bench/exec_volcano.o \
                   $(OBJ_DIR)/bench/exec_agg_ref.o $(OBJ_DIR)/bench/exec_bench_diff.o

# v1.3 步骤 5：全文检索的基准（A4 合并代价 + A5 IO 可证）。
#   ft_bench.c        A4 与入口
#   ft_bench_a5.c     A5（单 term 检索的页读）
#   ft_bench_corpus.c 语料生成（可复现）
FT_BENCH_OBJS := $(OBJ_DIR)/bench/ft_bench.o $(OBJ_DIR)/bench/ft_bench_a4.o \
                 $(OBJ_DIR)/bench/ft_bench_a5.o \
                 $(OBJ_DIR)/bench/ft_bench_corpus.o

# v1.4 步骤 4：地理空间的基准（A5 · Z-order 与 R-tree 的窗口 IO 对照）。
GEO_BENCH_OBJS := $(OBJ_DIR)/bench/geo_bench.o

# v1.5 步骤 6：时序模态的压缩比基准（A3 · 三种数据形态 × 三种列编码）。
TS_BENCH_OBJS := $(OBJ_DIR)/bench/ts_bench.o

# 基准要连主库的全部对象（但不能连 main.o —— 它自己带 main）
LIB_OBJS   := $(filter-out $(OBJ_DIR)/main.o,$(OBJS))

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/bench/%.o: $(BENCH_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BENCH_DIR) -MMD -MP -c -o $@ $<

# ---- A3：大锁基线那一份目标文件（-DDB_BIGLOCK_BASELINE） ----
#
# 单独一套 build_biglock/ 目录，不与主构建抢 .o ——
# 两个 make 并发写同一个 .o 会写坏（gcc 直接写目标文件，不走临时文件）。
$(BIGLOCK_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BIGLOCK_FLAGS) -MMD -MP -c -o $@ $<

$(BIGLOCK_DIR)/bench/%.o: $(BENCH_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(BIGLOCK_FLAGS) -I$(BENCH_DIR) -MMD -MP -c -o $@ $<

# ---- A3 的两个基准可执行文件 ----
$(LATCH_BENCH_EXE): $(OBJ_DIR)/bench/latch_bench.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(LATCH_BENCH_BIGLOCK_EXE): $(BIGLOCK_DIR)/bench/latch_bench.o $(BIGLOCK_LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# 基准 + 参照工具
#
# exec_bench（v1.1）：执行器基准 + 新旧执行器对拍。
#   它额外链 exec_volcano.o —— 那是"改造前执行器"的冻结副本，
#   只存在于这个基准里，主二进制（db.exe）不链它（PLAN §1.6 R5）。
bench: $(VEC_BENCH_EXE) $(GRAPH_BENCH_EXE) $(REF_EXE) $(EXEC_BENCH_EXE) $(LATCH_BENCH_EXE) $(LATCH_BENCH_BIGLOCK_EXE) $(FT_BENCH_EXE) $(GEO_BENCH_EXE) $(TS_BENCH_EXE)

$(TS_BENCH_EXE): $(TS_BENCH_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TS_BENCH_OBJS) $(LIB_OBJS) $(LDLIBS) -lm

$(GEO_BENCH_EXE): $(GEO_BENCH_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $(GEO_BENCH_OBJS) $(LIB_OBJS) $(LDLIBS) -lm

$(FT_BENCH_EXE): $(FT_BENCH_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $(FT_BENCH_OBJS) $(LIB_OBJS) $(LDLIBS) -lm

$(VEC_BENCH_EXE): $(VEC_BENCH_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $(VEC_BENCH_OBJS) $(LIB_OBJS) $(LDLIBS) -lm

$(EXEC_BENCH_EXE): $(EXEC_BENCH_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $(EXEC_BENCH_OBJS) $(LIB_OBJS) $(LDLIBS)

$(GRAPH_BENCH_EXE): $(GRAPH_BENCH_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $(GRAPH_BENCH_OBJS) $(LIB_OBJS) $(LDLIBS)

# hnswlib 是 header-only 的 C++ 库，直接用 g++ 编
$(REF_EXE): tools/hnswlib_ref.cpp
	@mkdir -p $(dir $@)
	$(CXX) -O2 -std=c++17 -I tools/third_party/hnswlib -o $@ $<

# 一键开 ASan + UBSan。写存储层时建议全程开着跑
san:
	$(MAKE) clean
	$(MAKE) SAN="-fsanitize=address,undefined" OPT="-O1 -g"

# 依赖方向检查：低层不许 include 高层
deps:
	@sh tools/check_deps.sh

test: $(TARGET)
	@sh test/run.sh

check: deps test

clean:
	rm -rf $(OBJ_DIR) $(BIGLOCK_DIR) $(TARGET)

-include $(DEPS)
-include $(BENCH_DEPS)
-include $(BIGLOCK_DEPS)

.PHONY: all san deps test check clean bench
