> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# Clock-Sweep 置换算法

## 概述

本文档详细描述 Buffer Pool 使用的 Clock-Sweep 页面置换算法。

---

## 一、算法原理

```mermaid
flowchart TD
    Start[开始扫描] --> GetCurrent[获取当前 Clock 指针位置]

    GetCurrent --> CheckRef{检查 ref_count}
    CheckRef -->|> 0| Skip1[跳过<br/>有线程引用]
    CheckRef -->|= 0| CheckUsage{检查 usage_count}

    CheckUsage -->|> 0| Decrement[usage_count--]
    CheckUsage -->|= 0| SelectVictim[选择为淘汰页]

    Skip1 --> Advance[移动 Clock 指针]
    Decrement --> Advance
    SelectVictim --> CheckDirty{检查脏页标志}

    Advance --> GetCurrent

    CheckDirty -->|是| Flush[刷写脏页]
    CheckDirty -->|否| Return[返回淘汰页]

    Flush --> Return
```

---

## 二、数据结构

```mermaid
classDiagram
    class ClockSweep {
        +uint64_t* clock_hand
        +uint64_t* clock_end
        +uint64_t* current_pos
        +buffer_t* buffers
        +int num_buffers
        +buffer_id_t select_victim()
        +void advance_hand()
    }

    class BufferEntry {
        +page_id_t page_id
        +int ref_count
        +bool is_dirty
        +uint64_t usage_count
        +bool is_pinned
    }

    ClockSweep "1" --> "*" BufferEntry
```

---

## 三、算法流程详解

### 3.1 扫描过程

```mermaid
sequenceDiagram
    participant BufMgr as Buffer Manager
    participant Clock as Clock-Sweep
    participant BufArray as Buffer 数组

    BufMgr->>Clock: select_victim()

    loop 遍历 Buffer 数组
        Clock->>BufArray: 获取当前 buffer

        alt ref_count > 0
            Clock->>Clock: 跳过，被引用
        else ref_count == 0
            alt usage_count > 0
                Clock->>BufArray: usage_count--
                Clock->>Clock: 给予第二次机会
            else usage_count == 0
                Clock->>BufArray: 选择为淘汰页
                alt is_dirty
                    BufMgr->>BufMgr: 标记为需要刷写
                end
                Clock-->>BufMgr: 返回 victim_id
            end
        end

        Clock->>Clock: advance_hand()
    end
```

### 3.2 访问次数更新

```mermaid
flowchart TD
    Start[页面被访问] --> GetPage[get_page(page_id)]
    GetPage --> CheckHit{页面在缓存中?}

    CheckHit -->|是| UpdateRef[ref_count++]
    UpdateRef --> UpdateUsage[usage_count++]
    UpdateUsage --> ReturnPage[返回页面]

    CheckHit -->|否| LoadPage[从磁盘加载]
    LoadPage --> SetRef[ref_count = 1]
    SetRef --> SetUsage[usage_count = 1]
    SetUsage --> ReturnPage
```

---

## 四、与 FIFO/LRU 对比

| 算法 | 优点 | 缺点 | 适用场景 |
|------|------|------|----------|
| **FIFO** | 简单、开销小 | 不考虑访问频率 | 顺序访问模式 |
| **LRU** | 考虑最近访问 | 需要维护链表，开销大 | 局部性强的场景 |
| **Clock-Sweep** | 近似 LRU，开销小 | 不是精确 LRU | 通用场景，平衡性能 |

---

## 五、性能特征

```mermaid
flowchart LR
    subgraph "Clock-Sweep 性能指标"
        AVG_SCAN[平均扫描长度<br/>num_buffers / 2]
        HIT_RATE[命中率<br/>> 95%]
        OVERHEAD[每次扫描开销<br/>O(num_buffers)]
    end
```

---

## 六、关键代码位置

| 功能 | 源文件 |
|------|--------|
| Clock-Sweep 实现 | `engine/src/db/storage/buffer/bufmgr.c` |
| Buffer 管理 | `engine/include/db/buf.h` |