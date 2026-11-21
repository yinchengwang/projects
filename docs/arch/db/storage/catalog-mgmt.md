> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# Catalog 管理

## 概述

本文档描述 Catalog 系统表的管理，包括系统表结构、CRUD 操作、缓存管理和 OID 分配。

---

## 一、系统表概览

### 1.1 核心系统表

```mermaid
flowchart TB
    subgraph "Catalog 系统表"
        PG_CLASS[pg_class<br/>表信息<br/>table_info_t]
        PG_ATTRIBUTE[pg_attribute<br/>列信息<br/>column_info_t]
        PG_INDEX[pg_index<br/>索引信息<br/>index_info_t]
        PG_DATABASE[pg_database<br/>数据库信息]
        PG_NAMESPACE[pg_namespace<br/>命名空间]
        PG_TYPE[pg_type<br/>数据类型]
        PG_PROC[pg_proc<br/>函数信息]
    end

    PG_CLASS -->|包含列| PG_ATTRIBUTE
    PG_CLASS -->|有索引| PG_INDEX
    PG_INDEX -->|索引关联| PG_CLASS
    PG_CLASS -->|所属| PG_NAMESPACE
    PG_CLASS -->|行类型| PG_TYPE
```

### 1.2 系统表关系

```mermaid
classDiagram
    class table_info_t {
        +Oid oid
        +char name[64]
        +Oid namespace_oid
        +char relkind
        +int16 natts
        +Oid filenode
        +int32 npages
        +float ntupes
        +bool has_index
        +bool has_pkey
    }

    class column_info_t {
        +Oid table_oid
        +char name[64]
        +Oid type_oid
        +int16 attnum
        +int16 len
        +int32 typmod
        +bool attnotnull
        +bool has_default
        +bool is_dropped
    }

    class index_info_t {
        +Oid oid
        +Oid table_oid
        +char name[64]
        +int16 nkeys
        +int16* key_nums
        +bool is_unique
        +bool is_primary
        +bool is_valid
    }

    table_info_t "1" --> "*" column_info_t : 包含
    table_info_t "1" --> "*" index_info_t : 有索引
    index_info_t --> "1" table_info_t : 索引于
```

---

## 二、系统表存储

### 2.1 系统表物理存储

```mermaid
flowchart TB
    subgraph "物理存储"
        DATA_DIR[数据目录<br/>/data/mydb/]
        GLOBAL[global/<br/>全局系统表]
        BASE[base/<br/>每个数据库一份]
    end

    subgraph "global/ 目录"
        PG_CLASS_FILE[pg_class_filenode<br/>系统表数据文件]
        PG_DATABASE_FILE[pg_database_filenode<br/>数据库列表]
    end

    subgraph "base/ 目录"
        DB1[db_oid1/<br/>数据库1]
        DB2[db_oid2/<br/>数据库2]
    end

    subgraph "数据库目录"
        PG_CLASS_DB[pg_class<br/>用户表+系统表元数据]
        PG_ATTRIBUTE_DB[pg_attribute<br/>列信息]
        PG_INDEX_DB[pg_index<br/>索引信息]
        PG_TYPE_DB[pg_type<br/>类型信息]
    end

    DB1 --> PG_CLASS_DB
    DB1 --> PG_ATTRIBUTE_DB
    DB1 --> PG_INDEX_DB
    DB1 --> PG_TYPE_DB
```

### 2.2 引导初始化

```mermaid
sequenceDiagram
    participant InitDB as initdb
    participant Catalog as Catalog 系统
    participant Disk as 磁盘
    participant WAL as WAL

    InitDB->>Catalog: catalog_init()

    Catalog->>Disk: 创建 pg_class 文件
    Disk-->>Catalog: 文件创建成功

    Catalog->>Disk: 创建 pg_attribute 文件
    Disk-->>Catalog: 文件创建成功

    Catalog->>Disk: 创建 pg_index 文件
    Disk-->>Catalog: 文件创建成功

    Catalog->>Catalog: 引导自举
    Note over Catalog: 将 pg_class、pg_attribute、pg_index<br/>自身信息写入系统表

    Catalog->>Catalog: 分配 OID 范围
    Catalog->>WAL: 记录系统表创建日志
    Catalog->>Disk: 刷盘

    Catalog-->>InitDB: Catalog 初始化完成
```

