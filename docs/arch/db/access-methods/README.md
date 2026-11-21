> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 访问方法层 - 架构设计

## 概述

访问方法层（Access Method Layer）是 db 数据库存储引擎的核心抽象层，负责统一管理表、索引等数据对象的存储和访问。参考 PostgreSQL 的 Relation + HeapAM + BTreeAM 设计。

---

## 一、子系统架构概览

```mermaid
flowchart TB
    subgraph "上层调用者"
        SQL_EXEC[SQL 执行器<br/>sql_exec.c]
        INDEX_MGR[索引管理器<br/>index.c]
    end

    subgraph "访问方法层"
        subgraph "Relation 抽象层"
            REL[Relation 管理<br/>rel.c]
            TUPLE_DESC[TupleDesc<br/>行描述符]
            SCAN_KEY[ScanKey<br/>扫描键]
        end

        subgraph "访问方法实现"
            HEAP_AM[Heap AM<br/>heapam.c]
            BTREE_AM[BTree AM<br/>btreeam.c]
            HASH_AM[Hash AM<br/>hash.c]
            OTHER_AM[其他 AM<br/>gist/gin/brin]
        end

        subgraph "元数据依赖"
            CATALOG[Catalog 系统]
            BUFFER[Buffer Pool]
        end
    end

    SQL_EXEC --> REL
    INDEX_MGR --> REL
    REL --> HEAP_AM
    REL --> BTREE_AM
    REL --> HASH_AM
    REL --> OTHER_AM
    HEAP_AM --> CATALOG
    HEAP_AM --> BUFFER
    BTREE_AM --> CATALOG
    BTREE_AM --> BUFFER
```

---

## 二、Relation 抽象

### 2.1 Relation 核心结构

```mermaid
classDiagram
    class RelationData {
        +Oid rd_id
        +Oid rd_relid
        +RelKind rd_relkind
        +AccessMethodType rd_am
        +Oid rd_relfilenode
        +Oid rd_tablespace
        +int rd_nblocks
        +int rd_lockmode
        +TupleDesc rd_att
        +void* rd_amstate
        +void* rd_fd
        +void* rd_bufferPool
        +int rd_refcnt
        +lock_manager_t* rd_lockmgr
    }

    class TupleDescData {
        +int natts
        +int tdtypeid
        +int tdtypmod
        +bool tdhasoid
        +attr_info* attrs
    }

    class ScanKeyData {
        +int sk_attno
        +ScanKeyOp sk_op
        +Oid sk_procedure
        +void* sk_argument
        +size_t sk_arglen
    }

    class TableScanDescData {
        +Relation rs_rd
        +BlockNumber rs_startblock
        +BlockNumber rs_cblock
        +int rs_cindex
        +BufferDesc* rs_cbuf
        +void* rs_ctbuf
        +ScanKey rs_key
        +int rs_nkeys
        +ScanDirection rs_direction
    }

    RelationData "1" --> "1" TupleDescData
    RelationData "1" --> "*" ScanKeyData
    TableScanDescData --> RelationData
    TableScanDescData --> ScanKeyData
```

### 2.2 Relation 生命周期

```mermaid
stateDiagram-v2
    [*] --> Closed: 未打开

    Closed --> Opening: relation_open()
    Opening --> Open: 成功打开
    Opening --> Closed: 打开失败

    Open --> Scanning: table_beginscan()
    Scanning --> Open: table_endscan()
    Open --> Modifying: insert/update/delete
    Modifying --> Open: 操作完成

    Open --> Closing: relation_close()
    Closing --> Closed: 关闭完成

    Closed --> Destroyed: relation_drop()
    Destroyed --> [*]

    note right of Open: rd_refcnt > 0
```

---

## 三、Heap AM（堆表访问方法）

### 3.1 Heap AM 结构

