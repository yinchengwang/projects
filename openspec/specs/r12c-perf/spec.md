> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# R12c 性能分析 Spec

## 概述

R12c 覆盖 Linux 性能分析工具链 + 火焰图实战，共 4 张卡：
- `profiling_cpp`: perf/top/htop + 进程级→线程级→函数级分析链路
- `flamegraph`: 火焰图生成/解读 + 叶子热点识别 + 优化案例
- `gdb_cpp`: GDB 高级用法 + 条件断点/watch point + 调试优化代码
- `clang_tidy`: 静态分析 + clang-tidy 规则配置 + CMake 集成

## Scaffold 结构

每张卡包含：
- `main.cpp` (~100-150 行): 演示代码，含跨平台支持 (#ifdef _WIN32 / #ifdef __linux__)
- `README.md`: 工具用法速查 + 分析链路 + Windows/WSL 说明
- `NOTES.md` (≥100 字): 工程对照，引用 `engineering/src/db/` 核心模块
- 部分卡有额外配置文件（Makefile、shell 脚本、.clang-tidy、gdb_commands.txt）

## 工程对照知识点

| 卡 ID | 对照模块 | 对照内容 |
|-------|----------|----------|
| profiling_cpp | `performance.c` | `parallel_executor_t`/`simd_sum_double`/`buffer_get_page` 的 perf top 热点定位 |
| flamegraph | `bufmgr.c`/`wal.c`/`btreeam.c` | Buffer Pool hash_search、WAL fsync 等火焰图热点解读 |
| gdb_cpp | `guc.c` | GUC 参数解析的条件断点、Buffer Pool LRU 的 watch point |
| clang_tidy | `CMakeLists.txt` | clang-tidy 集成方式、bufmgr/heapam/btreeam/wal 的规则检查 |

## 验收标准

1. statuses.json 4 张卡状态为 done
2. r12c-progress.md 有 4 行审计记录（含 commit SHA）
3. NOTES.md 工程对照 ≥100 字
4. main.cpp 编译/运行通过
5. 双轨核心库编译通过