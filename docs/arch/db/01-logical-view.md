> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# db 数据库存储引擎 - 逻辑视图

## 概述

本文档描述 db 数据库存储引擎的逻辑视图，展示系统的静态结构、核心组件及其关系。

---

## 一、系统分层架构

```mermaid
flowchart TB
    subgraph "应用层"
        API[API 接口层<br/>kv.h / table.h / vdb_api.h]
    end

    subgraph "SQL 层"
        SQL_PARSER[SQL 解析器<br/>parser/]
        SQL_PLANNER[查询计划器<br/>optimizer/]
        SQL_EXECUTOR[查询执行器<br/>executor/]
    end

    subgraph "访问方法层"
        REL[Relation 抽象<br/>rel.h]
        HEAP[堆表访问<br/>heapam.h]
        BTREE[BTree 索引<br/>btreeam.h]
    end

    subgraph "存储核心层"
        BUF[Buffer Pool<br/>buf.h]
        DISK[磁盘管理<br/>disk.h]
        WAL[写前日志<br/>wal.h]
        CATALOG[Catalog 系统<br/>catalog.h]
    end

    subgraph "索引系统"
        TRAD_IDX[传统索引<br/>btree/hash/gin/gist]
        VEC_IDX[向量索引<br/>hnsw/ivf/diskann 等]
    end

    subgraph "事务与并发"
        TXN[事务管理<br/>txn.h]
        LOCK[锁管理<br/>lock.h]
        MVCC[MVCC<br/>mvcc_hot.h]
    end

    subgraph "多模态存储"
        KV[KV 引擎<br/>kv_engine.h]
        VEC[向量引擎<br/>vector_engine.h]
        DOC[文档引擎<br/>doc_engine.h]
        TS[时序引擎<br/>ts_engine.h]
        SPATIAL[空间引擎<br/>spatial_engine.h]
        GRAPH[图引擎<br/>graph_engine.h]
        YANG[层次引擎<br/>yang_engine.h]
    end

    API --> SQL_EXECUTOR
    SQL_EXECUTOR --> SQL_PLANNER
    SQL_PLANNER --> SQL_PARSER
    SQL_EXECUTOR --> REL
    REL --> HEAP
    REL --> BTREE
    HEAP --> BUF
    BTREE --> BUF
    BUF --> DISK
    BUF --> WAL
    BUF --> CATALOG
    REL --> TRAD_IDX
    REL --> VEC_IDX
    TRAD_IDX --> BUF
    VEC_IDX --> BUF
    HEAP --> TXN
    BTREE --> LOCK
    TXN --> MVCC
    LOCK --> MVCC

    API --> KV
    API --> VEC
    API --> DOC
    API --> TS
    KV --> BUF
    VEC --> VEC_IDX
    DOC --> BUF
    TS --> BUF
```

---

## 二、核心组件类图

### 2.1 存储核心层

```mermaid
classDiagram
    class BufferPool {
        +buffer_t[] buffers
        +hash_table_t hash_table
        +int num_buffers
        +uint64_t *clock
        +buffer_t* get_page(page_id_t page_id)
        +void unpin_page(page_id_t page_id)
        +void flush_page(page_id_t page_id)
        +void flush_all()
    }

    class Buffer {
        +page_t* page
        +page_id_t page_id
        +int ref_count
        +bool is_dirty
        +uint64_t usage_count
        +pthread_mutex_t lock
    }

    class DiskManager {
        +int fd
        +char* db_file
        +uint64_t num_pages
        +page_t* read_page(page_id_t page_id)
        +void write_page(page_id_t page_id, page_t* page)
        +page_id_t allocate_page()
    }

    class Page {
        +page_header_t header
        +char data[PAGE_SIZE]
        +uint16_t get_free_space()
        +tuple_id_t insert_tuple(tuple_t* tuple)
        +tuple_t* get_tuple(tuple_id_t tid)
    }

    class WALManager {
        +int wal_fd
        +lsn_t current_lsn
        +wal_buf_t* wal_buf
        +lsn_t append_log_record(wal_record_t* record)
        +void flush(lsn_t lsn)
        +void recover()
    }

    BufferPool "1" --> "*" Buffer
    Buffer --> Page
    BufferPool --> DiskManager
    BufferPool --> WALManager
```

### 2.2 Catalog 系统

