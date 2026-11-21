> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# PostgreSQL 风格存储架构

## 概述

本项目实现了一个参考 PostgreSQL 架构的存储引擎，包含以下核心组件：

```
┌─────────────────────────────────────────────────────────────┐
│                    SQL 执行层                                │
│               (sql_executor.c/h)                           │
├─────────────────────────────────────────────────────────────┤
│                    Access Method 层                          │
│  ┌─────────────────┐         ┌─────────────────┐          │
│  │    Heap AM      │         │   BTree AM      │          │
│  │  (heapam.c/h)  │         │ (btreeam.c/h)   │          │
│  └────────┬────────┘         └────────┬────────┘          │
├───────────┼───────────────────────────┼────────────────────┤
│           │      Buffer Pool         │                      │
│  ┌────────▼────────┐         ┌───────▼─────────┐          │
│  │  bufmgr.c/h    │         │   Hash 表       │          │
│  │ Clock-Sweep    │         │   快速查找      │          │
│  └────────────────┘         └────────────────┘          │
├─────────────────────────────────────────────────────────────┤
│                    Catalog 系统                             │
│  ┌─────────────────────────────────────────────────┐     │
│  │ pg_class | pg_attribute | pg_index              │     │
│  │ OID 管理 | 表/列/索引元数据 | 哈希缓存          │     │
│  └─────────────────────────────────────────────────┘     │
├─────────────────────────────────────────────────────────────┤
│                    WAL 日志系统                            │
│  ┌─────────────────────────────────────────────────┐     │
│  │ wal.c/h | wal_buf.c/h                           │     │
│  │ 写前日志 | Buffer 协调                           │     │
│  └─────────────────────────────────────────────────┘     │
└─────────────────────────────────────────────────────────────┘
```

## 核心模块

### 1. Catalog 系统 (`db/catalog.h/c`)

负责存储数据库的元数据：

- **Oid 管理**：对象标识符分配
- **pg_class**：表、索引等 Relation 信息
- **pg_attribute**：列定义
- **pg_index**：索引定义
- **哈希缓存**：快速元数据查询

```c
// 创建表
column_def_t cols[2];
strcpy(cols[0].name, "id");
cols[0].type_oid = 23;  // int4
Oid table_oid = catalog_create_table("users", cols, 2);

// 查询表
table_info_t *info = catalog_get_table(table_oid);

// 获取列
int ncols;
column_info_t *cols = catalog_get_columns(table_oid, &ncols);
```

### 2. Buffer Pool (`db/buf.h`, `src/db/storage/bufmgr.c`)

内存缓存层，减少磁盘 I/O：

- **Clock-Sweep 置换算法**：高效的页面淘汰
- **Hash 表**：O(1) 页面查找
- **脏页管理**：标记和刷盘
- **Pin/Unpin**：页面引用计数

```c
// 初始化
buf_init(1024);  // 1024 个 buffer

// 读取页面
BufferDesc *buf = buf_read(relfilenode, blocknum, access_mode);
if (buf) {
    char *data = buf_get_data(buf);
    // 使用数据...
    buf_unpin(buf);
}

// 标记脏页
buf_dirty(buf);

// 刷所有脏页
buf_flush_all();
```

### 3. Access Method 层 (`db/rel.h`, `src/db/storage/rel.c`)

Relation 的抽象接口：

- **Relation**：表/索引的统一访问接口
- **TupleDesc**：行描述符
- **ScanKey**：扫描条件
- **扫描接口**：顺序/索引扫描

```c
// 打开表
Relation rel = relation_open(table_oid, REL_OPEN_READWRITE);

// 获取元数据
TupleDesc tdesc = relation_getdesc(rel);
int natts = relation_getnatts(rel);

// 开始扫描
TableScanDesc scan = table_beginscan_all(rel);
while (table_getnext(scan) != NULL) {
    // 处理元组...
}
table_endscan(scan);

relation_close(rel, 0);
```

### 4. Heap AM (`db/heapam.h`, `src/db/storage/heapam.c`)

堆表存储实现：

- **Page 结构**：页面头 + LinePointer + 元组数据
- **元组操作**：插入/更新/删除
- **页面操作**：初始化、空间计算、清理

```c
// 初始化页面
char page[HEAP_PAGE_SIZE];
heap_page_init(page, HEAP_PAGE_SIZE);

// 添加元组
HeapLinePointer lp;
heap_page_add_tuple(page, tuple_data, len, &lp);

// 扫描
void *tuple = heap_getnext(scan, ForwardScanDirection);
```

### 5. BTree AM (`db/btreeam.h`, `src/db/storage/btreeam.c`)

