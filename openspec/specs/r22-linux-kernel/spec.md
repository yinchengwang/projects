> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# R22 Linux 内核 — 规格沉淀

## 能力规格

### 1. direct_io（Direct I/O）

| 维度 | 规格 |
|------|------|
| 系统调用 | `open(path, O_DIRECT)`, `posix_memalign()` |
| 对齐要求 | 缓冲区 512B / 4KB 对齐 |
| 文件系统支持 | ext4/xfs 支持，tmpfs 不支持 |
| 工程应用 | MySQL `innodb_flush_method=O_DIRECT`, PostgreSQL `effective_io_concurrency` |

### 2. hugepages（HugePages）

| 维度 | 规格 |
|------|------|
| 页大小 | 2MB / 1GB |
| sysfs 接口 | `/proc/meminfo` (HugePages_*), `/sys/kernel/mm/hugepages/` |
| mmap 标志 | `MAP_HUGETLB` |
| 数据库配置 | PG `huge_pages=try`, MySQL `large_pages=1` |
| TLB 优化 | 1GB 内存: 262144×4KB 条目 → 512×2MB 条目 |

### 3. fsync（fsync 机制）

| 维度 | 规格 |
|------|------|
| 内核路径 | `submit_bio` → Page Cache → `pdflush/flusher` → 磁盘 |
| WAL 路径 | `wal.c: XLogWrite()` → `issue_xlog_fsync()` → `pg_fsync()` → `fdatasync()` |
| 组提交 | 多事务共享一次 fsync，`wal_writer_delay` 控制频率 |
| 性能差异 | fsync 约 123x vs 仅 write（HDD） |

### 4. io_scheduler（I/O 调度器）

| 维度 | 规格 |
|------|------|
| 调度器类型 | CFQ / Deadline / NOOP / Kyber / BFQ / mq-deadline |
| sysfs 接口 | `/sys/block/*/queue/scheduler` |
| NVMe 推荐 | `none`（硬件自行调度） |
| HDD 推荐 | `mq-deadline` / `bfq` |
| 数据库场景 | WAL 用 Deadline，数据用 none（SSD） |

### 5. strace（strace 原理）

| 维度 | 规格 |
|------|------|
| ptrace 调用 | `PTRACE_TRACEME` → `PTRACE_SYSCALL` |
| 停止类型 | syscall-enter-stop / syscall-exit-stop |
| 性能开销 | ~20-50%（频繁系统调用场景） |
| 替代方案 | perf trace（5-10% 开销）, bpftrace（1-5% 开销） |

### 6. seccomp（Seccomp）

| 维度 | 规格 |
|------|------|
| 模式 | SECCOMP_MODE_STRICT（仅 4 个 syscall）/ SECCOMP_MODE_FILTER（BPF） |
| prctl 接口 | `PR_SET_SECCOMP`, `PR_GET_SECCOMP` |
| BPF 返回值 | ALLOW / KILL_PROCESS / ERRNO / TRAP |
| 容器集成 | Docker 默认禁用 ~44 个危险 syscall |

### 7. ext4_xfs（文件系统）

| 维度 | 规格 |
|------|------|
| ext4 特性 | extent 映射、延迟分配（delalloc）、内联数据、jbd2 日志 |
| xfs 特性 | AG（分配组）并行、B+ 树元数据、校验和 |
| 日志模式 | writeback / ordered / data（ext4） |
| 数据库选型 | <100GB 用 ext4，>10TB 或高并发用 xfs |

### 8. disk_ssd（SSD 特性）

| 维度 | 规格 |
|------|------|
| NAND 类型 | SLC(10万PE) / MLC(3千-1万PE) / TLC(1-3千PE) / QLC(500-1千PE) |
| 写放大 | WAF = NAND 写入量 / 主机写入量（随机小写 3-10x） |
| TRIM 接口 | `BLKDISCARD` ioctl / `fstrim` 命令 |
| NVMe 队列 | 64K 队列 × 64K 深度，per-CPU 映射 |
| 数据库优化 | WAL 放 SSD、fstrim 定期执行、压缩减少写入量 |

### 9. block_layer（块设备层）

| 维度 | 规格 |
|------|------|
| bio 结构 | `bi_sector` + `bi_size` + `bi_io_vec`（bvec 列表） |
| blk-mq | 软件队列（per-CPU，无锁）+ 硬件队列（per-device） |
| 请求合并 | 前向合并 / 后向合并 / 插入 |
| 数据库应用 | 页面大小对齐（MySQL 16KB / PG 8KB），WAL 顺序写合并 |

### 10. lvm（LVM 逻辑卷）

| 维度 | 规格 |
|------|------|
| 三层架构 | PV（物理卷）→ VG（卷组）→ LV（逻辑卷） |
| 快照机制 | Copy-on-Write（COW），存储变化块的前镜像 |
| thin-pool | 按需分配，提交空间 > 物理空间（超分配） |
| 数据库应用 | 在线扩容、快照射份（WAL + 快照 + PITR） |

### 11. fallocate（空间预分配）

| 维度 | 规格 |
|------|------|
| mode=0 | 预分配块，不改变文件大小 |
| FALLOC_FL_KEEP_SIZE | 预分配但不改变文件大小 |
| FALLOC_FL_PUNCH_HOLE | 释放块，形成空洞 |
| FALLOC_FL_ZERO_RANGE | 清零指定范围，保证块已分配 |
| 数据库应用 | WAL segment 16MB 预分配，减少碎片和延迟波动 |

### 12. fdatasync（数据同步）

| 维度 | 规格 |
|------|------|
| fdatasync vs fsync | 仅同步数据，跳过 mtime/atime 更新（文件大小不变时） |
| WAL 优化 | 预分配固定大小文件，fdatasync 可跳过 inode 更新 |
| 同步策略 | 组提交、异步提交（synchronous_commit=off） |
| 性能 | fdatasync ~10-20% 节省 vs fsync（WAL 场景） |

## 工程源码对照

| 卡片 | 源码文件 | 对照内容 |
|------|---------|---------|
| fsync | `engineering/src/db/storage/wal/wal.c` | `XLogWrite()` → `issue_xlog_fsync()` → `pg_fsync()` |
| ext4_xfs | `engineering/src/db/storage/page/disk.c` | `detect_fs_type()` → I/O 策略选择 |
| lvm | `engineering/include/db/mm_storage.h` | 多模态存储引擎分层（VG 类比） |
| lvm | `engineering/src/db/storage/page/disk.c` | `disk_extend()` → lvextend 类比 |
| fallocate | `engineering/src/db/storage/page/disk.c` | `disk_init_sparse()` → thin-pool 类比 |
| block_layer | `engineering/src/db/storage/page/disk.c` | 页面分配 extent 对齐 |
| fdatasync | `engineering/src/db/storage/wal/wal.c` | 组提交 + fdatasync 优化 |

## 归档信息

- **变更名称**: r22-linux-kernel
- **归档时间**: 2026-07-12
- **scaffold 路径**: `learning/scaffold/linux/{direct_io,hugepages,fsync,io_scheduler,strace,seccomp,ext4_xfs,disk_ssd,block_layer,lvm,fallocate,fdatasync}/`
- **done count**: 158（146 原有 + 12 R22）
- **验收标准**: 12 张卡 scaffold（main.c/Makefile/README.md/NOTES.md）× 12 = 48 文件 + statuses.json + r22-progress.md
