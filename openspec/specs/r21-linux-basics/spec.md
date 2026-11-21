> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# R21 Linux 基础 — 能力规格

## 概述

R21 Linux 基础能力模块，涵盖 Linux 系统性能诊断与底层机制，共 **14 张学习卡**。

## 能力卡片

### 1. proc_fs — /proc 文件系统接口

**主题**：Linux /proc 虚拟文件系统

**核心能力**：
- 读取 `/proc/meminfo` 解析内存统计（MemTotal/MemFree/MemAvailable）
- 读取 `/proc/cpuinfo` 获取 CPU 型号、核心数
- 读取 `/proc/self/status` 获取进程状态信息

**工程对照**：`engineering/src/db/storage/bufmgr.c` Buffer Pool 诊断接口设计

---

### 2. cpu_diag — CPU 诊断

**主题**：CPU 性能监控

**核心能力**：
- 使用 `sysconf(_SC_NPROCESSORS_CONF)` 获取核心数
- 采样 `/proc/stat` 计算 CPU 利用率（user/nice/system/idle/iowait）
- 解析 `/proc/stat` 获取上下文切换次数（ctxt/intr/softirq）
- 遍历 `/proc/[pid]/stat` 列出 CPU 占用最高的进程

**工程对照**：数据库查询执行器的 CPU 开销分析

---

### 3. mem_diag — 内存诊断

**主题**：内存使用与泄漏检测

**核心能力**：
- 解析 `/proc/meminfo` 关键指标
- 演示 `malloc/free` 的内存分配与释放
- 内存泄漏检测思路：记录分配/释放计数差

**工程对照**：`engineering/src/db/storage/bufmgr.c` 内存管理机制

---

### 4. disk_iostat — 磁盘 I/O 诊断

**主题**：磁盘 I/O 性能分析

**核心能力**：
- 读取 `/proc/diskstats` 获取磁盘读写次数、扇区数
- 计算 IOPS（每秒 I/O 操作数）和吞吐量（MB/s）
- 解析设备名与分区信息

**工程对照**：数据库 I/O 性能瓶颈定位

---

### 5. htop — 进程监控原理

**主题**：进程监控工具实现

**核心能力**：
- 遍历 `/proc/*/stat` 获取所有进程信息
- 解析进程名、CPU 时间、内存占用
- 构建简单的进程树（PID → PPID 映射）
- 按 CPU/内存排序显示 Top 进程

**工程对照**：数据库连接监控与进程状态追踪

---

### 6. sar — 系统活动报告

**主题**：历史性能数据收集

**核心能力**：
- 使用 `sysinfo()` 系统调用获取系统概览
- 采样 `/proc/stat` 收集 CPU、内存、I/O 统计
- 数据格式化输出（类似 sar 命令）

**工程对照**：数据库性能监控与历史数据分析

---

### 7. tcp_state — TCP 连接状态

**主题**：TCP 状态机与连接统计

**核心能力**：
- 读取 `/proc/net/tcp` 解析 TCP 连接表
- 识别 TCP 11 种状态（ESTABLISHED/SYN_SENT/TIME_WAIT 等）
- 统计各状态连接数

**工程对照**：数据库连接池状态监控

---

### 8. socket_buf — Socket 缓冲区

**主题**：网络缓冲区调优

**核心能力**：
- 使用 `getsockopt/getsockopt` 获取/设置缓冲区大小
- 演示 `SO_SNDBUF` / `SO_RCVBUF` 参数
- 分析缓冲区大小对性能的影响

**工程对照**：`engineering/src/db/network/` 网络缓冲区设计

---

### 9. tcpdump — 网络抓包原理

**主题**：原始套接字与协议解析

**核心能力**：
- 创建 `SOCK_RAW` 原始套接字
- 接收以太网帧、IP 头、TCP 头
- 简单协议解析（Ethernet → IP → TCP）

**工程对照**：数据库网络协议分析与调试

---

### 10. conn_pool — 连接池

**主题**：资源池化技术

**核心能力**：
- 实现连接池：预创建、获取、归还机制
- 空闲连接超时回收
- 线程安全的连接管理

**工程对照**：`engineering/src/db/pool/` 数据库连接池实现

---

### 11. syscall — 系统调用

**主题**：Linux 系统调用接口

**核心能力**：
- 使用 `open/read/write/close` 进行文件 I/O
- 使用 `mmap` 进行内存映射 I/O
- 错误处理：`errno` 的使用与 `perror`

**工程对照**：`engineering/src/db/storage/page/disk.c` 页面 I/O 系统调用

---

### 12. pagecache — 页缓存

**主题**：文件 I/O 与缓存

**核心能力**：
- 使用 `posix_fadvise` 提示文件预读策略
- 演示 `readahead` 预读机制
- 讨论直接 I/O（O_DIRECT）绕过缓存

**工程对照**：数据库 Buffer Pool 与页缓存协同

---

### 13. cfs — CFS 调度器

**主题**：Linux 进程调度

**核心能力**：
- 读取 `/proc/sched_debug` 获取调度信息
- 理解虚拟运行时间（vruntime）概念
- 分析调度延迟与公平性

**工程对照**：数据库查询优先级与调度

---

### 14. vm — 虚拟内存

**主题**：虚拟内存管理

**核心能力**：
- 使用 `mmap` 创建内存映射
- 使用 `mprotect` 修改内存保护
- 使用 `madvise` 提示内存使用策略
- 理解缺页中断与页面换入/换出

**工程对照**：数据库 Buffer Pool 与虚拟内存交互

---

## 验证标准

| 验证项 | 标准 |
|--------|------|
| 文件完整性 | 14 张卡 × 4 文件（main.c/Makefile/README.md/NOTES.md）= 56 文件 |
| statuses.json | 14 个 linux 栈卡片状态为 "done" |
| r21-progress.md | 14 行，每行对应一张卡 |
| NOTES.md | 每张卡工程对照 ≥100 字 |
| 编译通过 | 所有 main.c 使用 `gcc -std=c11` 可编译 |

## 影响范围

- **新增文件**：56 个 scaffold 文件
- **修改文件**：`statuses.json`、`r21-progress.md`
- **不影响**：`engineering/` 源码（双轨隔离）