BTree 索引实现：

- **BTree 页面**：内部页 + 叶子页
- **键比较**：支持多种数据类型
- **扫描接口**：范围查询

```c
// 创建索引
btcreate(rel);

// 插入
btinsert(rel, &key, 1, heap_ptr);

// 扫描
BTScanDesc scan = bt_beginscan(rel, 1, &key);
while (bt_getnext(scan, ForwardScanDirection) != NULL) {
    // 处理...
}
bt_endscan(scan);
```

### 6. WAL 与 Buffer 协调 (`db/wal_buf.h`, `src/db/storage/wal_buf.c`)

保证数据一致性的关键组件：

- **脏页追踪**：记录修改过的页面
- **LSN 追踪**：日志序列号
- **检查点**：定期刷脏页
- **崩溃恢复**：基于 WAL 重做/撤销

```c
// 创建协调器
wal_buf_t *wb = wal_buf_create(wal, buffer_pool);

// 标记脏页并记录到 WAL
wal_buf_mark_dirty(wb, buf, txn_id);

// 事务提交（确保日志刷盘）
wal_buf_commit(wb, txn_id);

// 检查点
wal_buf_checkpoint(wb);
```

### 7. SQL 执行器 (`db/sql/sql_executor.h/c`)

整合所有存储组件的执行器：

```c
// 创建执行器
sql_executor_t *exec = sql_executor_create();
sql_executor_init(exec, "mydb", 1024);

// 事务管理
sql_executor_begin(exec);

// DDL
Oid table_oid = sql_executor_create_table(exec, "users", cols, 2);

// DML
sql_executor_insert(exec, "users", values, 3);

// 提交
sql_executor_commit(exec);

sql_executor_destroy(exec);
```

### 8. GUC 配置系统 (`db/guc.h`, `db/core/guc.c`)

PostgreSQL 风格的统一配置参数系统：

- **参数类型**：整数、浮点数、字符串、布尔、枚举
- **配置文件**：`postgresql.conf` 格式解析
- **单位转换**：支持 `kB`, `MB`, `GB`, `ms`, `s`, `min` 等
- **作用域**：支持 `postmaster`, `session`, `user` 级参数

```c
// 初始化
guc_init(data_dir);

// 设置参数
guc_set("work_mem", "64MB");

// 获取参数
const char *val = guc_get("shared_buffers");

// 加载配置文件
guc_load_file("postgresql.conf");

// 关闭
guc_shutdown();
```

核心参数：

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| max_connections | int | 100 | 最大连接数 |
| shared_buffers | int | 128MB | 共享缓冲区大小 |
| work_mem | int | 4MB | 工作内存 |
| wal_level | enum | replica | WAL 级别 |
| log_level | string | info | 日志级别 |

### 9. initdb 工具 (`db/initdb.h`, `db/core/initdb.c`)

数据库集群初始化工具：

```bash
./initdb -D /data/db
```

功能：
- 创建目录结构（`base/`, `global/`, `pg_wal/` 等）
- 初始化系统表（`pg_database`, `pg_class`, `pg_attribute`）
- 生成 `postgresql.conf` 配置文件
- 生成 `pg_hba.conf` 访问控制配置
- 初始化 WAL 日志文件

### 10. pg_ctl 工具 (`db/pg_ctl.h`, `db/core/pg_ctl.c`)

服务器进程控制工具：

```bash
./pg_ctl start -D /data/db      # 启动服务器
./pg_ctl stop -D /data/db       # 停止服务器
./pg_ctl restart -D /data/db    # 重启服务器
./pg_ctl status -D /data/db     # 查看状态
./pg_ctl reload -D /data/db     # 重新加载配置
./pg_ctl promote -D /data/db    # 提升备库为主库
```

### 11. 数据库服务器 (`db/db_server.h`, `db/core/db_server.c`)

简化版 PostgreSQL Wire 协议服务器：

- **监听端口**：默认 5432
- **协议处理**：StartupMessage、Simple Query
- **连接管理**：多线程处理并发连接
- **错误响应**：标准 PostgreSQL 错误格式

```c
// 启动服务器
server_start(5432, "/data/db");

// 执行 SQL（内部调用）
server_execute_sql("SELECT * FROM users", output, len);

// 停止服务器
server_stop();
```

### 12. 完整架构图