```mermaid
classDiagram
    class Catalog {
        +hash_table_t class_hash
        +hash_table_t attr_hash
        +hash_table_t index_hash
        +Oid next_oid
        +pg_class_t* get_class(Oid relid)
        +pg_attribute_t* get_attributes(Oid relid)
        +pg_index_t* get_index(Oid indexid)
        +Oid allocate_oid()
    }

    class pg_class {
        +Oid relid
        +char relname[NAMEDATALEN]
        +Oid relnamespace
        +char relkind
        +int relnatts
        +bool relisindexed
        +Oid relam
    }

    class pg_attribute {
        +Oid attrelid
        +char attname[NAMEDATALEN]
        +int attnum
        +Oid atttypid
        +int attlen
        +bool attnotnull
        +bool atthasdef
    }

    class pg_index {
        +Oid indexrelid
        +Oid indrelid
        +int indnatts
        +int* indkey
        +bool indisunique
        +bool indisprimary
    }

    Catalog "1" --> "*" pg_class
    Catalog "1" --> "*" pg_attribute
    Catalog "1" --> "*" pg_index
    pg_class --> pg_attribute : has
    pg_class --> pg_index : has
```

### 2.3 访问方法层

```mermaid
classDiagram
    class Relation {
        +Oid relid
        +char* relname
        +TupleDesc desc
        +RelFileNode node
        +int natts
        +bool is_index
        +void open()
        +void close()
    }

    class TupleDesc {
        +int natts
        +FormData_pg_attribute* attrs
        +int tupsize
        +TupleDesc copy()
    }

    class HeapTuple {
        +tuple_id_t tid
        +TupleDesc desc
        +Datum* values
        +bool* isnull
        +int natts
    }

    class IndexTuple {
        +Datum* key_values
        +bool* key_isnull
        +tuple_id_t heap_tid
        +int compare(IndexTuple* other)
    }

    class HeapAM {
        +Relation rel
        +HeapTuple* get_tuple(tuple_id_t tid)
        +tuple_id_t insert_tuple(HeapTuple* tuple)
        +void update_tuple(tuple_id_t tid, HeapTuple* tuple)
        +void delete_tuple(tuple_id_t tid)
        +HeapScan* begin_scan()
    }

    class BTreeAM {
        +Relation index_rel
        +bool insert(IndexTuple* tuple)
        +IndexScan* begin_scan(Datum* min_key, Datum* max_key)
        +IndexTuple* get_next(IndexScan* scan)
        +bool delete(IndexTuple* tuple)
    }

    Relation --> TupleDesc
    HeapAM --> Relation
    HeapAM --> HeapTuple
    BTreeAM --> Relation
    BTreeAM --> IndexTuple
    HeapTuple --> TupleDesc
```

### 2.4 事务与并发

```mermaid
classDiagram
    class TransactionManager {
        +hash_table_t txn_table
        +TransactionState current_txn
        +TransactionState begin_txn()
        +void commit_txn(TransactionState txn)
        +void abort_txn(TransactionState txn)
        +TransactionState get_current_txn()
    }

    class TransactionState {
        +TransactionId txnid
        +TransactionState parent
        +CommandId command_id
        +int subtrans_nesting
        +XidStatus status
        +lsn_t first_lsn
        +lsn_t commit_lsn
    }

    class LockManager {
        +hash_table_t lock_table
        +hash_table_t proclock_table
        +LOCKMETHOD lockmethod
        +bool acquire_lock(Oid relid, tuple_id_t tid, LockMode mode)
        +void release_lock(Oid relid, tuple_id_t tid, LockMode mode)
        +bool check_deadlock()
    }

    class MVCC {
        +TransactionId xmin
        +TransactionId xmax
        +bool is_visible(TransactionId xid, Snapshot snapshot)
        +void set_xmin(TransactionId xid)
        +void set_xmax(TransactionId xid)
    }

    TransactionManager "1" --> "*" TransactionState
    TransactionState --> MVCC
    TransactionManager --> LockManager
```

---

## 三、索引系统结构