---

## 三、OID 分配

### 3.1 OID 分配策略

```mermaid
flowchart TD
    Start[分配 OID] --> Lock[获取 OID 锁]

    Lock --> Read[读取当前 OID 计数器]
    Read --> Increment[next_oid = current_oid + 1]
    Increment --> Check{next_oid<br/>超出范围?}

    Check -->|否| Update[更新计数器]
    Check -->|是| Wrap[回绕到<br/>FirstNormalOid]

    Wrap --> Update
    Update --> Unlock[释放 OID 锁]
    Unlock --> Return[返回 next_oid]
```

### 3.2 OID 命名空间

```mermaid
flowchart LR
    subgraph "OID 范围"
        FIRST_NORMAL[FirstNormalOid<br/>16384]
        LAST_NORMAL[MaxOid<br/>4294967295]
        SYSTEM_OID[系统 OID<br/>1 - 16383]
    end

    subgraph "系统 OID 分配"
        PG_CLASS_OID[pg_class<br/>OID = 1259]
        PG_ATTRIBUTE_OID[pg_attribute<br/>OID = 1249]
        PG_INDEX_OID[pg_index<br/>OID = 2610]
        PG_DATABASE_OID[pg_database<br/>OID = 1262]
        PG_TYPE_OID[pg_type<br/>OID = 1247]
    end

    SYSTEM_OID --> PG_CLASS_OID
    SYSTEM_OID --> PG_ATTRIBUTE_OID
    SYSTEM_OID --> PG_INDEX_OID
    SYSTEM_OID --> PG_DATABASE_OID
    SYSTEM_OID --> PG_TYPE_OID
```

---

## 四、CRUD 操作流程

### 4.1 创建表

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant Catalog as Catalog 系统
    participant SQL as SQL 执行器
    participant Disk as 磁盘

    Caller->>Catalog: catalog_create_table(name, columns, ncolumns)

    Catalog->>Catalog: 检查表名是否重复

    alt 表名已存在
        Catalog-->>Caller: 返回 InvalidOid
    else 表名不存在
        Catalog->>Catalog: 分配 OID
        Catalog->>Catalog: 分配 filenode

        Catalog->>SQL: 创建表的数据文件
        SQL->>Disk: 创建文件
        Disk-->>SQL: 创建成功

        Catalog->>Catalog: 写入 pg_class 记录
        Catalog->>Catalog: 写入 pg_attribute 记录 (每列一条)

        Catalog->>Catalog: 添加到缓存
        Catalog-->>Caller: 返回表 OID
    end
```

### 4.2 查找表

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant Catalog as Catalog 系统
    participant Cache as 缓存

    Caller->>Catalog: catalog_lookup_table(name)

    Catalog->>Cache: 按名称查找
    alt 缓存命中
        Cache-->>Catalog: 返回 OID
        Catalog-->>Caller: 返回 OID
    else 缓存未命中
        Catalog->>Catalog: 扫描 pg_class 系统表
        Catalog->>Cache: 写入缓存
        Catalog-->>Caller: 返回 OID
    end
```

### 4.3 获取列信息

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant Catalog as Catalog 系统
    participant Cache as 缓存

    Caller->>Catalog: catalog_get_columns(table_oid, ncolumns)

    Catalog->>Cache: 查找列缓存
    alt 缓存命中
        Cache-->>Catalog: 返回列数组
    else 缓存未命中
        Catalog->>Catalog: 扫描 pg_attribute<br/>WHERE table_oid = ?
        Catalog->>Catalog: 按 attnum 排序
        Catalog->>Cache: 写入缓存
    end

    Catalog-->>Caller: 返回 column_info_t* 数组
