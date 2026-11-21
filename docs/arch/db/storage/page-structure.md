> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 页面结构

## 概述

本文档描述数据库存储引擎的页面结构，页面是存储的基本单位，默认大小为 8KB。

---

## 一、页面布局

### 1.1 整体结构

```mermaid
flowchart TB
    subgraph "页面整体布局 (8KB = 8192 字节)"
        HEADER[页面头部<br/>16 字节]
        DATA_AREA[数据区<br/>8176 字节]
    end

    subgraph "页面头部结构 (16 字节)"
        PAGE_ID[page_id<br/>4 字节]
        PAGE_TYPE[page_type<br/>1 字节]
        CHECKSUM[checksum<br/>2 字节]
        FREE_OFF[free_space_offset<br/>2 字节]
        RESERVED[reserved<br/>7 字节]
    end

    HEADER --> PAGE_ID
    HEADER --> PAGE_TYPE
    HEADER --> CHECKSUM
    HEADER --> FREE_OFF
    HEADER --> RESERVED
```

### 1.2 页面类型

```mermaid
flowchart LR
    subgraph "页面类型枚举"
        PAGE_FREE[FREE=0<br/>空闲页]
        PAGE_DATA[DATA=1<br/>数据页]
        PAGE_INDEX[INDEX=2<br/>索引页]
        PAGE_OVERFLOW[OVERFLOW=3<br/>溢出页]
        PAGE_META[META=4<br/>元数据页]
    end
```

---

## 二、页面类型详解

### 2.1 数据页 (DATA)

```mermaid
flowchart TB
    subgraph "数据页布局"
        HDR[页面头部<br/>16 字节]
        ITEM_PTRS[项指针数组<br/>从后向前增长]
        FREE_SPACE[空闲空间<br/>中间区域]
        ITEMS[元组数据<br/>从前向后增长]
    end

    subgraph "项指针结构"
        ITEM_OFF[item_offset<br/>2 字节<br/>元组偏移]
        ITEM_LEN[item_length<br/>2 字节<br/>元组长度]
    end

    ITEM_PTRS --> ITEM1[ItemPointer 1<br/>4 字节]
    ITEM_PTRS --> ITEM2[ItemPointer 2<br/>4 字节]
    ITEM_PTRS --> ITEM_N[ItemPointer N<br/>4 字节]
    ITEM1 --> ITEM_OFF
    ITEM1 --> ITEM_LEN
```

### 2.2 索引页 (INDEX)

```mermaid
flowchart TB
    subgraph "BTree 索引页布局"
        HDR[页面头部<br/>16 字节]
        LSN[page_lsn<br/>8 字节<br/>最近修改的 LSN]
        FLAGS[flags<br/>2 字节<br/>页面标志]
        ITEM_COUNT[item_count<br/>2 字节<br/>条目数]
        UPPER[upper<br/>2 字节<br/>上层空闲起始]
        SPECIAL[special<br/>2 字节<br/>专用数据偏移]
        ITEMS_PTRS[条目指针<br/>从后向前]
        FREE_SPACE[空闲空间]
        SPECIAL_DATA[专用数据<br/>叶子/内部节点]

        BTreePage --> HDR
        BTreePage --> LSN
        BTreePage --> FLAGS
        BTreePage --> ITEM_COUNT
        BTreePage --> UPPER
        BTreePage --> SPECIAL
        BTreePage --> ITEMS_PTRS
        BTreePage --> FREE_SPACE
        BTreePage --> SPECIAL_DATA
    end

    subgraph "BTreePage 结构"
        BTreePage[BTree 页面<br/>标准页面 + 扩展]
    end
```

### 2.3 溢出页 (OVERFLOW)

```mermaid
flowchart LR
    subgraph "溢出页链"
        ORIG_PAGE[原始数据页<br/>存储前 8KB]
        OV_PAGE1[溢出页 1<br/>下一段数据]
        OV_PAGE2[溢出页 2<br/>下一段数据]
        OV_PAGE3[溢出页 3<br/>最后一段数据]
    end

    ORIG_PAGE -->|next_overflow| OV_PAGE1
    OV_PAGE1 -->|next_overflow| OV_PAGE2
    OV_PAGE2 -->|next_overflow| OV_PAGE3
    OV_PAGE3 -->|next_overflow = 0| NULL
```