```mermaid
classDiagram
    class IndexAPI {
        <<interface>>
        +create_index(CreateIndexStmt* stmt)
        +drop_index(DropStmt* stmt)
        +IndexScan* begin_scan(IndexScanDesc desc)
        +bool insert(Datum* values, tuple_id_t tid)
        +bool delete(tuple_id_t tid)
        +IndexBuildResult* build(Relation heap, IndexInfo* info)
    }

    class BTreeIndex {
        +Relation index_rel
        +BTreeAM* btree
        +所有 IndexAPI 方法
    }

    class HashIndex {
        +Relation index_rel
        +hash_table_t hash
        +所有 IndexAPI 方法
    }

    class GINIndex {
        +Relation index_rel
        +PostingListTree* posting_tree
        +所有 IndexAPI 方法
    }

    class GiSTIndex {
        +Relation index_rel
        +GiSTTree* gist_tree
        +所有 IndexAPI 方法
    }

    class VectorIndex {
        +Relation index_rel
        +void* index_impl
        +int n_dims
        +DistanceMetric metric
        +所有 IndexAPI 方法
        +vector_t* knn_search(vector_t* query, int k)
    }

    class HNSWIndex {
        +hnsw_t* hnsw
        +int ef_construction
        +int M
        +int ef_search
        +所有 VectorIndex 方法
    }

    class IVFIndex {
        +ivf_t* ivf
        +int n_clusters
        +int n_probe
        +所有 VectorIndex 方法
    }

    class DiskANNIndex {
        +diskann_t* diskann
        +char* index_path
        +size_t cache_size
        +所有 VectorIndex 方法
    }

    IndexAPI <|-- BTreeIndex
    IndexApi <|-- HashIndex
    IndexApi <|-- GINIndex
    IndexApi <|-- GiSTIndex
    IndexAPI <|-- VectorIndex
    VectorIndex <|-- HNSWIndex
    VectorIndex <|-- IVFIndex
    VectorIndex <|-- DiskANNIndex
```

---

## 四、多模态存储引擎

```mermaid
classDiagram
    class StorageEngine {
        <<interface>>
        +void* open(const char* path)
        +void close(void* handle)
        +int insert(void* handle, const void* key, const void* value)
        +int query(void* handle, const void* key, void** value)
        +int delete(void* handle, const void* key)
        +int scan(void* handle, void** results, int limit)
    }

    class KVEngine {
        +kv_t* kv
        +所有 StorageEngine 方法
        +int put(const char* key, const char* value)
        +int get(const char* key, char** value)
    }

    class VectorEngine {
        +vector_db_t* vdb
        +所有 StorageEngine 方法
        +int insert_vector(uint64_t id, float* vec, int dim)
        +int knn_search(float* query, int k, uint64_t* results)
        +int create_index(IndexConfig* config)
    }

    class DocEngine {
        +doc_db_t* doc_db
        +所有 StorageEngine 方法
        +int insert_doc(const char* id, const char* json)
        +int query_jsonpath(const char* path, char** result)
    }

    class TimeSeriesEngine {
        +ts_db_t* ts_db
        +所有 StorageEngine 方法
        +int insert_series(uint64_t metric_id, timestamp_t ts, double value)
        +int aggregate_series(uint64_t metric_id, timestamp_t start, timestamp_t end, AggFunc func)
    }

    class SpatialEngine {
        +spatial_db_t* spatial_db
        +所有 StorageEngine 方法
        +int insert_point(uint64_t id, double x, double y)
        +int query_bbox(double min_x, double min_y, double max_x, double max_y, uint64_t* results)
    }

    class GraphEngine {
        +graph_db_t* graph_db
        +所有 StorageEngine 方法
        +int insert_vertex(uint64_t id, const char* label, void* properties)
        +int insert_edge(uint64_t from, uint64_t to, const char* label)
        +int traverse(uint64_t start, int depth, TraverseResult** results)
    }

    class YangEngine {
        +yang_db_t* yang_db
        +所有 StorageEngine 方法
        +int insert_node(const char* path, const char* value)
        +int get_node(const char* path, char** value)
        +int traverse_path(const char* path, char** results)
    }

    StorageEngine <|-- KVEngine
    StorageEngine <|-- VectorEngine
    StorageEngine <|-- DocEngine
    StorageEngine <|-- TimeSeriesEngine
    StorageEngine <|-- SpatialEngine
    StorageEngine <|-- GraphEngine
    StorageEngine <|-- YangEngine

    VectorEngine --> VectorIndex : uses
```

---

## 五、核心数据结构

### 5.1 页面结构

```mermaid
classDiagram
    class PageHeader {
        +uint64_t pd_lsn
        +uint16_t pd_checksum
        +uint16_t pd_flags
        +uint16_t pd_lower
        +uint16_t pd_upper
        +uint16_t pd_special
        +uint16_t pd_pagesize_version
    }

    class ItemIdData {
        +uint16_t lp_off
        +uint16_t lp_flags
        +uint16_t lp_len
    }

    class HeapTupleHeader {
        +TransactionId t_xmin
        +TransactionId t_xmax
        +CommandId t_cid
        +uint16_t t_infomask
        +uint8_t t_hoff
        +uint16_t t_bits[FLEXIBLE_ARRAY]
    }

    class BTreePageHeader {
        +PageHeader base
        +uint32_t btpo_prev
        +uint32_t btpo_next
        +uint32_t btpo_level
        +uint16_t btpo_flags
    }

    PageHeader "1" --> "*" ItemIdData : linp[]
    ItemIdData --> HeapTupleHeader : points to
    BTreePageHeader --|> PageHeader
```