```mermaid
classDiagram
    class HeapAM {
        +heapam_init()
        +heapam_shutdown()
        +heap_insert(rel, tuple, len, cid, options, bistate)
        +heap_delete(rel, tid, cid, crosscheck, wait)
        +heap_update(rel, tid, newtuple, newlen, cid, options, bistate, lockmode)
        +heap_lock_tuple(rel, tid, cid, mode, nowait, tm_result)
        +heap_getnext(scan, direction)
        +heap_getcurr(scan)
    }

    class HeapPage {
        +PageHeaderData pd_header
        +HeapLinePointerData* lp_array
        +uint8_t* data_area
    }

    class PageHeaderData {
        +uint32_t pd_lsn
        +uint16_t pd_checksum
        +uint16_t pd_flags
        +uint16_t pd_lower
        +uint16_t pd_upper
        +uint16_t pd_special
        +uint16_t pd_pagesize_version
        +uint32_t pd_prune_xid
    }

    class HeapLinePointerData {
        +uint32_t t_off
        +uint8_t t_flags
        +uint8_t t_xmax
    }

    HeapAM --> HeapPage : 操作
    HeapPage --> PageHeaderData
    HeapPage --> HeapLinePointerData
```

### 3.2 堆页面布局

```mermaid
flowchart TB
    subgraph "堆页面 (8KB)"
        PD_HEADER[PageHeaderData<br/>24 字节]
        LP1[LinePointer 1<br/>6 字节]
        LP2[LinePointer 2<br/>6 字节]
        LP3[LinePointer 3<br/>6 字节]
        FREE_SPACE[空闲空间<br/>pd_lower → pd_upper]
        TUPLE1[元组 1<br/>从后向前增长]
        TUPLE2[元组 2<br/>从后向前增长]
        SPECIAL[特殊空间<br/>pd_special → 页面末尾]
    end

    PD_HEADER --> LP1
    LP1 --> LP2
    LP2 --> LP3
    LP3 --> FREE_SPACE
    FREE_SPACE --> TUPLE2
    TUPLE2 --> TUPLE1
    TUPLE1 --> SPECIAL
```

### 3.3 元组插入流程

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant HeapAM as Heap AM
    participant Page as 堆页面
    participant Buffer as Buffer Pool
    participant WAL as WAL

    Caller->>HeapAM: heap_insert(rel, tuple, len, cid)

    HeapAM->>HeapAM: 构造元组描述符
    HeapAM->>HeapAM: 设置 xmin, xmax, cid

    HeapAM->>Buffer: 获取页面 (extend 或已有)
    Buffer-->>HeapAM: 返回页面指针

    HeapAM->>Page: 检查空闲空间

    alt 空间不足
        HeapAM->>Buffer: 申请新页面
        Buffer-->>HeapAM: 新页面
        HeapAM->>Page: 初始化新页面
    end

    HeapAM->>Page: 分配 LinePointer
    HeapAM->>Page: 复制元组数据
    HeapAM->>Page: 更新 pd_lower/pd_upper

    HeapAM->>WAL: 记录插入日志
    WAL-->>HeapAM: 返回 LSN

    HeapAM->>Page: 更新 pd_lsn
    HeapAM->>Buffer: 标记页面为脏

    HeapAM-->>Caller: 返回 0 (成功)
```

### 3.4 表扫描流程

```mermaid
sequenceDiagram
    participant Caller as 调用者
    HeapAM->>HeapAM: heap_getnext(scan, direction)

    loop 遍历页面
        HeapAM->>HeapAM: 检查当前页面是否有效

        alt 需要新页面
            HeapAM->>Buffer: 获取下一个页面
            Buffer-->>HeapAM: 页面数据
        end

        loop 遍历页面内的元组
            HeapAM->>HeapAM: 读取 LinePointer
            HeapAM->>HeapAM: 检查元组可见性

            alt 元组可见
                HeapAM->>HeapAM: 构造返回元组
                HeapAM-->>Caller: 返回元组指针
            else 元组不可见
                HeapAM->>HeapAM: 跳过，继续下一个
            end
        end
    end

    HeapAM-->>Caller: 返回 NULL (扫描结束)
```

---

## 四、BTree AM（BTree 索引访问方法）

### 4.1 BTree 结构

```mermaid
classDiagram
    class BTreeAM {
        +btcreate(rel)
        +btdestroy(rel)
        +btinsert(rel, values, nkeys, heap_ptr)
        +btdelete(rel, values, nkeys, heap_ptr)
        +btbuild(rel, tuples, ntuples)
        +bt_beginscan(rel, nkeys, key)
        +bt_getnext(scan, direction)
        +bt_compare(rel, key1, key2, nkeys)
    }

    class BTPageHeaderData {
        +uint16_t btpo_flags
        +uint16_t btpo_level
        +uint32_t btpo_prev
        +uint32_t btpo_next
        +uint32_t btpo_xact
        +uint16_t btpo_offset
        +uint16_t btpo_count
    }

    class BTreekirtData {
        +Oid heap_node
        +uint32_t block_number
        +uint16_t offset
        +uint16_t flags
        +char* key_data
    }

    class BTScanDescData {
        +Relation bt_relation
        +ScanKey bt_key
        +int bt_nkeys
        +ScanDirection bt_direction
        +BufferDesc* bt_curbuf
        +uint32_t bt_curpage
        +int bt_curitem
        +void* bt_curtuple
    }

    BTreeAM --> BTPageHeaderData
    BTreeAM --> BTreekirtData
    BTreeAM --> BTScanDescData
