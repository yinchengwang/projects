> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# Catalog 子系统 - 架构设计

## 概述

Catalog 子系统是数据库的元数据管理中心，维护所有数据库对象（表、列、索引、类型、命名空间）的系统表。参考 PostgreSQL 的 pg_class、pg_attribute、pg_index 等系统表设计。

---

## 一、子系统架构概览

```mermaid
flowchart TB
    subgraph "Catalog 子系统"
        subgraph "系统表存储"
            PG_CLASS[pg_class<br/>表/索引/视图元数据]
            PG_ATTRIBUTE[pg_attribute<br/>列元数据]
            PG_INDEX[pg_index<br/>索引元数据]
            PG_DATABASE[pg_database<br/>数据库元数据]
            PG_NAMESPACE[pg_namespace<br/>命名空间]
            PG_TYPE[pg_type<br/>数据类型]
            PG_PROC[pg_proc<br/>函数/存储过程]
        end

        subgraph "核心功能"
            OID_MGR[OID 分配器<br/>OID 生成与回收]
            CACHE_MGR[缓存管理器<br/>内存缓存加速]
            CAT_CRUD[CRUD 操作<br/>创建/读取/更新/删除]
        end

        subgraph "引导系统"
            BOOTSTRAP[引导自举<br/>系统表自身创建]
            INIT_CATALOG[Catalog 初始化<br/>系统启动时]
        end
    end

    subgraph "上层调用者"
        DDL[DDL 执行器<br/>CREATE/ALTER/DROP]
        DML[DML 执行器<br/>INSERT/SELECT/UPDATE]
        OPTIMIZER[优化器<br/>统计信息查询]
    end

    DDL --> CAT_CRUD
    DML --> CAT_CRUD
    OPTIMIZER --> CACHE_MGR
    OPTIMIZER --> PG_CLASS
    CAT_CRUD --> PG_CLASS
    CAT_CRUD --> PG_ATTRIBUTE
    CAT_CRUD --> PG_INDEX
    CAT_CRUD --> OID_MGR
    CAT_CRUD --> CACHE_MGR
    BOOTSTRAP --> INIT_CATALOG
```

---

## 二、系统表结构

### 2.1 系统表关系图

```mermaid
erDiagram
    pg_class ||--o{ pg_attribute : "包含列"
    pg_class ||--o{ pg_index : "有索引"
    pg_index ||--|| pg_class : "索引于"
    pg_class }o--|| pg_namespace : "所属命名空间"
    pg_class }o--|| pg_type : "行类型"

    pg_class {
        Oid oid PK
        string name "表名"
        Oid namespace_oid FK
        Oid type_oid FK
        char relkind "r=表,i=索引,v=视图"
        int16 natts "列数"
        Oid filenode "物理文件节点"
        Oid tablespace "表空间"
        int32 npages "页面数估计"
        float ntupes "元组数估计"
        bool has_index "是否有索引"
        bool has_pkey "是否有主键"
    }

    pg_attribute {
        Oid table_oid FK
        string name "列名"
        Oid type_oid FK
        int16 attnum "列序号"
        int16 len "类型长度"
        int32 typmod "类型修饰符"
        bool attnotnull "NOT NULL"
        bool has_default "有默认值"
        bool is_dropped "已删除"
    }

    pg_index {
        Oid oid PK
        Oid table_oid FK
        string name "索引名"
        int16 nkeys "键列数"
        bool is_unique "唯一索引"
        bool is_primary "主键索引"
        bool is_valid "索引有效"
    }

    pg_namespace {
        Oid oid PK
        string name "命名空间名"
        Oid owner "所有者"
    }

    pg_type {
        Oid oid PK
        string name "类型名"
        int16 len "类型长度"
        char typtype "类型分类"
        Oid input_proc "输入函数"
        Oid output_proc "输出函数"
    }
```

### 2.2 系统表数据流