### 5.2 WAL 记录结构

```mermaid
classDiagram
    class WALRecord {
        +uint8_t xl_rmid
        +uint8_t xl_info
        +XLogRecPtr xl_prev
        +uint32_t xl_tot_len
        +uint32_t xl_xid
        +XLogRecPtr xl_crc
        +char* xl_data[FLEXIBLE]
    }

    class XLogRecordBlockHeader {
        +uint8_t forknum
        +uint8_t flags
        +uint16_t data_len
        +uint16_t data_offset
        +uint32_t blkref
    }

    class XLogRecordDataHeader {
        +uint8_t id
        +uint8_t datalen[FLEXIBLE]
    }

    WALRecord "1" --> "*" XLogRecordBlockHeader
    WALRecord "1" --> "*" XLogRecordDataHeader
```

---

## 六、模块依赖关系

```mermaid
flowchart LR
    subgraph "应用层"
        API[db API]
    end

    subgraph "SQL 层"
        PARSER[parser]
        PLANNER[optimizer]
        EXECUTOR[executor]
    end

    subgraph "访问方法层"
        REL[rel]
        HEAP[heapam]
        BTREE[btreeam]
    end

    subgraph "存储核心层"
        BUF[bufmgr]
        DISK[disk]
        PAGE[page]
        WAL[wal]
        CATALOG[catalog]
    end

    subgraph "事务层"
        TXN[txn]
        LOCK[lock]
        MVCC[mvcc]
    end

    subgraph "索引层"
        TRAD_IDX[传统索引]
        VEC_IDX[向量索引]
    end

    subgraph "工具层"
        GUC[guc]
        INITDB[initdb]
        PG_CTL[pg_ctl]
        SERVER[db_server]
    end

    API --> EXECUTOR
    EXECUTOR --> PLANNER
    PLANNER --> PARSER
    EXECUTOR --> REL
    REL --> HEAP
    REL --> BTREE
    HEAP --> BUF
    BTREE --> BUF
    BUF --> DISK
    BUF --> PAGE
    BUF --> WAL
    BUF --> CATALOG
    HEAP --> TXN
    BTREE --> TXN
    TXN --> LOCK
    TXN --> MVCC
    REL --> TRAD_IDX
    REL --> VEC_IDX
    TRAD_IDX --> BUF
    VEC_IDX --> BUF
    API --> GUC
    API --> INITDB
    API --> PG_CTL
    API --> SERVER
```

---

## 七、关键代码位置

| 组件 | 头文件 | 源文件 |
|------|--------|--------|
| Buffer Pool | `engine/include/db/buf.h` | `engine/src/db/storage/buffer/` |
| Disk Manager | `engine/include/db/disk.h` | `engine/src/db/storage/kv/` |
| Catalog | `engine/include/db/catalog.h` | `engine/src/db/storage/catalog/` |
| WAL | `engine/include/db/wal.h` | `engine/src/db/storage/wal/` |
| Heap AM | `engine/include/db/heapam.h` | `engine/src/db/executor/graph/sql/heapam.c` |
| BTree AM | `engine/include/db/btreeam.h` | `engine/src/db/index/btree/` |
| Relation | `engine/include/db/rel.h` | `engine/src/db/executor/graph/sql/rel.c` |
| Transaction | `engine/include/db/txn.h` | `engine/src/db/txn/` |
| Lock Manager | `engine/include/db/lock.h` | `engine/src/db/concurrency/` |
| GUC Config | `engine/include/db/guc.h` | `engine/src/db/bgworker/` |

---

## 八、设计决策

### 8.1 分层架构

**决策**: 采用 PostgreSQL 风格的分层架构
**原因**:
- 清晰的职责分离
- 便于独立测试和优化
- 支持多种访问方法共用存储层

### 8.2 Buffer Pool 实现

**决策**: 采用 Clock-Sweep 置换算法 + Hash 表查找
**原因**:
- PostgreSQL 成熟实践
- 平衡性能与实现复杂度
- 支持并发访问

### 8.3 多模态存储

**决策**: 统一 StorageEngine 接口，各引擎独立实现
**原因**:
- 支持不同数据模型的特定优化
- 统一 API 降低使用复杂度
- 便于扩展新的存储引擎