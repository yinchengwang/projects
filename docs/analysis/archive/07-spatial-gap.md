> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# Spatial 模态差距深度分析

> 审查日期：2026-08-27 ｜ 审查方式：静态代码审查
> 代码位置：`engine/src/db/storage/spatial/`（~2.85K 行，6 文件）+ `engine/src/db/index/hilbert.h`

## 1. 实现现状盘点

### 1.1 模块清单

| 模块 | 文件 | 行数 |
|------|------|------|
| 引擎主体 | `spatial_engine.c` | 808 |
| 几何类型 | `spatial_geo.c` | 600 |
| R-Tree 文件持久化 | `rtree_file.c` | 470 |
| R-Tree 算法 | `rtree.c` | 662 |
| 四叉树 | `spatial_quadtree.c` | 298 |

### 1.2 关键事实修正（相对 8 月 25 日旧对比文档）

- 旧文档称"Hilbert 曲线支持"——**确认**。`spatial_engine.c:7` `#include "db/index/hilbert.h"`、:163/489/540/670/686-740 多处 Hilbert 辅助索引创建/查询路径
- 旧文档称"R-Tree 索引"——**确认**。`rtree.c` 实现 Guttman quadratic split（:227+）
- 旧文档称"缺少 Multi*/GeometryCollection/CircularString"——本次未逐个核实，留作 P3 验证项

## 2. 代码级质量审查

### 2.1 并发正确性

**缺陷 1：R-Tree 完全无锁——并发 insert/search 同 vector UAF 风险「确认·实现质量缺陷」**

`rtree.c` grep `pthread_mutex|spinlock|use_lock|lock_` 零命中。`rtree_insert_impl`（:300）与 `rtree_search_impl`（:411）共享同一 tree 结构无同步——并发插入触发节点分裂 + 并发搜索读取 children 数组 → 读到悬挂指针/已释放子节点。

`spatial_engine.c` 也无锁——四模态中 Graph/Vector/Timeseries/Document 复制了同一份 buggy 自旋锁（见前卷 2.1），Spatial 是"连复制都懒得做"——更糟。

### 2.2 崩溃恢复

**缺陷 1：R-Tree 文件持久化无 fsync「疑似·实现质量缺陷」**

`rtree_file.c`（470）未核全文，但从 spatial 模态整体看（无 WAL grep 命中），与 Vector/Timeseries/Document 同病——持久化无 fsync。

**缺陷 2：Hilbert 辅助索引重建路径与 R-Tree 主索引同步「疑似·实现质量缺陷」**

`spatial_engine.c:686-740` Hilbert 索引用于 R-Tree 查询优化，但 Hilbert 与 R-Tree 是两个独立结构，崩溃后两者一致性需要事务或重排序；本次未深入。

### 2.3 内存安全

**正面证据 1：R-Tree quadratic split 算法忠实 Guttman「确认」**

`rtree.c:227-300` 的 `rtree_quadratic_split` 严格按 Guttman 1984 实现：pick_seeds（:192）选最浪费的种子对，quadratic cost 按面积增量分配剩余条目。

**缺陷 1：double 等值比较「确认·实现质量缺陷（边界 case）」**

`rtree.c:270` `enlarge_a == enlarge_b`——浮点等值比较在不同输入路径下可能产生极小差异（同一边界但表达式不同），导致分配结果非确定。学术实现常用严格 `<` 加 `area_a < area_b` 兜底（当前已含，但等值那一支仍依赖 IEEE-754 严格等值）。

**正面证据 2**：`rtree_node_free`（:48）递归释放子节点，路径完整；`rtree_node_create`（:19）calloc 失败回滚。

### 2.4 错误处理

**缺陷 1：rtree_insert 返回值未核全文「疑似·实现质量缺陷」**

`rtree_insert:360` 返回 int（推测 0/-1），但 rtree_insert_impl（:300）路径中节点分裂失败如何传播未核全文——可能留下不一致状态（已分裂但未传播到根）。

### 2.5 算法实现质量

**正面证据 1：R-Tree + Hilbert 双重索引是 PostGIS/H3 之外的合理工程路径「确认」**

Hilbert 曲线将空间局部性映射到一维序，再走 R-Tree 减少搜索范围——优于纯 R-Tree 的纯 bbox 重叠测试；与 H3 网格索引（S2/H3 全覆盖切割）是不同取舍。

**缺陷 1：仅平面欧氏距离——无 Haversine/球面坐标「确认·功能缺失」**