```
┌─────────────────────────────────────────────────────────────┐
│                      客户端连接层                             │
│         pg_ctl | psql | libpq | 应用驱动                   │
└─────────────────────┬───────────────────────────────────────┘
                      │ PostgreSQL Wire 协议
┌─────────────────────▼───────────────────────────────────────┐
│                    数据库服务器 (db_server)                    │
│              Socket 监听 | 连接管理 | SQL 转发                │
└─────────────────────┬───────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────┐
│                   SQL 执行器层 (sql_executor)                 │
│            解析 | 优化 | 执行 | 事务管理                      │
└──────┬──────────────┬────────────────────────────────────────┘
       │              │
┌──────▼──────┐ ┌─────▼──────────────────────────────────┐
│  DDL 执行  │ │  DML 执行                              │
│ 创建/删除  │ │  SELECT/UPDATE/DELETE                 │
└─────────────┘ └────────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────┐
│                    Access Method 层                          │
│         Heap AM | BTree AM | 其他访问方法                    │
└──────┬──────────────┬────────────────────────────────────────┘
       │              │
┌──────▼──────┐ ┌─────▼──────────────────────────────────┐
│  Buffer Pool │ │  Catalog                               │
│  页面缓存    │ │  元数据管理                           │
└──────┬──────┘ └─────┬────────────────────────────────────┘
       │              │
┌──────▼──────────────▼─────────────────────────────────────┐
│                      WAL 层                                 │
│              写前日志 | 检查点 | 崩溃恢复                   │
└─────────────────────┬───────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────┐
│                    磁盘文件层                                │
│              页面文件 | WAL 文件 | 系统表                    │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                    配置系统 (GUC)                            │
│   postgresql.conf ──→ guc ──→ shared_buffers, work_mem 等  │
└─────────────────────────────────────────────────────────────┘
```

## 测试

所有模块都有完整的单元测试：

| 模块 | 测试文件 | 测试数 |
|------|----------|--------|
| Catalog | test_catalog | 15 |
| Buffer Pool | test_buffer_pool | 19 |
| Relation/AM | test_relation | 28 |
| Heap AM | test_heapam | 16 |
| BTree AM | test_btreeam | 25 |
| 集成测试 | storage_integration_test | 18+ |

运行测试：

```bash
# 运行单个模块测试
./build/test/db/storage/test_catalog.exe
./build/test/db/storage/test_buffer_pool.exe

# 运行所有存储测试
cmake --build build --target test_storage_all
```

## 文件列表

```
include/db/
├── catalog.h          # Catalog 公共接口
├── buf.h             # Buffer Pool 公共接口
├── rel.h              # Relation/AM 公共接口
├── heapam.h           # Heap AM 接口
├── btreeam.h          # BTree AM 接口
├── wal.h               # WAL 接口
├── wal_buf.h          # WAL-Buffer 协调接口
└── sql/
    └── sql_executor.h  # SQL 执行器接口

src/db/storage/
├── catalog.c          # Catalog 实现
├── bufmgr.c            # Buffer Pool 实现
├── rel.c               # Relation 实现
├── heapam.c            # Heap AM 实现
├── btreeam.c           # BTree AM 实现
└── wal_buf.c           # WAL-Buffer 协调实现

src/db/sql/
└── sql_executor.c       # SQL 执行器实现
```

## 参考资料

- PostgreSQL 源码：`src/backend/storage/buffer/`
- PostgreSQL 源码：`src/include/storage/buf_internals.h`
- 《Database System Concepts》- 缓存管理章节

---

## 多模态存储引擎

除关系模型外，项目还实现了多种数据模型的专用存储引擎，支持以下高级存储优化特性：

| 特性 | 模块 | 描述 |
|------|------|------|
| VecPage 分页存储 | 向量引擎 | 内存页管理 + Clock-Sweep 置换 |
| PQ 量化压缩 | 向量引擎 | 乘积量化，向量压缩存储 |
| CSR 图存储 | 图引擎 | 压缩稀疏行格式，O(1) 出入边查询 |
| Hilbert 索引 | 空间引擎 | Hilbert 曲线辅助索引 |
| 分段索引 | 时序引擎 | 时间分段存储和二分查找 |
| 倒排索引 | 文档引擎 | 全文搜索索引 |

### 1. 向量引擎 (`db/storage/vector/`)

支持向量数据的存储和相似度搜索：

- **VecPage 分页存储**：高效的内存页管理和 Clock-Sweep 置换
- **PQ 量化压缩**：乘积量化，支持向量压缩存储和 ADIS 距离计算
- **HNSW 索引集成**：高效近似最近邻搜索
- **多种度量**：L2 距离、余弦相似度、内积