```

### 4.2 BTree 结构示意图

```mermaid
flowchart TB
    ROOT[根节点<br/>level=2<br/>5, 15, 25]
    INT1[内部节点<br/>level=1<br/>1, 3, 5]
    INT2[内部节点<br/>level=1<br/>7, 10, 15]
    INT3[内部节点<br/>level=1<br/>18, 22, 25]
    LEAF1[叶子节点<br/>level=0<br/>1, 2, 3]
    LEAF2[叶子节点<br/>level=0<br/>4, 5, 6]
    LEAF3[叶子节点<br/>level=0<br/>7, 8, 10]
    LEAF4[叶子节点<br/>level=0<br/>12, 15, 16]
    LEAF5[叶子节点<br/>level=0<br/>18, 20, 22]
    LEAF6[叶子节点<br/>level=0<br/>24, 25, 30]

    ROOT --> INT1
    ROOT --> INT2
    ROOT --> INT3

    INT1 --> LEAF1
    INT1 --> LEAF2
    INT2 --> LEAF3
    INT2 --> LEAF4
    INT3 --> LEAF5
    INT3 --> LEAF6

    LEAF1 --> LEAF2
    LEAF2 --> LEAF3
    LEAF3 --> LEAF4
    LEAF4 --> LEAF5
    LEAF5 --> LEAF6
```

### 4.3 BTree 插入流程

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant BTree as BTree AM
    participant Page as 索引页面
    participant Buffer as Buffer Pool

    Caller->>BTree: btinsert(rel, values, nkeys, heap_ptr)

    BTree->>BTree: 从根节点开始查找

    loop 逐层向下
        BTree->>Buffer: 获取当前页面
        Buffer-->>BTree: 页面数据

        alt 叶子页面
            BTree->>Page: 检查是否有空闲空间
            alt 空间足够
                BTree->>Page: 在正确位置插入
            else 空间不足
                BTree->>BTree: 页面分裂
                BTree->>Buffer: 分配新页面
                BTree->>Page: 半页数据移到新页面
                BTree->>BTree: 向上层插入分裂键
            end
        else 内部页面
            BTree->>Page: 二分查找定位子页面
            BTree->>Buffer: 获取子页面
        end
    end

    BTree->>Buffer: 标记修改的页面为脏
    BTree-->>Caller: 返回 0
```

### 4.4 BTree 索引扫描

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant BTree as BTree AM
    participant Buffer as Buffer Pool

    Caller->>BTree: bt_beginscan(rel, nkeys, key)
    BTree->>BTree: 初始化扫描描述符
    BTree-->>Caller: 返回 BTScanDesc

    loop 获取匹配元组
        Caller->>BTree: bt_getnext(scan, direction)

        alt 首次或方向改变
            BTree->>BTree: 从根节点搜索到叶子
            BTree->>BTree: 定位到第一个匹配键
        else 继续扫描
            BTree->>BTree: 移动到下一个条目
        end

        BTree->>Buffer: 获取当前页面
        Buffer-->>BTree: 页面数据

        alt 当前页面有更多条目
            BTree->>BTree: 检查键是否匹配扫描条件
            alt 匹配
                BTree-->>Caller: 返回堆元组指针
            else 不匹配
                BTree-->>Caller: 返回 NULL (扫描结束)
            end
        else 页面遍历完
            BTree->>BTree: 通过 btpo_next 跨到下一页
            BTree->>Buffer: 获取下一页
            Buffer-->>BTree: 页面数据
            BTree->>BTree: 继续扫描
        end
    end