```mermaid
flowchart TB
    subgraph "创建表数据流"
        CREATE[CREATE TABLE] --> NEW_OID[OID 分配器<br/>分配表 OID]
        NEW_OID --> NEW_FILENODE[OID 分配器<br/>分配 filenode]
        NEW_FILENODE --> INSERT_CLASS[插入 pg_class 记录]
        INSERT_CLASS --> INSERT_ATTR[循环插入 pg_attribute 记录<br/>每列一条]
        INSERT_ATTR --> DONE[创建完成]
    end

    subgraph "创建索引数据流"
        CREATE_IDX[CREATE INDEX] --> IDX_OID[OID 分配器<br/>分配索引 OID]
        IDX_OID --> INSERT_INDEX[插入 pg_index 记录]
        INSERT_INDEX --> UPDATE_CLASS[更新 pg_class<br/>has_index = true]
        UPDATE_CLASS --> IDX_DONE[索引创建完成]
    end

    subgraph "查询元数据"
        QUERY[查询请求] --> LOOKUP[按名称或 OID 查找]
        LOOKUP --> CACHE{缓存命中?}
        CACHE -->|命中| RETURN[返回元数据]
        CACHE -->|未命中| SCAN_CLASS[扫描 pg_class]
        SCAN_CLASS --> SCAN_ATTR[扫描 pg_attribute]
        SCAN_ATTR --> UPDATE_CACHE[更新缓存]
        UPDATE_CACHE --> RETURN
    end
```

---

## 三、OID 分配

### 3.1 OID 分配器

```mermaid
classDiagram
    class OidManager {
        +Oid next_oid
        +Oid first_normal_oid
        +Oid max_oid
        +bool wrapped
        +pthread_mutex_t lock
        +Oid allocate_oid()
        +void set_next_oid(Oid oid)
        +Oid get_current_oid()
    }

    class OidRange {
        +Oid min
        +Oid max
        +const char* description
    }

    OidManager --> OidRange : 范围定义
```

### 3.2 OID 分配范围

```mermaid
flowchart LR
    subgraph "OID 空间"
        OID0[0<br/>InvalidOid]
        OID1[...<br/>系统 OID]
        OID16383[16383<br/>系统 OID 上限]
        OID16384[16384<br/>FirstNormalOid]
        OIDMAX[4294967295<br/>MaxOid]
    end

    OID0 --> OID1
    OID1 --> OID16383
    OID16383 --> OID16384
    OID16384 --> OIDMAX

    subgraph "系统 OID 分配示例"
        PG_CLASS_OID[pg_class<br/>OID=1259]
        PG_ATTR_OID[pg_attribute<br/>OID=1249]
        PG_INDEX_OID[pg_index<br/>OID=2610]
        PG_DB_OID[pg_database<br/>OID=1262]
        PG_TYPE_OID[pg_type<br/>OID=1247]
        PG_NS_OID[pg_namespace<br/>OID=2615]
    end

    OID1 --> PG_CLASS_OID
    OID1 --> PG_ATTR_OID
    OID1 --> PG_INDEX_OID
    OID1 --> PG_DB_OID
    OID1 --> PG_TYPE_OID
    OID1 --> PG_NS_OID
```

---

## 四、缓存管理

### 4.1 缓存架构

```mermaid
classDiagram
    class CatalogCache {
        +hashtable_t* oid_map
        +hashtable_t* name_map
        +hashtable_t* column_cache
        +hashtable_t* index_cache
        +uint64_t hits
        +uint64_t misses
        +pthread_rwlock_t lock
        +table_info_t* lookup_by_oid(Oid)
        +table_info_t* lookup_by_name(const char*)
        +column_info_t* get_columns(Oid)
        +index_info_t* get_indexes(Oid)
        +void invalidate(Oid)
        +void invalidate_all()
    }

    class CacheEntry {
        +Oid oid
        +char name[64]
        +void* data
        +bool is_valid
        +time_t cached_at
        +uint32_t refcount
    }

    CatalogCache "*" --> "*" CacheEntry
```

### 4.2 缓存生命周期

```mermaid
stateDiagram-v2
    [*] --> Empty: 系统启动

    Empty --> Populating: 首次访问
    Populating --> Valid: 数据加载完成

    Valid --> Stale: DDL 操作 (DROP/ALTER)
    Stale --> Invalidated: 标记失效

    Invalidated --> Populating: 下次访问时重载
    Invalidated --> Empty: 全部失效

    Valid --> Empty: catalog_invalidate_all()

    note right of Valid: 缓存命中<br/>hits++
    note right of Stale: 但数据仍在<br/>下次访问重载
```

### 4.3 缓存失效触发

```mermaid
flowchart TD
    Start[DDL 操作] --> CheckType{操作类型}

    CheckType -->|DROP TABLE| InvalidateTable[失效表 + 列 + 索引缓存]
    CheckType -->|ALTER TABLE| InvalidateColumn[失效列缓存]
    CheckType -->|CREATE INDEX| InvalidateIndex[失效索引缓存]
    CheckType -->|DROP INDEX| InvalidateIndex
    CheckType -->|CREATE TABLE| NoOp[无需失效]

    InvalidateTable --> Done
    InvalidateColumn --> Done
    InvalidateIndex --> Done
    NoOp --> Done
    Done --> Return[操作完成]
```

