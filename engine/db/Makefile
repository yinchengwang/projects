# 编译配置
# 关键：-fno-strict-aliasing 别省，页缓冲里大量类型双关，开优化后容易出诡异的错

# Windows 下 make 默认用 cmd.exe，$(shell find) 会失败，强制用 sh
SHELL   := sh

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
OBJ_DIR := build
SRCS    := $(shell find $(SRC_DIR) -name '*.c')
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
else
VEC_BENCH_EXE   := $(OBJ_DIR)/vec_bench
GRAPH_BENCH_EXE := $(OBJ_DIR)/graph_bench
REF_EXE         := $(OBJ_DIR)/hnswlib_ref
EXEC_BENCH_EXE  := $(OBJ_DIR)/exec_bench
endif

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

# 基准 + 参照工具
#
# exec_bench（v1.1）：执行器基准 + 新旧执行器对拍。
#   它额外链 exec_volcano.o —— 那是"改造前执行器"的冻结副本，
#   只存在于这个基准里，主二进制（db.exe）不链它（PLAN §1.6 R5）。
bench: $(VEC_BENCH_EXE) $(GRAPH_BENCH_EXE) $(REF_EXE) $(EXEC_BENCH_EXE)

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
	rm -rf $(OBJ_DIR) $(TARGET)

-include $(DEPS)
-include $(BENCH_DEPS)

.PHONY: all san deps test check clean bench
