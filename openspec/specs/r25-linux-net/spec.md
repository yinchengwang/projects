> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# R25 Linux 网络基础 — 规格沉淀

## 概述

R25 Linux 网络基础能力覆盖 10 张卡，涵盖 epoll 事件驱动、零拷贝技术、CPU 亲和性、Unix 域 socket、SO_REUSEPORT 负载均衡、iptables 防火墙、网络命名空间、veth 虚拟网卡、网桥配置、XDP 快速路径等核心主题。

## 能力规格

### 1. epoll（epoll 事件驱动）

| 属性 | 值 |
|------|-----|
| 主题 | epoll 高性能事件驱动 |
| 核心概念 | epoll_create/epoll_ctl/epoll_wait、LT vs ET 模式、Reactor 模式 |
| 工程对照 | `engineering/src/db/api/` REST 服务器的 epoll 演进方向 |
| 文件 | main.c + Makefile + README.md + NOTES.md |

### 2. zero_copy（零拷贝技术）

| 属性 | 值 |
|------|-----|
| 主题 | Linux 零拷贝技术 |
| 核心概念 | sendfile/splice/mmap、DMA 传输、4次→2次拷贝 |
| 工程对照 | 高性能网络传输、文件服务优化 |
| 文件 | main.c + Makefile + README.md + NOTES.md |

### 3. cpu_affinity（CPU 亲和性）

| 属性 | 值 |
|------|-----|
| 主题 | CPU 亲和性配置 |
| 核心概念 | sched_setaffinity、pthread_setaffinity_np、NUMA 亲和 |
| 工程对照 | 网络处理优化、多线程服务器 CPU 绑定 |
| 文件 | main.c + Makefile + README.md + NOTES.md |

### 4. unix_socket（Unix Domain Socket）

| 属性 | 值 |
|------|-----|
| 主题 | Unix 域 socket 通信 |
| 核心概念 | AF_UNIX、SOCK_STREAM vs SOCK_DGRAM、本地进程通信 |
| 工程对照 | 本地进程 IPC、Redis 通信优化 |
| 文件 | main.c + Makefile + README.md + NOTES.md |

### 5. so_reuseport（SO_REUSEPORT）

| 属性 | 值 |
|------|-----|
| 主题 | SO_REUSEPORT 负载均衡 |
| 核心概念 | 多监听 socket、端口复用、内核级负载均衡 |
| 工程对照 | Nginx/Redis 多进程监听、多核扩展 |
| 文件 | main.c + Makefile + README.md + NOTES.md |

### 6. iptables（iptables 防火墙）

| 属性 | 值 |
|------|-----|
| 主题 | iptables 防火墙配置 |
| 核心概念 | 四表五链、NAT、连接追踪、规则匹配 |
| 工程对照 | 数据库服务器防火墙、白名单、端口限制 |
| 文件 | main.c + Makefile + README.md + NOTES.md |

### 7. namespace（网络命名空间）

| 属性 | 值 |
|------|-----|
| 主题 | Linux 网络命名空间 |
| 核心概念 | unshare、ip netns、网络隔离、容器网络基础 |
| 工程对照 | Kubernetes Pod 网络隔离、Docker 网络原理 |
| 文件 | main.c + Makefile + README.md + NOTES.md |

### 8. veth（虚拟以太网设备）

| 属性 | 值 |
|------|-----|
| 主题 | veth pair 虚拟网线 |
| 核心概念 | veth pair、peer 配置、命名空间连接 |
| 工程对照 | 容器网络实现、Pod 网络与 veth 的关系 |
| 文件 | main.c + Makefile + README.md + NOTES.md |

### 9. bridge（网桥配置）

| 属性 | 值 |
|------|-----|
| 主题 | Linux 网桥配置 |
| 核心概念 | brctl、ip link、二层转发、VLAN |
| 工程对照 | Docker 网桥模式、Kubernetes CNI |
| 文件 | main.c + Makefile + README.md + NOTES.md |

### 10. xdp（XDP 快速路径）

| 属性 | 值 |
|------|-----|
| 主题 | XDP (eXpress Data Path) |
| 核心概念 | eBPF 程序、驱动层处理、BPF 映射、高性能网络 |
| 工程对照 | Cilium/DPDK、Cloudflare DDoS 防护 |
| 文件 | main.c + Makefile + README.md + NOTES.md |

## 技术栈关联

- **前置知识**：R21 Linux 基础、R24 Linux 存储
- **延伸知识**：R26 Linux 高级网络
- **数据库关联**：R12 DB 索引系统、R13 DB 事务（网络层）

## 验收标准

- [x] 10 张卡 scaffold 完整（main.c + Makefile + README.md + NOTES.md）
- [x] 每张卡 NOTES.md 工程对照 ≥100 字
- [x] 10 张卡在 Linux 环境可编译/运行
- [x] statuses.json 更新为 done