```c
// 创建向量引擎
void *db = vector_engine_open("my_vectors", ACCESS_MODE_READ_WRITE);

// 启用 VecPage 分页存储（默认启用）
// 启用 PQ 量化压缩
vector_engine_enable_pq(db, 16, 8);  // 16 个子空间，8 位

// 训练 PQ 量化器
vector_engine_train_pq(db, 1000);

// 插入向量
float vec[128] = { /* ... */ };
vector_engine_insert(db, vec, sizeof(vec));

// 构建 HNSW 索引
vector_engine_build_index(db, 16, 200);

// HNSW 搜索
float query[128] = { /* ... */ };
vector_search_results_t results;
vector_engine_search_hnsw(db, query, 10, &results);

// 保存 PQ 量化器和索引
vector_engine_save_pq(db);
vector_engine_save_index(db);

vector_engine_close(db);
```

### 2. 空间引擎 (`db/storage/spatial/`)

支持空间数据的存储和查询：

- **R-Tree 索引**：高效范围查询和最近邻
- **Hilbert 曲线索引**：空间填充曲线辅助索引，优化局部性和范围查询
- **持久化存储**：R-Tree 文件格式（4KB 页面 + 64 字节头）
- **几何类型**：点、线、多边形、边界框
- **查询支持**：边界框查询、KNN 最近邻

```c
// 创建空间引擎
void *db = spatial_engine_open("geo_data", ACCESS_MODE_READ_WRITE);

// 插入几何对象
bbox_t bbox = bbox_create(0, 0, 10, 10);
spatial_engine_insert(db, &bbox, sizeof(bbox), 1);

// 构建 R-Tree 和 Hilbert 索引
spatial_engine_build_index(db, 16);

// Hilbert 辅助范围查询
spatial_query_result_t results[100];
int count = spatial_engine_hilbert_search(db, &query_bbox, results, 100);

// Hilbert 辅助 KNN 查询
count = spatial_engine_hilbert_knn(db, &point, 5, results, 5);

// R-Tree 范围查询
rtree_result_t rtree_results[10];
spatial_engine_search_bbox(db, &query_bbox, rtree_results, 10);

spatial_engine_close(db);
```

### 3. 时序引擎 (`db/storage/ts/`)

支持时序数据的存储和聚合查询：

- **分段索引**：时间范围分段存储，每段使用压缩块
- **Gorilla 压缩**：高效的时序数据压缩（5-10x）
- **聚合查询**：SUM、AVG、MIN、MAX、COUNT
- **时间粒度**：秒、分钟、小时、天级聚合
- **保留策略**：TTL 过期数据清理

```c
// 创建时序引擎
void *db = ts_engine_open("metrics", ACCESS_MODE_READ_WRITE);

// 启用分段索引（默认启用）
ts_engine_enable_segment_index(db, 4096, GRANULARITY_HOUR);

// 插入数据点
ts_record_t record = { .timestamp = 1700000000000, .value = 42.5 };
ts_engine_insert(db, &record, sizeof(record));

// 查询聚合数据
ts_query_results_t results;
ts_engine_query(db, start_time, end_time, GRANULARITY_HOUR, AGG_AVG, &results);

// 使用分段索引查询
uint32_t seg_count;
uint64_t total_points;
ts_engine_segment_stats(db, &seg_count, &total_points);

// 压缩
ts_engine_enable_compression(db);

// TTL 过期清理
ts_engine_expire(db, older_than_ms);

ts_engine_close(db);
```

### 4. KV 引擎 (`db/storage/kv/`)

键值存储引擎：

- **WAL 支持**：写前日志保证数据一致性
- **TTL 支持**：键值对过期管理
- **批量操作**：批量读取

```c
// 创建 KV 引擎
kv_t *db = kv_open("mykv.db");

// 基本操作
kv_put(db, "key1", 4, "value1", 6);
const char *val = kv_get(db, "key1", 4);
kv_delete(db, "key1", 4);

// 带 TTL 的键值
kv_put_with_ttl(db, "session:123", 11, "data", 4, 3600000); // 1小时后过期

kv_close(db);
```

### 5. 图引擎 (`db/storage/graph/`)

图数据存储和遍历：

- **CSR 存储格式**：压缩稀疏行格式，O(1) 出边/入边查询
- **COO 增量缓冲区**：支持增量边添加，定期合并到 CSR
- **顶点/边操作**：创建、删除、属性管理
- **遍历算法**：BFS、DFS、最短路径、PageRank
- **遍历包装层**：易于使用的遍历接口