```

### 4.4 删除表

```mermaid
flowchart TD
    Start[catalog_drop_table] --> CheckExists{表存在?}

    CheckExists -->|否| NotFound[返回 CATALOG_NOT_FOUND]
    CheckExists -->|是| CheckIndex{有索引?}

    CheckIndex -->|是| DropIndexes[删除所有索引]
    DropIndexes --> DeleteFile[删除数据文件]
    CheckIndex -->|否| DeleteFile

    DeleteFile --> DeletePgClass[从 pg_class 删除记录]
    DeletePgClass --> DeletePgAttr[从 pg_attribute 删除记录]
    DeletePgAttr --> DeletePgIndex[从 pg_index 删除记录]
    DeletePgIndex --> InvalidateCache[使缓存失效]
    InvalidateCache --> Success[返回 CATALOG_SUCCESS]
```

---

## 五、缓存管理

### 5.1 缓存结构

```mermaid
classDiagram
    class CatalogCache {
        +table_info_t* table_cache
        +column_info_t* column_cache
        +index_info_t* index_cache
        +hashtable_t* oid_map
        +hashtable_t* name_map
        +uint64_t hits
        +uint64_t misses
        +pthread_rwlock_t lock
    }

    class CacheEntry {
        +Oid oid
        +char name[64]
        +void* data
        +bool is_valid
        +time_t cached_at
    }

    CatalogCache "1" --> "*" CacheEntry : 缓存条目
```

### 5.2 缓存失效策略

```mermaid
sequenceDiagram
    participant Caller as 调用者
    participant Catalog as Catalog 系统
    participant Cache as 缓存

    Note over Caller,Cache: 主动失效

    Caller->>Catalog: catalog_invalidate_table(table_oid)
    Catalog->>Cache: 删除表缓存
    Catalog->>Cache: 删除列缓存
    Catalog->>Cache: 删除索引缓存
    Catalog-->>Caller: 失效完成

    Note over Caller,Cache: 全局失效

    Caller->>Catalog: catalog_invalidate_all()
    Catalog->>Cache: 清空所有缓存
    Catalog-->>Caller: 全部失效完成
```

---

## 六、结果码

```mermaid
flowchart LR
    subgraph "Catalog 结果码"
        SUCCESS[CATALOG_SUCCESS = 0<br/>操作成功]
        NOT_FOUND[CATALOG_NOT_FOUND = 1<br/>对象不存在]
        DUPLICATE[CATALOG_DUPLICATE = 2<br/>对象已存在]
        IN_USE[CATALOG_IN_USE = 3<br/>对象被使用中]
        ERROR[CATALOG_ERROR = -1<br/>一般错误]
    end
```

---

## 七、性能指标

| 指标 | 目标值 | 说明 |
|------|--------|------|
| 表查找 (按 OID) | O(1) | Hash 缓存 |
| 表查找 (按名称) | O(1) | Hash 缓存 |
| 列信息获取 | O(1) | 缓存命中 |
| 缓存命中率 | > 99% | 启动后稳定 |
| OID 分配 | O(1) | 原子递增 |
| 系统表插入 | < 1ms | 顺序追加 |

---

## 八、关键代码位置

| 功能 | 头文件 | 源文件 |
|------|--------|--------|
| Catalog 公共接口 | `engine/include/db/catalog.h` | `engine/src/db/storage/catalog/catalog.c` |
| 系统表定义 | `engine/include/db/catalog.h` | `engine/src/db/storage/catalog/catalog.c` |
| 缓存管理 | `engine/include/db/catalog.h` | `engine/src/db/storage/catalog/catalog.c` |
| OID 分配 | `engine/include/db/catalog.h` | `engine/src/db/storage/catalog/catalog.c` |