---

## 五、Catalog 操作流程

### 5.1 创建表流程

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant Catalog as Catalog 系统
    participant OID as OID 分配器
    participant Cache as 缓存
    participant Disk as 磁盘

    Caller->>Catalog: catalog_create_table(name, columns, ncolumns)

    Catalog->>Catalog: 检查表名唯一性
    Catalog->>OID: allocate_oid()
    OID-->>Catalog: 返回表 OID

    Catalog->>OID: allocate_oid()
    OID-->>Catalog: 返回 filenode

    Catalog->>Disk: 创建数据文件
    Disk-->>Catalog: 文件创建成功

    Catalog->>Cache: 构造 table_info_t
    Catalog->>Cache: 写入 pg_class 缓存

    loop 每列
        Catalog->>Cache: 构造 column_info_t
        Catalog->>Cache: 写入 pg_attribute 缓存
    end

    Catalog->>Disk: 持久化系统表记录
    Disk-->>Catalog: 写入成功

    Catalog-->>Caller: 返回表 OID
```

### 5.2 查找表流程

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant Catalog as Catalog 系统
    participant Cache as 缓存

    alt 按名称查找
        Caller->>Catalog: catalog_lookup_table(name)
        Catalog->>Cache: name_map 查找

        alt 缓存命中
            Cache-->>Catalog: 返回 OID
        else 缓存未命中
            Catalog->>Catalog: 扫描 pg_class 系统表
            Catalog->>Cache: 更新缓存
            Cache-->>Catalog: 返回 OID
        end

        Catalog-->>Caller: 返回 OID
    else 按 OID 获取信息
        Caller->>Catalog: catalog_get_table(oid)
        Catalog->>Cache: oid_map 查找

        alt 缓存命中
            Cache-->>Catalog: 返回 table_info_t*
        else 缓存未命中
            Catalog->>Catalog: 从 pg_class 读取
            Catalog->>Cache: 更新缓存
            Cache-->>Catalog: 返回 table_info_t*
        end

        Catalog-->>Caller: 返回 table_info_t*
    end
```

### 5.3 获取列信息流程

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant Catalog as Catalog 系统
    participant Cache as 缓存

    Caller->>Catalog: catalog_get_columns(table_oid, ncolumns)

    Catalog->>Cache: 查找列缓存
    alt 缓存命中
        Cache-->>Catalog: 返回 column_info_t*
        Catalog-->>Caller: 返回列数组
    else 缓存未命中
        Catalog->>Catalog: 扫描 pg_attribute<br/>WHERE table_oid = ?
        Catalog->>Catalog: 按 attnum 排序
        Catalog->>Cache: 写入缓存
        Catalog-->>Caller: 返回 column_info_t*
    end
```

---

## 六、Catalog 结果码

```mermaid
flowchart LR
    subgraph "Catalog 结果码"
        SUCCESS[0 成功<br/>CATALOG_SUCCESS]
        NOT_FOUND[1 不存在<br/>CATALOG_NOT_FOUND]
        DUPLICATE[2 重复<br/>CATALOG_DUPLICATE]
        IN_USE[3 使用中<br/>CATALOG_IN_USE]
        ERROR[-1 一般错误<br/>CATALOG_ERROR]
    end
```

---

## 七、性能指标

| 指标 | 目标值 | 说明 |
|------|--------|------|
| 表查找 (按 OID) | O(1) | Hash 缓存 |
| 表查找 (按名称) | O(1) | Hash 缓存 |
| 列信息获取 | O(1) | 缓存命中 |
| 索引信息获取 | O(1) | 缓存命中 |
| 缓存命中率 | > 99% | 启动后稳定 |
| OID 分配 | O(1) | 原子递增 |
| 系统表插入 | < 1ms | 顺序追加 |
| 缓存失效 | O(1) | 标记删除 |

---

## 八、关键代码位置

| 功能 | 头文件 | 源文件 |
|------|--------|--------|
| Catalog 公共接口 | `engine/include/db/catalog.h` | `engine/src/db/storage/catalog/catalog.c` |
| Catalog 内部实现 | `engine/include/db/storage/catalog/catalog.h` | `engine/src/db/storage/catalog/catalog.c` |
| 缓存管理 | `engine/include/db/catalog.h` | `engine/src/db/storage/catalog/catalog.c` |
| OID 分配 | `engine/include/db/catalog.h` | `engine/src/db/storage/catalog/catalog.c` |