`spatial_engine.c` grep `haversine|spherical|geography` 零命中。地理数据（GPS 经纬度）若直接用平面欧氏距离：纬度方向 1° ≈ 111 km，赤道与极地差距巨大——错得离谱。PostGIS 用 `geography` 类型专门处理；DuckDB Spatial 有 sphere_distance。

**缺陷 2：四叉树文件 298 行——可能不如 R-Tree 主流「疑似·设计」**

PostGIS/DuckDB Spatial 主用 R-Tree（GiST 变体），四叉树更多用于客户端渲染；服务端两个都实现（rtree.c 662 + spatial_quadtree.c 298）维护成本翻倍。

**缺陷 3：空间分析函数（Union/Buffer/Intersection）未核「疑似·功能缺失」**

旧文档列为重大 gap（缺 900+ ST_* 函数）；本次未深入 spatial_geo.c 核 600 行全量函数清单——按 P2-3 OpenSpec 文档（commit 719c06d9c）应有落地。

### 2.6 API 设计

**正面证据**：R-Tree 提供 `rtree_insert/search`（:360/436）极简核心 API，外层 spatial_engine 集成到 mm_storage；Hilbert 辅助索引作为可选查询路径。

**缺陷 1：API 缺乏 PostGIS 兼容性包装「确认·功能缺失」**

PostGIS 提供 `ST_Distance/Within/Intersects/Union/Buffer/Contains/...` 1000+ ST_* 函数——生态兼容性是 GIS 应用迁移成本的核心。无 ST_* 命名空间意味着 PostGIS 用户无法平滑迁移。

## 3. 业界标杆对比

| 维度 | 自实现 | PostGIS 3.4 | DuckDB Spatial | H3 | SpatiaLite |
|------|--------|-------------|----------------|-----|-----------|
| 几何类型 | Point/LineString/Polygon | 全部 OGC（含 Multi*/Curve） | Point/Line/Polygon | 仅六边形 | 全部 OGC |
| 索引 | R-Tree + Hilbert（双重） | GiST（R-Tree 变体） | R-Tree（Spatial Join 优化） | H3 网格 | R-Tree |
| 空间函数 | 基础 + P2-3 补充 | ST_* 1000+ | ST_* 子集 | H3_* 函数 | ST_* 完整 |
| 坐标系统 | 仅平面 | 平面 + 球面（geography） | 平面 + 球面 | 球面 | 平面 + 球面 |
| 距离 | 平面欧氏 | ST_Distance/Haversine/Vincenty | 平/球 | H3 grid | 平/球 |
| Hilbert 曲线 | ✓ | ✓ | ✓ | ✗（用 H3） | ✓ |
| 拓扑 | ✗ | ✓（拓扑/网络/3D） | ✗ | ✗ | ✗ |

## 4. 差距矩阵

| 维度 | 评分 | 关键证据 |
|------|------|---------|
| 并发正确性 | 3 | R-Tree 无锁 `rtree.c`；spatial_engine 无锁——四模态中唯一连 buggy 自旋锁都没复制的 |
| 崩溃恢复 | 3 | 无 WAL 集成；Hilbert 与 R-Tree 同步待核 |
| 内存安全 | 5 | Guttman quadratic split 忠实实现；double 等值比较边界风险 `rtree.c:270` |
| 错误处理 | 5 | rtree_insert 失败传播未核全文 |
| 算法实现质量 | 5 | R-Tree + Hilbert 双重索引合理；仅平面无 Haversine |
| API 设计 | 4 | 核心 R-Tree API 清晰；缺 ST_* PostGIS 兼容包装 |

**实现质量缺陷清单（3 项确认 + 4 项疑似）**：
1. R-Tree 无锁（并发）
2. double 等值比较边界（算法）
3. 无球面距离 / geography 类型（功能缺失）
4. 无 WAL（疑似）
5. Hilbert 与 R-Tree 同步（疑似）
6. ST_* PostGIS 兼容（功能缺失）
7. rtree_insert 失败传播（疑似）

## 5. 改进优先级

| 优先级 | 项目 | 分类 | 工作量 |
|--------|------|------|--------|
| P0 | 抽取公共并发原语库——五模态同时上锁（vector/ts/document/graph/spatial） | 实现质量缺陷 | M |
| P0 | spatial 接入 WAL | 实现质量缺陷 | M |
| P1 | Haversine 球面距离 + geography 类型 | 功能缺失 | M |
| P1 | double 等值改 `<=` + tie-breaker（area_a < area_b） | 实现质量缺陷 | S |
| P2 | ST_* PostGIS 兼容子集（Distance/Within/Intersects/Contains/Buffer/Union） | 功能缺失 | L |
| P2 | Hilbert/R-Tree 崩溃后一致性协议 | 实现质量缺陷 | M |