```c
// 创建图引擎
void *db = graph_engine_open("social", ACCESS_MODE_READ_WRITE);

// 启用 CSR 存储（默认启用）
graph_engine_enable_csr(db, 1000000);  // 最大顶点数

// 添加顶点和边
graph_vertex_id_t alice = graph_engine_add_vertex(db, "Alice", ...);
graph_vertex_id_t bob = graph_engine_add_vertex(db, "Bob", ...);
graph_engine_add_edge(db, alice, bob, "friend");

// 使用 CSR 快速查询出边
uint32_t out_count;
const graph_csr_edge_t *out_edges = graph_engine_get_out_edges(db, alice, &out_count);

// 使用 CSR 快速查询入边
uint32_t in_count;
const graph_csr_edge_t *in_edges = graph_engine_get_in_edges(db, bob, &in_count);

// 合并 COO 缓冲区到 CSR
graph_engine_csr_compact(db);

// BFS 遍历
graph_engine_bfs_result_t bfs_result;
graph_engine_bfs(db, alice, 3, &bfs_result);

// 最短路径
graph_path_result_t path;
graph_engine_shortest_path(db, alice, bob, &path);

// PageRank
graph_engine_pagerank_result_t pr;
graph_engine_pagerank(db, 0.85, 100, 1e-6, &pr);

// 保存 CSR 存储
graph_engine_save_csr(db);

graph_engine_close(db);
```

### 6. 文档引擎 (`db/storage/doc/`)

文档存储和 JSONPath 查询：

- **倒排索引**：全文搜索索引，支持 BM25 打分
- **FST 术语字典**：高效术语查找
- **增量编码压缩**：倒排列表压缩存储
- **JSON 文档**：存储和检索 JSON 文档
- **JSONPath 查询**：支持 `$.field`、`$.a.b.c`、`$.arr[0]`

```c
// 创建文档引擎
void *db = doc_engine_open("docs", ACCESS_MODE_READ_WRITE);

// 启用倒排索引（默认启用）
doc_engine_enable_inverted_index(db, "simple");

// 插入 JSON 文档
const char *json = "{\"name\":\"test\",\"value\":42}";
doc_engine_insert(db, doc_id, json, strlen(json));

// 倒排索引搜索
doc_inverted_result_t results[100];
uint32_t count = doc_engine_inverted_search(db, "search term", results, 100);

// JSONPath 查询
doc_jsonpath_result_t *jp_results;
int jp_count = doc_engine_query_jsonpath(db, "$.name", &jp_results, 100);

// 清理
doc_engine_free_jsonpath_results(jp_results, jp_count);
doc_engine_close(db);
```

### 7. 树引擎 (`db/storage/yang/`)

层次结构数据存储：

- **树结构**：节点、父子关系、路径
- **持久化**：树结构保存到 `tree.yang` 文件
- **遍历接口**：递归遍历、回调式访问

```c
// 创建树引擎
void *db = yang_engine_open("config", ACCESS_MODE_READ_WRITE);

// 插入节点
yang_engine_insert(db, "/root/server", "server", YANG_NODE_ELEMENT, "config");

// 查找节点
yang_node_t *node = yang_engine_find(db, "/root/server");

// 遍历树
yang_engine_traverse(db, callback, ctx);

yang_engine_close(db);
```

### 存储引擎对比

| 引擎 | 数据模型 | 索引 | 查询能力 | 压缩 | 优化特性 |
|------|---------|------|---------|------|----------|
| 向量引擎 | 向量 | HNSW | KNN 搜索 | PQ 量化 | VecPage 分页 |
| 空间引擎 | 几何 | R-Tree + Hilbert | bbox/KNN | - | Hilbert 索引 |
| 时序引擎 | 时序点 | 分段索引 | 聚合查询 | Gorilla | 时间分段 |
| KV 引擎 | 键值对 | - | 点查询 | - | - |
| 图引擎 | 图 | - | 遍历/PageRank | - | CSR 存储 |
| 文档引擎 | JSON | 倒排索引 | JSONPath/全文 | - | BM25 |
| 树引擎 | 层次树 | - | 路径查询 | - | - |

### 引擎注册

所有存储引擎通过 `storage_engine.h` 中的 `storage_ops_t` 接口统一注册：

```c
// 注册引擎
storage_engine_register(MODEL_VECTOR, vector_engine_get_ops());
storage_engine_register(MODEL_SPATIAL, spatial_engine_get_ops());
storage_engine_register(MODEL_TIMESERIES, ts_engine_get_ops());
storage_engine_register(MODEL_KEY_VALUE, kv_engine_get_ops());
storage_engine_register(MODEL_GRAPH, graph_engine_get_ops());
storage_engine_register(MODEL_DOCUMENT, doc_engine_get_ops());
storage_engine_register(MODEL_TREE, yang_engine_get_ops());
```