```

---

## 五、访问方法接口统一

### 5.1 AM 接口表

```mermaid
flowchart LR
    subgraph "统一访问方法接口"
        CREATE[create<br/>创建对象]
        DESTROY[destroy<br/>销毁对象]
        INSERT[insert<br/>插入]
        DELETE[delete<br/>删除]
        UPDATE[update<br/>更新]
        BEGINSCAN[beginscan<br/>开始扫描]
        GETNEXT[getnext<br/>获取下一条]
        ENDSCAN[endscan<br/>结束扫描]
    end

    subgraph "Heap AM 实现"
        H_CREATE[heap_create]
        H_DESTROY[heap_destroy]
        H_INSERT[heap_insert]
        H_DELETE[heap_delete]
        H_UPDATE[heap_update]
        H_BEGIN[heap_beginscan]
        H_NEXT[heap_getnext]
        H_END[heap_endscan]
    end

    subgraph "BTree AM 实现"
        B_CREATE[btcreate]
        B_DESTROY[btdestroy]
        B_INSERT[btinsert]
        B_DELETE[btdelete]
        B_UPDATE[无<br/>BTree 不直接更新]
        B_BEGIN[bt_beginscan]
        B_NEXT[bt_getnext]
        B_END[bt_endscan]
    end

    CREATE --> H_CREATE
    CREATE --> B_CREATE
    DESTROY --> H_DESTROY
    DESTROY --> B_DESTROY
    INSERT --> H_INSERT
    INSERT --> B_INSERT
    DELETE --> H_DELETE
    DELETE --> B_DELETE
    UPDATE --> H_UPDATE
    BEGINSCAN --> H_BEGIN
    BEGINSCAN --> B_BEGIN
    GETNEXT --> H_NEXT
    GETNEXT --> B_NEXT
    ENDSCAN --> H_END
    ENDSCAN --> B_END
```

### 5.2 扫描方向枚举

```mermaid
flowchart LR
    subgraph "ScanDirection"
        FORWARD[ForwardScanDirection = 0<br/>正向扫描]
        BACKWARD[BackwardScanDirection = 1<br/>反向扫描]
    end
```

---

## 六、索引扫描

### 6.1 索引扫描流程

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant Index as 索引管理器
    participant BTree as BTree AM
    participant Heap as Heap AM
    participant Buffer as Buffer Pool

    Caller->>Index: index_beginscan(indexrel, nkeys, key)

    Index->>BTree: bt_beginscan(indexrel, nkeys, key)
    BTree-->>Index: 返回 BTScanDesc
    Index-->>Caller: 返回 IndexScanDesc

    loop 获取索引匹配的堆元组
        Caller->>Index: index_getnext(scan)

        Index->>BTree: bt_getnext(bt_scan, ForwardScanDirection)
        BTree-->>Index: 返回索引条目 (含 heap_ptr)

        Index->>Heap: 根据 heap_ptr 获取堆元组
        Heap->>Buffer: 获取堆页面
        Buffer-->>Heap: 页面数据
        Heap-->>Index: 返回堆元组

        Index->>Index: 检查可见性
        Index-->>Caller: 返回堆元组
    end
```

---

## 七、模块依赖与统计

### 7.1 依赖关系

```mermaid
flowchart TB
    REL[rel.c] --> BUF[Buffer Pool<br/>buf.h]
    REL --> CAT[Catalog<br/>catalog.h]

    HEAP[heapam.c] --> REL
    HEAP --> BUF

    BTREE[btreeam.c] --> REL
    BTREE --> BUF
```

### 7.2 统计信息

| 统计项 | Heap AM | BTree AM |
|--------|---------|----------|
| 插入次数 | `inserts` | `insertions` |
| 删除次数 | `deletes` | `deletions` |
| 更新次数 | `updates` | - |
| 元组读取数 | `tuples_read` | - |
| 索引扫描次数 | - | `index_scans` |
| 索引元组数 | - | `index_tuples` |
| 索引页面数 | - | `index_pages` |
| 死亡元组 | `dead_tuples` | - |
| HOT 更新次数 | `tuples_hot_updated` | - |

---

## 八、关键代码位置

| 功能 | 头文件 | 源文件 |
|------|--------|--------|
| Relation 抽象 | `engine/include/db/rel.h` | `engine/src/db/rel/rel.c` |
| Heap AM | `engine/include/db/heapam.h` | `engine/src/db/access_methods/heapam.c` |
| BTree AM | `engine/include/db/btreeam.h` | `engine/src/db/access_methods/btreeam.c` |
| 索引扫描 | `engine/include/db/rel.h` | `engine/src/db/rel/rel.c` |