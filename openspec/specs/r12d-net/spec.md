> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# R12d 网络编程 — 规格定义

## 概述

R12d 网络编程系列包含 4 张卡片，涵盖 Linux 网络编程基础、事件驱动、性能优化和问题诊断。

## 卡片清单

| ID | 目录 | 说明 | 状态 |
|----|------|------|------|
| socket | `learning/scaffold/linux/socket/` | socket/bind/listen/accept/connect + TCP/UDP | done |
| epoll | `learning/scaffold/linux/epoll/` | epoll_create/ctl/wait + LT/ET + Reactor | done |
| network_debug | `learning/scaffold/linux/network_debug/` | tcpdump/netstat/ss + 连接状态诊断 | done |
| zero_copy | `learning/scaffold/linux/zero_copy/` | sendfile/mmap + 零拷贝原理 | done |

## 技术规格

### socket 卡片

- **文件**: `main.c` (~360 行)
- **内容**:
  - TCP Server: socket → bind → listen → accept → recv/send
  - TCP Client: socket → connect → send/recv
  - UDP: socket → bind → sendto/recvfrom
  - Socket 选项: SO_REUSEADDR, SO_KEEPALIVE, TCP_NODELAY, SO_RCVTIMEO
- **编译**: `gcc -Wall -Wextra -std=c11`

### epoll 卡片

- **文件**: `main.c` (~400 行)
- **内容**:
  - epoll_create1/ctl/wait 基础用法
  - LT (Level-Triggered) 模式
  - ET (Edge-Triggered) 模式
  - Reactor 事件循环模式
- **编译**: `gcc -Wall -Wextra -std=c11`

### network_debug 卡片

- **文件**: `main.sh` (~240 行)
- **内容**:
  - tcpdump 抓包基础
  - netstat/ss 查看连接状态
  - TIME_WAIT/CLOSE_WAIT 问题处理
  - 网络延迟测量
- **运行**: `bash main.sh`

### zero_copy 卡片

- **文件**: `main.c` (~320 行)
- **内容**:
  - 传统 read/write（4 次拷贝）
  - mmap 内存映射（3 次拷贝）
  - sendfile 零拷贝（2 次拷贝）
  - 性能对比测量
- **编译**: `gcc -Wall -Wextra -std=c11`

## 工程对照

| 卡片 | 工程源码 | 说明 |
|------|---------|------|
| socket | `engineering/src/db/api/rest_api.c` | socket/bind/listen/accept/closesocket |
| epoll | `engineering/src/db/api/` | 当前使用 pthread 单线程 accept，未来可演进为 epoll |
| network_debug | - | 网络问题排查思路（TIME_WAIT/CLOSE_WAIT 处理） |
| zero_copy | `engineering/src/db/storage/page/disk.c` | 使用 pread/pwrite，当前无零拷贝优化 |

## 学习路径

1. **socket** → 理解网络通信基础
2. **epoll** → 掌握高性能事件驱动
3. **network_debug** → 学会问题诊断
4. **zero_copy** → 理解 I/O 性能优化

## 归档信息

- **归档日期**: 2026-07-12
- **变更**: r12d-net
- **状态**: 已完成