### 2.4 元数据页 (META)

```mermaid
flowchart TB
    subgraph "元数据页布局"
        HDR[页面头部<br/>16 字节]
        TOT_TUPLES[total_tuples<br/>8 字节<br/>总元组数]
        TOT_PAGES[total_pages<br/>8 字节<br/>总页数]
        FREE_PAGES[free_pages<br/>8 字节<br/>空闲页数]
        FILL_FACTOR[fill_factor<br/>4 字节<br/>填充因子]
        TIMESTAMP[create_time<br/>8 字节<br/>创建时间戳]
        CUSTOM[custom_data<br/>变长<br/>自定义元数据]
    end
```

---

## 三、页面操作流程

### 3.1 页面创建

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant PageMgr as 页面管理器
    participant Header as 页面头部

    Caller->>PageMgr: page_create(page_id, type)

    PageMgr->>PageMgr: 分配 page_t 内存
    PageMgr->>Header: 设置 page_id
    PageMgr->>Header: 设置 page_type
    PageMgr->>Header: 初始化 free_space_offset = 0
    PageMgr->>Header: 清空校验和

    PageMgr->>PageMgr: 清空数据区 (memset 0)
    PageMgr->>PageMgr: 计算并设置校验和

    PageMgr-->>Caller: 返回 page_t* 指针
```

### 3.2 空间分配

```mermaid
flowchart TD
    Start[page_alloc_space(page, size)] --> Align[对齐 size 到 4 字节]

    Align --> Check{检查空闲空间<br/>PAGE_DATA_SIZE - free_off >= size?}

    Check -->|足够| Alloc[current_off = free_off<br/>free_off += size]
    Check -->|不足| Fail[返回 (uint16_t)-1]

    Alloc --> Return[current_off]

    Fail --> Return
```

### 3.3 校验和计算

```mermaid
flowchart TD
    Start[page_set_checksum(page)] --> Calc[使用 CRC16 计算<br/>头部 + 数据区]

    Calc --> Hash[计算校验和值]
    Hash --> Store[写入 header.checksum]
    Store --> Done[完成]

    Start2[page_verify_checksum(page)] --> Read[读取 header.checksum]
    Read --> Recalc[重新计算校验和]
    Recalc --> Compare{比较}

    Compare -->|相等| Valid[返回 true]
    Compare -->|不等| Invalid[返回 false]
```

---

## 四、页面生命周期

```mermaid
stateDiagram-v2
    [*] --> Allocated: page_create()
    Allocated --> Clean: 分配后初始化
    Clean --> Dirty: 修改数据
    Dirty --> Clean: 刷盘 (flush)
    Clean --> Free: 释放页面
    Dirty --> Free: 释放前刷盘
    Free --> Allocated: 重新分配
    Allocated --> [*]: 删除文件

    note right of Allocated: 刚创建或<br/>重新分配
    note right of Dirty: 已修改<br/>未刷盘
    note right of Clean: 内容与<br/>磁盘一致
```

---

## 五、页面操作 API

| 函数 | 功能 | 复杂度 |
|------|------|--------|
| `page_create()` | 创建新页面 | O(1) |
| `page_free()` | 释放页面 | O(1) |
| `page_get_free_space()` | 获取空闲空间 | O(1) |
| `page_get_used_space()` | 获取已用空间 | O(1) |
| `page_alloc_space()` | 分配空间 | O(1) |
| `page_set_checksum()` | 设置校验和 | O(n) |
| `page_verify_checksum()` | 验证校验和 | O(n) |
| `page_write_data()` | 写入数据 | O(1) |
| `page_read_data()` | 读取数据 | O(1) |

---

## 六、关键代码位置

| 功能 | 头文件 | 源文件 |
|------|--------|--------|
| 页面结构定义 | `engine/include/db/page.h` | - |
| 页面操作实现 | `engine/include/db/page.h` | `engine/src/db/storage/page/page.c` |
| 项指针管理 | `engine/include/db/page.h` | `engine/src/db/storage/page/page.c` |
| 校验和计算 | `engine/include/db/page.h` | `engine/src/db/storage/page/page.c` |