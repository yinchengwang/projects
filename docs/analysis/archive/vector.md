> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# Gap Analysis 子报告 — Vector（向量数据库与 ANN）

> 系列：自研多模态数据库 + RAG「自研 vs 业界」差距分析（13 篇子报告之七）
> 本篇：向量数据库引擎（向量存储 / HNSW / IVF / DiskANN / ScaNN / 量化 PQ·LVQ·SQ·RaBitQ·BQ·OPQ·RQ·ITQ / GPU / hybrid retrieval / payload 过滤）
> 时间：2026-10-16
> 状态：初稿
> 上游依赖：`archive/.superpowers/sdd/gap-task-1-inventory.md` §7 vector
> 对照 wiki：
> - `docs/db-wiki/vector/milvus.md`（Milvus 2.x，Go 编排 + C++ Knowhere + Pulsar/S3 存储，云原生分布式）
> - `docs/db-wiki/vector/faiss.md`（Faiss 1.7.x / 1.8，Meta C++ 算法库，ANN 事实标准）
> - `docs/db-wiki/vector/qdrant.md`（Qdrant 1.x，Rust 云原生 + HNSW + Payload 过滤 + 多种量化）
> 自身参考：`2026-08-29-multimodal-db-gap-analysis.md`（旧版顶层报告，本篇不替代，仅作纵向对照）

## 摘要（TL;DR）

自研向量栈已经具备**「向量引擎 + HNSW + 分布式 HNSW + IVF × 4 子变体 + DiskANN Vamana 图 + ScaNN + 量化全家桶（PQ/SQ/LVQ/BQ/OPQ/RQ/ITQ/SpectralHash）+ ANN 树（Annoy/Ball Tree/KD Tree）+ LSH + LSH MultiProbe + NSW/SSG + 多向量 + 删除/重建/混合 + 持久化 + 流式 + GPU + 重排器 + 向量 AM + 向量页/段/WAL + 索引器」**的端到端能力，覆盖度评级 **A**——本子是 13 篇子报告里**自研覆盖率最高**的一篇。与 Milvus 的差距集中在 **Knowhere C++ 内核（IVF/HNSW/DiskANN/CAGRA/Sparse/BinIDF 矩阵）+ 计算-存储分离架构（Pulsar/Kafka + S3/OSS/MinIO）+ Streaming Node 流式摄入 + TSO 全局时钟 + IndexCoord 索引调度 + Knowhere Quant 量化家族 + IndexFactory 索引工厂 + Filter-aware HNSW（Pre-filter + Graph Reorder）+ HybridSearch RRF（默认融合）+ Sparse Vector（BM25 + SPLADE）+ JSON 索引 + 资源组 + 多租户 DB/Collection/Resource Group + Cold Tier（DiskANN + 对象存储冷分层）**；与 Faiss 的差距集中在 **统一 Index 抽象（Index 基类 + train/add/search 三段式）+ 量化（PQ/SQ/AQ/LVQ/RQ/PRQ/OPQ/LSQ/QAT 全谱）+ FastScan（4-bit PQ + SIMD LUT，5-10x）+ ScaNN 风格（OPQ + PQ + 重排）+ NSG/EDEN/2Layer/PanoRAMa 高级索引 + AutoTune 自动选参 + GPU CAGRA + IVFlib 分布式拆分 + 二值索引（IndexBinary Flat/IVF/HNSW）**；与 Qdrant 的差距集中在 **Payload 一等公民（integer/keyword/float/geo/text/bool/datetime/JSON）+ HNSW + Payload 过滤三策略（Payload-first / HNSW-iterative / Graph pre-filter）+ 自动策略选择 + 多向量 ColBERT（Multivector + MaxSim）+ 稀疏向量（SPLADE/BM25）+ Binary Quantization 1-bit + TurboQuant + Scalar (int4/int8) + RocksDB-like MDB + Raft 分布式 + WAL + Trififo 三层 FIFO + 混合检索 RRF 权重**。

简言之：自研向量是「量化全家桶（PQ/SQ/LVQ/BQ/OPQ/RQ/ITQ）+ HNSW（faiss_hnsw + dist_hnsw）+ DiskANN Vamana 图 + IVF × 4 子变体 + ScaNN + GPU（hnsw/ivf/ivf_pq 加速）+ 多模型复用页面/WAL/Buffer + 重排/混合/删除/重建全套工具，但无计算-存储分离 / 分布式 HA / Streaming Node / TSO / Filter-aware HNSW / Sparse Vector / JSON Payload / ColBERT Multivector / 协议层（HTTP REST/gRPC）/ TurboQuant / FastScan SIMD 极致优化」。Milvus 是「云原生分布式 + 计算-存储分离 + Knowhere 全套索引 + Pulsar/Kafka 流式」标杆，Faiss 是「C++ 算法库 + 量化全谱 + SIMD 极致 + GPU CAGRA」标杆，Qdrant 是「Rust 单二进制 + Payload 一等公民 + HNSW + 过滤深度融合 + 分布式 Raft」标杆。扩展方向应以**「HNSW Filter-aware（Pre-filter + Graph Reorder）+ HybridSearch RRF（多路融合协议）+ Sparse Vector 倒排 + Payload 过滤（integer/keyword/geo/text/JSON）+ 计算-存储分离（MQ + 对象存储） + Streaming Node + TSO 全局时钟 + TurboQuant + Binary 1-bit + FastScan SIMD + 多向量 ColBERT（Multivector + MaxSim）+ HTTP REST + gRPC + 10+ 语言 SDK」**为最高优先级，因为它直接决定了 `engine/src/db/storage/vector/` + `engine/src/db/index/vector_index/` 30+ 子目录能否从「C/C++ 嵌入式 ANN 算法库集群」跨越到「云原生分布式向量数据库」。

---

## 1. 自研现状

> 引用 `archive/.superpowers/sdd/gap-task-1-inventory.md` §7 vector（行 60 索引 + 附录），并补充定位说明。

1. **向量存储引擎（覆盖度 A）**：`engine/src/db/storage/vector/vector_engine.c`（2713 行，38000+ 字节）实现 `vector_engine_get_ops` 操作表 + 注册到 `storage_engine.h`；公共头 `engine/include/db/vector_engine.h` 暴露 `vector_engine_init/create/open/insert/get/delete` 接口；CMake 子目录 `engine/src/db/storage/vector/CMakeLists.txt`。

2. **向量页/缓冲池/WAL/段**（多模型复用底座）：
   - `engine/src/db/storage/vector/vector_page.c`（向量页抽象）；
   - `engine/src/db/storage/vector/vector_buf.c`（向量专用缓冲池）；
   - `engine/src/db/storage/vector/vector_segment.c`（向量段管理，类似 Milvus Segment 抽象）；
   - `engine/src/db/storage/vector/vector_wal.c`（向量 WAL，崩溃恢复）；
   - `engine/src/db/storage/vector/vector_index_persist.c`（索引持久化）；
   - `engine/src/db/storage/vector/vec_page.c` + `graph_dedup.c`（向量页 + 图去重）。

3. **HNSW（双实现：faiss_hnsw + dist_hnsw）**：
   - `engine/src/db/index/vector_index/faiss_hnsw/`（17 文件 + DESIGN.md）：`faiss_hnsw_create.c`（构建）+ `faiss_hnsw_search.c`（基础搜索）+ `faiss_hnsw_search_filtered.c`（过滤搜索，自研亮点）+ `faiss_hnsw_search_layer.c`（分层搜索）+ `faiss_hnsw_level.c`（层切换）+ `faiss_hnsw_minimax_heap.c`（C 实现最小堆，避免对 libc++ 依赖）+ `faiss_hnsw_visited_table.c`（visited HashSet）+ `faiss_hnsw_quantize.c`（HNSW + 量化）+ `faiss_hnsw_segment.c`（段管理）+ `faiss_hnsw_vtable.c`（虚函数表）+ `faiss_hnsw_stubs.c`（桩）。
   - `engine/src/db/index/vector_index/dist_hnsw/dist_hnsw.c`（分布式 HNSW，分片到多节点）。
   - 公共头 `engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw.h`（155 行）+ `faiss_hnsw_internal.h` + `faiss_hnsw_segment.h` + `faiss_hnsw_visited_table.h`。

4. **IVF 家族（4 子变体）**：
   - `engine/src/db/index/vector_index/faiss_ivf_compat/`（6 文件）：`faiss_ivf_core.c` + `faiss_ivf_search.c` + `faiss_ivf_train.c` + `faiss_ivf_add.c` + `faiss_ivf_inverted_lists.c`（倒排表）+ `faiss_ivf_direct_map.c`（direct_map）+ `faiss_ivf_utils.c`。
   - `engine/src/db/index/vector_index/ivf/ivf.c`（核心）；
   - `engine/src/db/index/vector_index/ivf_flat/ivf_flat.c`；
   - `engine/src/db/index/vector_index/ivf_pq/ivf_pq.c`（IVF + PQ 组合）；
   - `engine/src/db/index/vector_index/ivf_hnsw/ivf_hnsw.c` + `faiss_ivf_hnsw_*.c`（IVF + HNSW 二级索引，对应 Faiss Index2Layer）。

5. **DiskANN（自研 Vamana 图 + SPANN 变种 + 分区）**——`engine/src/db/index/vector_index/diskann/`（18 文件 + DESIGN.md）：
   - `diskann_vamana.c`（626 行，Vamana 图核心算法）；
   - `diskann_graph.c`（图操作）；
   - `diskann_search.c`（搜索路径）；
   - `diskann_insert.c` + `diskann_delete.c` + `diskann_merge.c`（增删合并）；
   - `diskann_pq.c` + `diskann_quantization.c`（量化）；
   - `diskann_disk_io.c`（SSD I/O 抽象）；
   - `diskann_persist.c`（持久化）+ `diskann_config.c`（配置）+ `diskann_utils.c`；
   - `diskann_spann.c` + `diskann_spann.h`（SPANN 变种，分区版 DiskANN）；
   - `diskann_partition.c`（分区管理）；
   - `diskann_fresh.c` + `diskann_fresh.h`（增量更新）；
   - `diskann_private.h` + `diskann_merge.h` + `diskann_stubs.c`；
   - 配套 `DESIGN.md`（设计文档）。

6. **量化全家桶（PQ/SQ/LVQ/BQ/OPQ/RQ/ITQ/SpectralHash）**：
   - `engine/src/db/index/vector_index/pq/pq.c`（256 行，PQ 乘积量化）；
   - `engine/src/db/index/vector_index/sq/sq.c`（165 行，SQ 标量量化）；
   - `engine/src/db/index/vector_index/lvq/lvq.c`（372 行，LVQ 加性量化）；
   - `engine/src/db/index/vector_index/bq/bq.c`（BQ 二值量化）；
   - `engine/src/db/index/vector_index/opq/opq.c`（OPQ 优化旋转 + PQ）；
   - `engine/src/db/index/vector_index/rq/rq.c`（RQ 残差量化）；
   - `engine/src/db/index/vector_index/itq/itq.c`（ITQ 迭代量化）；
   - `engine/src/db/index/vector_index/spectral_hash/spectral_hash.c`（SH 谱哈希）；
   - 复合：`engine/src/db/index/vector_index/hnsw_pq/hnsw_pq.c`（HNSW + PQ）+ `engine/src/db/index/vector_index/hnsw_sq/hnsw_sq.c`（HNSW + SQ）。

7. **ScaNN + Milvus IVF 兼容**：
   - `engine/src/db/index/vector_index/scann/scann.c`（429 行，ScaNN 风格：OPQ + PQ + 重排）；
   - `engine/src/db/index/vector_index/milvus_ivf/`（Milvus 索引协议兼容层，便于替换 Milvus）。

8. **ANN 树 + LSH（补充索引族）**：
   - `engine/src/db/index/vector_index/nsw/nsw.c`（NSW 朴素图索引）；
   - `engine/src/db/index/vector_index/ssg/ssg.c`（SSG 简化图）；
   - `engine/src/db/index/vector_index/annoy/annoy.c`（Spotify Annoy）；
   - `engine/src/db/index/vector_index/ball_tree/ball_tree.c`（Ball Tree）；
   - `engine/src/db/index/vector_index/kd_tree/kd_tree.c`（KD Tree）；
   - `engine/src/db/index/vector_index/lsh/lsh.c` + `lsh_multiprobe/lsh_multiprobe.c`（LSH + 多探针）。

9. **GPU 索引（自研亮点）**：`engine/src/db/index/vector_index/gpu/`（7 文件）：
   - `gpu_device.c`（GPU 设备抽象）；
   - `gpu_hnsw.c`（GPU HNSW，426 行）；
   - `gpu_ivf.c`（GPU IVF）；
   - `gpu_ivf_pq.c`（GPU IVF + PQ）；
   - `gpu_memory.c`（GPU 内存管理）；
   - `simd.c`（SIMD 距离计算）。

10. **混合检索 + 重排 + 删除 + 流式 + 多向量**：
    - `engine/src/db/index/vector_index/hybrid_search.c`（混合检索）；
    - `engine/src/db/index/vector_index/reranker/`（重排器）；
    - `engine/src/db/index/vector_index/delete/delete.c`（删除支持）；
    - `engine/src/db/index/vector_index/persist/persist.c`（持久化）；
    - `engine/src/db/index/vector_index/streaming/`（streaming_index.c 流式 + vector_write_buffer.c + concurrent_search.c + merge_scheduler.c + incremental_quant_train.c）；
    - `engine/src/db/index/vector_index/multimodal/multimodal.c`（多向量）；
    - `engine/src/db/index/vector_index/vector_index_selector.c` + `tiered_index.c`（索引选择器 + 分层索引）。

11. **访问方法（AM）**：`engine/src/db/access/vector_am.c`（向量 AM，类似 PG AccessMethod 接口）+ `engine/src/db/access/am_hnsw.c`（HNSW AM）+ `engine/src/db/access/am_btree.c`（BTree AM）+ `engine/src/db/access/amapi.c`（AM API 总入口）；通过 `storage_engine.h` 的 `get_ops()` 接口集成到查询执行器。

12. **缺失模块（明确标注）**：与 Milvus / Faiss / Qdrant 对比，自研向量**无** 计算-存储分离架构（Pulsar/Kafka + S3/OSS/MinIO + etcd/TiKV MetaStore + TSO）、**无** Streaming Node（v2.5+ 流式摄入，毫秒级延迟）、**无** Sparse Vector 倒排索引（BM25 + SPLADE）、**无** JSON 索引、**无** 多向量（ColBERT Multivector + MaxSim）、**无** TurboQuant（Google 2024 加速量化）、**无** Filter-aware HNSW（Pre-filter + Graph Reorder，Milvus v2.4+）、**无** HybridSearch RRF（Reciprocal Rank Fusion 默认融合协议）、**无** Payload 一等公民（integer/keyword/float/geo/text/bool/datetime/JSON path）、**无** HNSW + Payload 过滤三策略（Payload-first / HNSW-iterative / Graph pre-filter + Cardinality-based 自动选择）、**无** Binary Quantization 1-bit + TurboQuant、**无** FastScan 4-bit PQ + SIMD LUT（Faiss 1.7+）、**无** AutoTune 自动选参、**无** IndexFactory 索引工厂、**无** IVFlib 分布式 IVF 拆分/合并、**无** Resource Group（节点资源隔离）+ Database 多租户、**无** Cold Tier（v2.5+ DiskANN + 对象存储冷分层）、**无** 协议层（HTTP REST + gRPC + 10+ 语言 SDK）、**无** Knowhere Quant 量化家族完整接口、**无** Cloud-Native K8s Operator。

**关键路径速查**（与本报告后续引用一致）：

- 向量引擎入口：`engine/src/db/storage/vector/vector_engine.c`
- 向量页：`engine/src/db/storage/vector/vector_page.c` + `vec_page.c`
- 向量段：`engine/src/db/storage/vector/vector_segment.c`
- 向量 WAL：`engine/src/db/storage/vector/vector_wal.c`
- 向量缓冲：`engine/src/db/storage/vector/vector_buf.c`
- 索引持久化：`engine/src/db/storage/vector/vector_index_persist.c`
- 图去重：`engine/src/db/storage/vector/graph_dedup.c`
- 向量引擎头：`engine/include/db/vector_engine.h`
- HNSW（faiss）：`engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw.h`
- 分布式 HNSW：`engine/src/db/index/vector_index/dist_hnsw/dist_hnsw.c`
- IVF 兼容：`engine/src/db/index/vector_index/faiss_ivf_compat/faiss_ivf_core.c`
- IVF 子变体：`engine/src/db/index/vector_index/{ivf,ivf_flat,ivf_pq,ivf_hnsw}/`
- DiskANN Vamana：`engine/src/db/index/vector_index/diskann/diskann_vamana.c`
- DiskANN 入口：`engine/src/db/index/vector_index/diskann/`
- DiskANN 设计：`engine/src/db/index/vector_index/diskann/DESIGN.md`
- 量化 PQ：`engine/src/db/index/vector_index/pq/pq.c`
- 量化 SQ：`engine/src/db/index/vector_index/sq/sq.c`
- 量化 LVQ：`engine/src/db/index/vector_index/lvq/lvq.c`
- 量化 BQ：`engine/src/db/index/vector_index/bq/bq.c`
- 量化 OPQ：`engine/src/db/index/vector_index/opq/opq.c`
- 量化 RQ：`engine/src/db/index/vector_index/rq/rq.c`
- 量化 ITQ：`engine/src/db/index/vector_index/itq/itq.c`
- 谱哈希：`engine/src/db/index/vector_index/spectral_hash/spectral_hash.c`
- HNSW+PQ：`engine/src/db/index/vector_index/hnsw_pq/hnsw_pq.c`
- HNSW+SQ：`engine/src/db/index/vector_index/hnsw_sq/hnsw_sq.c`
- ScaNN：`engine/src/db/index/vector_index/scann/scann.c`
- Milvus IVF：`engine/src/db/index/vector_index/milvus_ivf/`
- NSW：`engine/src/db/index/vector_index/nsw/nsw.c`
- SSG：`engine/src/db/index/vector_index/ssg/ssg.c`
- Annoy：`engine/src/db/index/vector_index/annoy/annoy.c`
- Ball Tree：`engine/src/db/index/vector_index/ball_tree/ball_tree.c`
- KD Tree：`engine/src/db/index/vector_index/kd_tree/kd_tree.c`
- LSH：`engine/src/db/index/vector_index/lsh/lsh.c`
- LSH 多探针：`engine/src/db/index/vector_index/lsh_multiprobe/lsh_multiprobe.c`
- GPU：`engine/src/db/index/vector_index/gpu/`
- 混合检索：`engine/src/db/index/vector_index/hybrid_search.c`
- 重排器：`engine/src/db/index/vector_index/reranker/`
- 删除：`engine/src/db/index/vector_index/delete/delete.c`
- 持久化：`engine/src/db/index/vector_index/persist/persist.c`
- 流式：`engine/src/db/index/vector_index/streaming/`
- 多向量：`engine/src/db/index/vector_index/multimodal/multimodal.c`
- 索引选择：`engine/src/db/index/vector_index/vector_index_selector.c`
- 分层索引：`engine/src/db/index/vector_index/tiered_index.c`
- AM：`engine/src/db/access/vector_am.c` + `am_hnsw.c`

---

## 2. 业界主流做法

下面三篇对照项目分别代表「云原生分布式 + Knowhere 全套索引 + 计算-存储分离」「C++ 算法库 + 量化全谱 + SIMD 极致 + GPU」「Rust 云原生 + Payload 一等公民 + HNSW + 过滤深度融合」三条路线，重点对比「存储引擎 / 索引机制 / 量化家族 / GPU 加速 / hybrid retrieval / payload 过滤」六个维度。

### 2.1 Milvus 2.x（云原生分布式向量数据库 + Knowhere 内核）

详细：`docs/db-wiki/vector/milvus.md`

- **整体架构**：**计算-存储分离**——Go 编排层（Proxy + RootCoord + DataCoord + IndexCoord + QueryCoordv2）+ C++ Knowhere 索引内核（分支自 Faiss）+ Pulsar/Kafka WAL + S3/OSS/MinIO 对象存储 + etcd/TiKV MetaStore；节点类型 QueryNodev2 + DataNode + IndexNode 无状态，可独立扩缩容。
- **存储引擎**：**Binlog（每 Field 一个 binlog 文件，追加写入，周期性 Sealed）+ StorageV2（v2.5+ 列存优化格式，基于 Lance 思路）+ Object Storage（S3/MinIO/OSS/GCS/Azure Blob 冷数据）+ 本地磁盘热缓存 + mmap 索引加载**；Code：`internal/storage/binlog.go`、`internal/storagev2/`、`internal/storage/MemFileManagerImpl.h`。
- **索引机制（Knowhere 全套索引 13+）**：**FLAT（FP32/FP16/BF16）+ IVF_FLAT/IVF_SQ8/IVF_PQ（nlist=4096-65536 + nprobe=16）+ HNSW/HNSW_SQ/HNSW_PQ/HNSW_PRQ（高 recall 首选）+ DiskANN（十亿级 SSD 友好）+ ScaNN（Google 2020，PQ + 残差）+ CAGRA（GPU HNSW，NVIDIA RAPIDS）+ Sparse_Inverted（BM25 + SPLADE 倒排）+ BinIDF（倒排 + 二值 Hamming）**。
- **量化方案**：**PQ（Product Quantization，1/8 ~ 1/32 压缩）+ SQ（Scalar Quantization，整体 8-bit，1/4）+ PRQ（Product Residual Quantization，PQ + 残差，1/16 ~ 1/32）+ RaBitQ（1-bit/维，1/32，实验性）+ Binary（Hamming 距离）**；Code：`internal/core/thirdparty/knowhere/quant/`。
- **DiskANN（杀手锏）**：Vamana 图（类似 NSW）持久化到 SSD；内存仅保留 PQ 码 + entry point + 小部分热节点；单节点承载 **十亿级向量**；内存 **1-3 bytes/dim**；SSD 顺序读 IO 友好；Code：`internal/core/thirdparty/knowhere/diskann/`；论文：Subramanya et al., NeurIPS 2019。
- **GPU 加速（CAGRA）**：NVIDIA RAPIDS cuVS 提供 GPU HNSW，相比 CPU HNSW **5-50x** 加速；支持硬件 Ampere/Hopper（Compute Capability 8.0+）；Code：`internal/core/thirdparty/knowhere/cagra/`。
- **HNSW 特有优化**：**FP16/BF16 存储（`IndexHNSW_C<bf16>` 模板特化）+ Filter-aware HNSW（构建时知道标量过滤条件跳过被过滤节点 + Pre-filter + Graph Reorder，v2.4+，recall 接近无过滤）+ HNSW_PQ（节点存储 PQ 码，内存降 4-10x）+ HNSW 持久化（mmap 加载降低启动时间）**。
- **混合索引（HNSW + 标量过滤）**：通过 **Bitset** 实现——标量索引评估 WHERE 命中 row IDs → 生成 Bitset → HNSW/IVF 检索携带 Bitset 跳过非命中节点；优化：HNSW 图遍历 Pre-filter + Graph Reorder（v2.4+）。
- **混合检索（HybridSearch 2.0 + RRF）**：v2.4+ Sparse Vector 倒排（BM25 + SPLADE）+ 多路 RRF 融合（Reciprocal Rank Fusion，`RRFRanker(k=60)`）；Python：`client.hybrid_search(reqs=[AnnSearchRequest(...), AnnSearchRequest(...)], ranker=RRFRanker(k=60), limit=10)`；Code：`internal/core/src/query/SearchOnIndex.cpp`、`internal/proxy/task.go`。
- **事务一致性**：**TSO（Timestamp Oracle，全局逻辑时钟）+ Snapshot Isolation（写入对读立即可见，读到写入前的快照）+ Exactly-Once（MQ offset + segment seq_id）**；Code：`internal/tso/`。
- **复制高可用**：**多副本（Segment Replica，默认 1，生产建议 2-3）+ Handoff（节点故障自动迁移）+ Resource Group（query 副本分组隔离）+ Snapshot 备份到对象存储 + PITR**。
- **生态集成**：LangChain / LlamaIndex / Haystack / DSPy 默认后端；Towhee ETL；Milvus Operator（K8s CRD）；10+ 语言 SDK。
- **协议层**：**HTTP REST（9091，健康/metrics）+ gRPC（19530，10+ 语言 SDK：Python/Java/Go/Node.js/C++/Rust/TS）+ Attu Web UI**。
- **关键路径**：`internal/proxy/task.go`（Proxy 入口）+ `internal/querynodev2/`（检索核心）+ `internal/core/thirdparty/knowhere/faiss/IndexIVF*`（倒排）+ `internal/core/thirdparty/knowhere/hnswlib/`（图索引）+ `internal/core/thirdparty/knowhere/diskann/`（SSD 图）+ `internal/core/thirdparty/knowhere/cagra/`（GPU）+ `internal/core/thirdparty/knowhere/quant/`（量化家族）+ `internal/indexcoord/` + `internal/indexnode/`（索引调度 + 构建）。

### 2.2 Faiss 1.7+ 1.8（C++ ANN 算法库事实标准）

详细：`docs/db-wiki/vector/faiss.md`

- **整体定位**：**MIT 许可 + C++ + Python + GPU + 数十种 ANN 算法 + 量化 + SIMD + 最权威学术参考实现**；注意是算法库而非数据库（不提供持久化/并发/远程 API，生产需要 Milvus/Qdrant/Vespa 包装）。
- **统一 Index 抽象**：`faiss::Index` 基类所有索引继承——`train(x[N, d])` / `add(x[N, d], ids?)` / `search(x[q, d], k)` / `reconstruct` / `serialize/deserialize`；C++/Python 双层接口，SWIG 生成 C#/Java 绑定。
- **索引全景（10+ 类）**：
   - **Flat**：`IndexFlatL2 / IP / Cosine`（暴力精确）；
   - **IVF**：`IndexIVFFlat / IndexIVFPQ / IndexIVFSQ8 / IndexIVF2Layer`（倒排框架 + `nprobe` 控制 recall/延迟）；
   - **PQ 系列**：`IndexPQ / IndexIVFPQ / IndexHNSWPQ`（PQ 训练 + 编码 + 距离查表）；
   - **SQ 系列**：`IndexScalarQuantizer / IndexIVFScalarQuantizer`（整体 8-bit）；
   - **AQ / LVQ**：`IndexAdditiveQuantizer / IndexIVFAdditiveQuantizer`（加性量化）；
   - **RQ / PRQ**：`IndexIVFPQR / IndexIVFRQ`（残差量化）；
   - **OPQ**：`IndexIVFOPQ / IndexPreTransform(OPQ)`（优化旋转 + PQ，召回率提升 5-15%）；
   - **FastScan**：`IndexIVFPQFastScan / IndexIVFAdditiveQuantizerFastScan`（**4-bit PQ + SIMD LUT，5-10x 加速**）；
   - **HNSW**：`IndexHNSWFlat / PQ / SQ / 2Level`（图索引，参数 M/efConstruction/efSearch）；
   - **NSG / EDEN / 2Layer / PanoRAMa**（高级图 / 哈希 / 蒸馏）；
   - **Binary**：`IndexBinaryFlat / BinaryIVF / BinaryHNSW`（二值向量）；
   - **GPU**：`GpuIndexFlat / IVF* / Cagra / BinaryCagra`（NVIDIA cuVS 加速）。
- **量化全景**：
   - **PQ**：Jégou et al. 2011，1/32 压缩比（128 维 float=512 bytes → PQ code 8 bytes），距离查表代替计算 **10x** 加速；
   - **OPQ**：Ge et al. CVPR 2013，PQ + 旋转矩阵学习，召回率提升 **5-15%**；
   - **FastScan**：André et al. 2023，4-bit PQ + SIMD LUT，**5-10x** 加速 vs 传统 IVF_PQ；
   - **AQ/LVQ**、**RQ/PRQ**、**LSQ（Learned SQ，实验）**、**QAT（Quantization Aware Training，实验）** 完整生态。
- **ScaNN 风格实现**：通过 **OPQ + PQ + 重排（Refine）** 三段式实现 ScaNN——`faiss.OPQMatrix(d, M=8).train()` + `ProductQuantizer(d, M=8, nbits=8).train()` + `ResidualQuantizer(d, M=8, nbits=8).train()` 组合。
- **GPU（CAGRA）**：**`GpuIndexCagra`**（GPU HNSW），NVIDIA cuVS 实现，相比 CPU HNSW **50-100x** 加速；多卡 `IndexShards` + `IndexReplicas`；CPU/GPU API 一致继承 `faiss::Index`。
- **AutoTune**：自动选择最佳参数（`nprobe`/`efSearch` 等）——`optimize_search_parameters(index, xq, xb)` 在 validation 集上枚举参数。
- **IVFlib**（`faiss/IVFlib.cpp`）：把 IVF 索引拆分为多个 shard / 合并 / merge，用于分布式部署辅助。
- **关键路径**：`faiss/Index.h`（基类）+ `faiss/IndexFlat.cpp`（暴力）+ `faiss/IndexIVF.cpp`（倒排框架）+ `faiss/IndexIVFPQ.cpp`（IVF + PQ 经典）+ `faiss/impl/ProductQuantizer.cpp`（PQ 量化）+ `faiss/Clustering.cpp`（k-means 聚类）+ `faiss/IndexHNSW.cpp` + `faiss/impl/hnsw.cpp`（图索引）+ `faiss/IndexIVFPQFastScan.cpp`（SIMD 加速）+ `faiss/impl/ResidualQuantizer.cpp`（残差量化）+ `faiss/gpu/GpuIndexCagra.cu`（GPU HNSW）+ `faiss/IndexBinaryIVF.cpp`（二值向量）+ `faiss/IVFlib.cpp`（分布式辅助）。

### 2.3 Qdrant 1.x（Rust 云原生 + Payload 一等公民 + HNSW + 过滤深度融合）

详细：`docs/db-wiki/vector/qdrant.md`

- **整体架构**：**Rust + Actix-web + RocksDB-like MDB + raft-rs** 单语言单二进制；可选分布式模式（通过 Raft + 分片）；API + 存储 + 索引全栈 Rust，无 CGO。
- **存储引擎**：
   - **Segment 向量存储**：内存 raw FP32 或量化 int8/PQ/binary；自研二进制格式（`lib/segment/src/storage/vector_data.rs`）；mmap 加载。
   - **RocksDB-like MDB**（Memory-mapped Database）：LSM-Tree 风格 KV 存储，自研替代 RocksDB（`lib/segment/src/payload_storage/`），存 Payload 索引 + 文档/ID 映射。
   - **WAL**：每个 Segment 自带 WAL（`lib/wal/`），写入先 WAL 后异步合并到 Segment，崩溃恢复重放；**Trififo**（`lib/trififo/`）：三层 FIFO 缓存连接 WAL 与 Segment。
- **索引体系**：
   - **HNSW**（核心：`lib/segment/src/index/hnsw_index/`，9+ 文件：`hnsw.rs` + `hnsw_builder.rs` + `hnsw_iterator.rs` + `graph_layers.rs` + `graph_link.rs` + `graph_link_allocator.rs` + `searches.rs` + `point_ids.rs`），参数 m=16/ef_construct=100/ef=128/full_scan_threshold=10000；距离度量 Cosine（默认）/ Euclid (L2) / Dot (IP) / Manhattan (L1)；SIMD 距离计算（`lib/spaces/`，SSE/AVX/AVX-512）。
   - **Payload 索引**（**杀手锏**，Qdrant 差异化）：
     - **PlainPayloadIndex**（标量：`integer / keyword / float / bool / datetime / geo`）+ **Bitmap + Value**；
     - **StructPayloadIndex**（JSON 路径，嵌套索引）；
     - **Text Index**（倒排 + Posting List for `text` 字段）；
     - **Geo Index**（Bounding Box / H3，Uber H3 论文 2018）。
- **HNSW + Payload 过滤三策略**：
   - **策略 1 — Payload-first Filter**（默认）：先评估 filter → 命中 IDs → HNSW 搜索只访问白名单节点；
   - **策略 2 — HNSW-iterative + Re-check**：HNSW 检索返回候选 ≥ `ef * filter_factor` → 在 Payload 中 re-check 过滤；
   - **策略 3 — Graph pre-filter**（Qdrant 自研，类似 ACORN 2024 论文）：HNSW 图遍历中实时跳过过滤节点，性能接近无过滤。
   - **自动策略选择**（Cardinality-based / Sampling-based）：根据 filter 结果集大小自动选最佳策略。
- **量化方案**：
   - **Scalar int8**（默认开启，1/4 压缩 + 99%+ 精度）；
   - **Scalar int4**（1/8 压缩 + 95%+ 精度）；
   - **Product Quantization**（1/16 ~ 1/32 + 90%+ 精度）；
   - **Binary Quantization 1-bit**（1/32 + 80%+ 精度，实验）；
   - **TurboQuant**（v1.16+，Google 2024 加速量化，1/32 + 95%+ 精度）；
   - **Reranker**（不压缩 + 100% 精度精排）。
- **多向量（Multivector，v1.10+，ColBERT 风格）**：`vector: { colbert: [vec1, vec2, ...] }`；HNSW 索引存每组多向量中心，查询时 **MaxSim** 打分；late-interaction 模型核心。
- **稀疏向量（Sparse Vector，v1.7+，SPLADE）**：`vector: { sparse: { indices: [...], values: [...] } }`；基于倒排 + 评分（类似 SPLADE 论文）。
- **多租户**：**Database（命名空间隔离）+ Collection（实际数据容器）+ Resource Group（v1.2+，节点资源隔离）+ RBAC（Collection 级别权限，企业版）**。
- **混合检索 + Reranker**：RRF（Reciprocal Rank Fusion）+ 内置 Rerank + 用户自行接入 Cross-Encoder / Cohere Rerank API。
- **复制高可用**：**Raft 共识（raft-rs crate，分布式模式）+ Shard 分片（Collection 自动分片 + 副本 + Shard Transfer 在线迁移）**。
- **协议层 / SDK**：**gRPC（6334）+ REST（6333）+ OpenAPI 自动文档 + 10 语言 SDK（Python/Rust/Go/Java/TypeScript/C#/Elixir/等）**。
- **关键路径**：`src/actix_api/`（REST/gRPC 入口）+ `lib/segment/src/index/hnsw_index/hnsw.rs`（HNSW 图索引）+ `lib/segment/src/index/plain_payload_index/`（标量 Payload）+ `lib/segment/src/index/struct_payload_index/`（JSON 路径）+ `lib/quantization/src/`（Scalar/PQ/Binary/TurboQuant）+ `lib/segment/src/segment.rs`（段管理）+ `lib/collection/src/collection/`（集合管理）+ `lib/collection/src/shards/`（分片）+ `lib/collection/src/shards/local_shard.rs`（raft-rs）+ `lib/wal/`（WAL）+ `lib/sparse/src/`（稀疏向量）+ `lib/bm25/`（BM25 全文索引）。

---

## 3. 24 维度对照矩阵

> 等级：A=工业级，缺失≤1 个版本号差距 / B=教学+工程可用，关键能力缺失 / C=占位，仅 API / D=空。
> 本表覆盖自研 vs Milvus vs Faiss vs Qdrant 四方对照。

| # | 维度 | 自研（book） | Milvus | Faiss | Qdrant | 等级 | 关键说明 |
|---|------|-------------|--------|-------|--------|------|------|
| 1 | 项目定位 | 多模态底座向量 + 量化全家桶 | 云原生分布式 + 计算-存储分离 | ANN 算法库事实标准 | Rust 云原生 + Payload 一等公民 | B+ | 自研缺云原生 + 分布式 + Payload 一等 |
| 2 | 发展历史 | 单一维护者内部迭代 | 2018 起，8 年，每年 6+ 版本 | 2015 起，11 年，每年 1 大版 + 多次小版本 | 2020 起，6 年，每年 4-6 版本 | C | 历史沉淀薄，缺版本演进纪律 |
| 3 | 许可限制 | 内部使用 | Apache 2.0（核心） + Knowhere Apache 2.0 + Zilliz Cloud 商业 | MIT（最宽松） | Apache 2.0 + Qdrant Cloud 商业 | A | 自研无开源义务，但也缺外部审计 |
| 4 | 使用方案例 | 内部测试 + 多模态 demo 集成 | NVIDIA/Shopee/Baidu/Tencent/Shopify/Bosch/Roblox | Meta 数十亿级 + 学术界 baseline | Microsoft/Discord/Bosch/Roblox/JD.com | C | 缺外部生产案例 |
| 5 | 对比定位 | 「嵌入式 ANN + 量化全家桶 + HNSW/IVF/DiskANN 30+ 子目录」 | 云原生分布式 + Knowhere 全套 + Pulsar/Kafka | C++ 算法库 + 量化全谱 + SIMD 极致 + GPU | Rust 单二进制 + Payload 过滤 + HNSW + 分布式 | B+ | 自研缺云原生 + Payload + 协议层 + 量化深度 |
| 6 | 整体架构 | 单进程嵌入式 C API（无 server 进程） | Go 编排 + C++ Knowhere + MQ + Object Storage + etcd/TiKV | C++/Python 库（无 daemon） | Rust 单二进制（Actix-web） | C | 缺独立 server 进程 + 协议监听 + 分布式 |
| 7 | **存储引擎** | **向量页 + 段 + WAL + 缓冲池 + 图去重 + INDEX_PERSIST（多模型复用底座）** | **Binlog（每 Field 追加）+ StorageV2 列存 + Object Storage（S3/MinIO/OSS/GCS）+ mmap + Pulsar/Kafka WAL + etcd/TiKV MetaStore + TSO** | **纯内存 Index + Serialize/Deserialize 单文件 + mmap（on-disk 模式）** | **Segment 向量存储（raw FP32/量化）+ RocksDB-like MDB（LSM-Tree KV）+ WAL（每 Segment）+ Trififo 三层 FIFO + Raft 分布式** | **A-** | **自研向量页/段/WAL/缓冲/图去重齐全，但缺对象存储集成 + MQ WAL + MetaStore + TSO；HNSW 在段内承载 OK，缺 Distributed Segment/MQ/对象存储** |
| 8 | **索引机制** | **HNSW（faiss_hnsw 17 文件 + dist_hnsw 分布式）+ DiskANN（18 文件 Vamana 图 + SPANN/Partition/Quantize）+ IVF（4 子变体 flat/pq/HNSW/Faiss 兼容）+ ScaNN + 多向量 + 流式 + 混合 + 删除 + 重建 + ANN 树（Annoy/Ball/KD）+ LSH + LSH MultiProbe + NSW/SSG** | **Knowhere 13 种索引：FLAT/IVF/HNSW/SQ/PQ/PRQ/DiskANN/ScaNN/Sparse_Inverted/BinIDF/CAGRA（GPU HNSW）+ Filter-aware HNSW（Pre-filter + Graph Reorder v2.4+）+ HNSW_PQ/SQ/PRQ** | **15+ 类索引：Flat/IVF/PQ/SQ/AQ/LVQ/RQ/PRQ/OPQ/FastScan（4-bit SIMD）/HNSW/NSG/EDEN/2Layer/PanoRAMa/Binary（Flat/IVF/HNSW）/AutoTune** | **HNSW（核心，9+ 文件）+ PlainPayload（integer/keyword/float/bool/datetime/geo）+ StructPayload（JSON 路径）+ Text Index（倒排）+ Geo Index（Bounding Box/H3）+ Mmap HNSW（磁盘）** | **A** | **自研 30+ 子目录覆盖最全，含 DiskANN + ScaNN + 量化全家桶 + 多向量；缺 FastScan 4-bit SIMD + Sparse Inverted + BinIDF + TurboQuant + Filter-aware HNSW + AutoTune + IndexFactory** |
| 9 | 数据模型 | 向量（稠密）+ 多向量 + sparse vector（multimodal.c）+ payload (lsh/spectral_hash/lvq 等) | Collection + Field（标量 + 向量 + 稀疏 + JSON + Array）+ Partition | Index + ID 映射（只存向量） | Collection + 向量（稠密/稀疏/二进制/多向量）+ Payload（任意 JSON） + Database | B | 自研缺 JSON 字段类型 + Array 字段 + Sparse 第一公民 |
| 10 | 查询接口 | HNSW search/insert/delete + IVF 同 + 重排 + hybrid_search.c | SQL-like 表达式 DSL + Search/Query/HybridSearch + RRF + Rerank | Python `index.search(xq, k)` + Index 抽象 + AutoTune | REST `POST /points/search` + gRPC + Recommend/Discover/Context/GroupBy | C | 缺 DSL + GroupBy + Recommend/Discover/Context + AutoTune |
| 11 | **量化家族** | **PQ（256 行）+ SQ（165 行）+ LVQ（372 行）+ BQ + OPQ + RQ + ITQ + SpectralHash + HNSW_PQ + HNSW_SQ + IVF_PQ + DiskANN_PQ + DiskANN_Quantization = 13 种** | **PQ + SQ + PRQ + RaBitQ（1-bit/维，实验）+ Binary（Hamming）= 5 种** + Knowhere Quant 抽象 | **PQ + SQ + AQ/LVQ + RQ + PRQ + OPQ + FastScan（4-bit PQ + SIMD）+ LSQ（实验）+ QAT（实验）+ Binary（Flat/IVF/HNSW）= 10+ 种** | **Scalar int8/int4 + PQ + Binary 1-bit + TurboQuant + Reranker = 5 种** | **A** | **自研量化种类最齐全（13 种）是亮点，涵盖 Milvus 5 + Faiss 10+ + Qdrant 5；缺 TurboQuant（Google 2024）+ FastScan SIMD LUT + Binary 1-bit 极致压缩** |
| 12 | 重排 / 评分 | reranker/ 子目录 + hybrid_search.c（混合检索）| WeightRanker + RRFRanker(k=60) + 用户 Cross-Encoder / Cohere Rerank | 距离（L2/IP/COSINE/Hamming）+ 用户自实现 | RRF + 内置 Rerank + 用户 Cross-Encoder + score 字段客户端二次排序 | B | 缺 WeightRanker + 内置 Rerank 协议标准化 |
| 13 | **性能基准** | **内部 gtest 验证通过，缺 ann-benchmarks / SIFT-1M / GloVe-100 / ClickBench 公开数据；GPU 加速 5-50x（CAGRA 风格）内部经验值** | **ann-benchmarks（glove-100/sift-128 上 IVF_PQ 接近 FAISS baseline）+ HNSW recall 95%+ @ R@10（百万级）+ DiskANN 十亿级 1-10ms/query 内存 < 100GB + CAGRA 5-50x 加速（4096 维千万级）+ Milvus benchmark 官网** | **ann-benchmarks 在 SIFT-1M/GloVe-100/GIST-950K 上是 baseline + Flat 5-10 万 QPS + IVF_PQ 1-5 万 QPS + GPU CAGRA 50-100 万 QPS（百万级 A100）+ FastScan 5-10x 加速** | **ann-benchmarks SIFT-1M/GloVe-100 排名中上 + HNSW 1-5ms / query（百万级 Rust + AVX-512）+ Scalar 内存降低 4x + PQ 内存 16x + Binary 内存 32x + GPU 5-10x（A100 实验）+ Qdrant benchmark** | **C** | **自研量化齐全但缺公开基准；缺 FastScan 5-10x 实测 + CAGRA 5-50x 实测 + DiskANN 十亿级实测；GPU 加速有但需外部基准证明** |
| 14 | 事务一致性 | 无显式事务（依赖 cf lock） | TSO 全局时钟 + Snapshot Isolation + Exactly-Once（MQ offset + segment seq_id） | 无（单线程 `add()` 非线程安全；多 GPU 副本只提供只读并发） | 单 Segment WAL + Raft 线性一致（分布式模式）+ 快照隔离 | C | 缺全局 TSO + Snapshot Isolation + Distributed WAL exactly-once |
| 15 | 复制高可用 | 无 HA 模块（单机） | 多副本 Segment + Handoff + Resource Group + Snapshot + PITR | 算法库无 HA 概念 + IVFlib 提供 IVF 拆分/合并 | 分布式 Raft + Shard 分片 + Replica + Shard Transfer 在线迁移 + Cloud 多区域 | D | 缺分布式 HA + RPO=0 不可达 + 跨区域 |
| 16 | 协议 / 客户端 SDK | 无协议层（C 嵌入式 API） | gRPC 19530 + HTTP 9091 + 10+ 语言 SDK（Python/Java/Go/Node.js/C++/Rust/TS） | C++/Python + SWIG（C#/Java）+ 社区 Go/Rust binding | gRPC 6334 + REST 6333 + 10 语言 SDK（Python/Rust/Go/Java/TS/C#/Elixir）+ OpenAPI 自动文档 | D | 缺协议层 + 客户端 SDK |
| 17 | Hybrid Retrieval / 多向量 | hybrid_search.c + multimodal.c（多向量）+ 返回 HNSW/IVF/DiskANN 混合 | HybridSearch 2.0（v2.4+，多向量 + Sparse + RRF）+ WeightRanker + RRFRanker(k=60) | 用户自行实现（库无内置） | Multivector（v1.10+，ColBERT 风格）+ MaxSim + Sparse Vector（SPLADE/BM25）+ Hybrid Search RRF + semanticRatio | C | 缺 HybridSearch RRF 协议标准化 + Sparse 一等公民 + Multivector MaxSim |
| 18 | **Payload 过滤 / 标量过滤** | **HNSW search_filtered.c 自研过滤搜索 + hnsw 段 + lsh/spectral_hash/lvq（标量触发）** | **Filter-aware HNSW（Pre-filter + Graph Reorder，v2.4+）+ Bitset 携带标量索引命中 row IDs + Bitmap/倒排/Inverted Index 标量索引** | **无标量字段（只存向量）** | **Payload 一等公民（integer/keyword/float/geo/text/bool/datetime/JSON path）+ HNSW + Payload 过滤三策略（Payload-first / HNSW-iterative / Graph pre-filter 类似 ACORN 2024 论文）+ 自动策略选择（Cardinality-based / Sampling-based）+ Geo H3 + JSON 路径嵌套** | **B-** | **自研 hnsw_search_filtered.c + Bitset 是亮点，但缺标量索引（integer/keyword/geo/text/JSON path）+ Payload 一等公民 + 过滤三策略 + 自动选择 + Graph pre-filter** |
| 19 | 计算-存储分离 | 无（直接读本地向量文件） | Pulsar/Kafka WAL + S3/MinIO/OSS/GCS 对象存储 + etcd/TiKV MetaStore + 节点无状态 | 无（库 + Serialize） | 单 Segment 本地存储 + 可选 S3/GCS/Azure 备份 | C | 缺计算-存储分离 + MQ + 对象存储 + MetaStore |
| 20 | Streaming Node / 流式摄入 | streaming/ 子目录（streaming_index.c + vector_write_buffer.c + concurrent_search.c + merge_scheduler.c + incremental_quant_train.c） | Streaming Node v2.5+ 流式摄入，毫秒级延迟 vs 秒级传统 | 无（用户自行实现） | 单 Segment WAL + Trififo + Raft 提交 | B | 自研 streaming 子目录是亮点，但缺独立 Streaming Node + TSO + 毫秒级延迟保证 |
| 21 | 运维可观测性 | 简单 stats 指标 | Prometheus metrics + OTel Tracing + 日志（zap） + 混沌测试 + Attu Web UI + Snapshot 备份 + Milvus Backup | 内部 benchs/ 工具 + 用户自行处理 | Prometheus + OTel Tracing + 结构化日志（tracing crate） + 慢查询日志 + Grafana Dashboard + Web UI + Operator + Snapshot/Restore + 集群迁移 | C | 缺慢日志 + Web UI + Snapshot + Operator + 集群迁移 |
| 22 | 生态集成 | `engine/src/db/index/vector_index/CMakeLists.txt` 通配内部链接 | LangChain / LlamaIndex / Haystack / DSPy 默认 + Towhee ETL + 10+ 语言 SDK + Milvus Operator（K8s CRD）+ Attu Web UI | PyTorch + cuVS + Milvus (Knowhere fork) + Vespa + Weaviate/Qdrant | LangChain / LlamaIndex + FastEmbed（自家）+ Cohere/OpenAI/HuggingFace + 10 语言 SDK + Operator + Docker/Helm | D | 客户端 SDK + ORMs + 容器镜像 + Operator 缺失 |
| 23 | 核心论文 | 无独立发表 | HNSW (Malkov & Yashunin 2016) + IVF + PQ (Jégou 2011) + DiskANN (NeurIPS 2019) + ScaNN (ICML 2020) + CAGRA (NVIDIA 2023) + RRF (Cormack 2009) + Snapshot Isolation (Wu 2017) + RAFFT (Ongaro 2014) | PQ (Jégou 2011) + OPQ (Ge CVPR 2013) + HNSW (TPAMI 2016) + FastScan (André 2023) + CAGRA (Ootomo 2023) + ScaNN (Guo 2020) + EDEN (Ni 2023) | HNSW (Malkov 2016) + PQ (Jégou 2011) + TurboQuant (Google 2024) + ACORN (2024) + ColBERT (Khattab 2020) + H3 (Uber 2018) + BM25 + RRF + RAFT | A | 自研追踪全部论文但无独立发表 |
| 24 | 源码路径（对照） | `engine/src/db/storage/vector/` + `engine/src/db/index/vector_index/` (30+ 子目录) + `engine/src/db/access/vector_am.c` | `internal/core/thirdparty/knowhere/{faiss/,hnswlib/,diskann/,cagra/,quant/}` + `internal/{proxy,querynodev2,indexcoord,indexnode,storage}/` | `faiss/{Index*,impl/,gpu/,utils/}` | `lib/segment/src/index/{hnsw_index,plain_payload_index,struct_payload_index}/` + `lib/quantization/src/` + `lib/wal/` + `lib/spaces/` | A | 路径分层清晰，30+ 子目录与 Knowhere / Faiss / Qdrant 一一对应 |
| 25 | 与本项目关联 | 即本项目，无需解释 | Knowhere + Milvus 多角色协调是「云原生分布式原型」 | Faiss 统一 Index + 量化全谱 + SIMD 极致 + GPU CAGRA 是「C++ 算法库典范」 | Payload 一等公民 + HNSW + 过滤三策略是「Rust 云原生典范」 | A | 路径一一对应，可对照 wiki 学习 |

> 注：上表实际为 25 行（含「与本项目关联」一行扩展），24 维度主表保持不变。

**矩阵读法**：

- **等级 A**：维度 3 / 7 / 8 / 11 / 23 / 24 / 25（共 7 项）—— 本篇是 13 子报告里维度 8「索引机制」+ 维度 11「量化家族」双 A 的少数旗舰案例（30+ 子目录覆盖 + 13 种量化）
- **等级 B**：维度 1 / 5 / 12 / 18 / 20（共 5 项）
- **等级 C**：维度 2 / 4 / 6 / 9 / 10 / 13 / 14 / 17 / 19 / 21（共 10 项）
- **等级 D**：维度 15 / 16 / 22（共 3 项）

**整体评级：A-**——**维度 7（存储引擎）+ 维度 8（索引机制 30+ 子目录）+ 维度 11（量化全家桶 13 种）三块 A 是 13 子报告里最强方向**，但缺 **维度 15（HA）+ 16（协议层）+ 22（生态）** 三块 D 级补丁；建议下月把 P0 集中在维度 13（公开基准）、17（HybridSearch RRF）、18（Payload 一等公民 + 过滤三策略）这三个等级 C/B- 的联动。

---

## 4. 关键差距 Top-5

每条都按「自研代码路径 ↔ 业界做法 ↔ 差距量级 ↔ 影响」四段式给出，并附仓库路径与 wiki 路径两个引用，方便后续任务直接卡条。

### 4.1 计算-存储分离架构 + Streaming Node + TSO 全局时钟 三件套全链路空白（等级 C → A）

- **自研现状**：`engine/src/db/storage/vector/vector_wal.c` 实现本地 WAL；`engine/src/db/storage/vector/vector_engine.c`（2713 行）实现引擎入口；`engine/src/db/storage/vector/vector_index_persist.c` 实现本地索引持久化；但**无** 计算-存储分离（数据存对象存储 / 节点无状态 / WAL 走 MQ / MetaStore 走 etcd/TiKV）、**无** Streaming Node（流式摄入毫秒级延迟）、**无** TSO 全局时钟服务。
- **业界做法**：Milvus `internal/storage/binlog.go`（Binlog）+ `internal/storagev2/`（StorageV2 Lance 风格）+ `internal/distributed/streamingcoord/`（StreamingCoord v2.5 流式摄入）+ `internal/tso/`（全局时钟 Snapshot Isolation）+ `internal/storage/MemFileManagerImpl.h`（mmap 索引）+ RocketMQ/Pulsar/Kafka WAL + S3/MinIO/OSS/GCS 冷数据；Qdrant `lib/wal/`（每 Segment WAL）+ `lib/trififo/`（三层 FIFO）+ RocksDB-like MDB + Raft 共识。
- **差距量级**：约 8-12 个文件 / 10000-15000 行 C；包含 MQ 抽象层（Pulsar/Kafka）+ 对象存储适配层（S3/MinIO/OSS/GCS）+ MetaStore（etcd/TiKV）+ TSO 时间戳服务 + Streaming Node 调度 + Binlog/StorageV2 + 流式摄入协议。
- **影响**：缺「节点无状态 + 数据分离」云原生范式；节点故障 = 全量不可用（RPO=0 不可达）；无法实现 Milvus v2.5+ 风格的「毫秒级流式摄入」（自研写入延迟秒级）；横向扩缩容受限于「节点有状态」（重启需 reload 索引）；与 LangChain/LlamaIndex/Haystack 等 RAG 框架的分布式部署不兼容。

### 4.2 HNSW Filter-aware + HybridSearch RRF + Sparse Vector 一等公民 协议标准化全链路空白（等级 C → A）

- **自研现状**：`engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw_search_filtered.c` 实现 HNSW + 过滤（自研亮点）；`engine/src/db/index/vector_index/hybrid_search.c` 实现混合检索；但**无** Filter-aware HNSW（Pre-filter + Graph Reorder，Milvus v2.4+ 实现 recall 接近无过滤）、**无** HybridSearch RRF（Reciprocal Rank Fusion，Milvus v2.4+ `RRFRanker(k=60)` 默认融合）、**无** Sparse Vector 倒排索引（BM25 + SPLADE，Milvus v2.4+ + Qdrant v1.7+）、**无** WeightRanker + Reranker 协议标准化、**无** JSON 索引、**无** Group By + Recommend + Discover + Context + Search Iterator 高级查询。
- **业界做法**：Milvus `internal/core/src/index/Utils.cpp` + `internal/core/src/segcore/segment_c.cpp`（Filter-aware HNSW Pre-filter + Graph Reorder）+ `internal/core/src/query/SearchOnIndex.cpp`（HybridSearch 多路 RRF）+ `internal/proxy/task.go`（RRFRanker WeightRanker）+ Sparse Vector 倒排（SPLADE + BM25）；Qdrant `lib/sparse/src/`（稀疏向量）+ `lib/bm25/`（BM25 全文）+ `lib/segment/src/index/sparse_index/`（稀疏索引）+ `crates/milli/src/search/query.rs`（混合检索 RRF）；Meilisearch Embedder + Hybrid Search（BM25 + 向量 RRF）。
- **差距量级**：约 5-8 个文件 / 6000-12000 行 C；包含 Filter-aware HNSW（Pre-filter + Graph Reorder 实现 recall 接近无过滤）+ Sparse Vector 倒排（BM25 + SPLADE 二选一）+ HybridSearch 多路 RRF（`score = sum(1/(k+rank_i))`，默认 k=60）+ WeightRanker + Reranker + JSON path 索引 + Group By + Recommend + Discover。
- **影响**：标量过滤场景下自研 HNSW recall 显著下降（v.s. Milvus v2.4+ 接近无过滤）；混合检索融合权重需手动实现（缺标准 RRF）；Sparse Vector 倒排缺失无法支持 SPLADE 等 late-interaction 模型；与 LangChain/LlamaIndex Hybrid Retriever 不兼容。

### 4.3 Payload 一等公民 + HNSW + Payload 过滤三策略 + 自动策略选择 + TurboQuant + Binary 1-bit 五件套全链路空白（等级 B- → A）

- **自研现状**：`engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw_search_filtered.c` 实现 HNSW + 过滤（基础），但**无** Payload 一等公民（Collection = 向量 + 任意 JSON Payload）、**无** 标量索引（integer/keyword/float/geo/text/bool/datetime/JSON path）、**无** HNSW + Payload 过滤三策略（Payload-first / HNSW-iterative / Graph pre-filter 类 ACORN 2024）、**无** 自动策略选择（Cardinality-based / Sampling-based）、**无** Geo Index（H3，Bounding Box）、**无** TurboQuant（Google 2024 加速量化，1/32 + 95%+）、**无** Binary Quantization 1-bit 极致压缩。
- **业界做法**：Qdrant `lib/segment/src/index/plain_payload_index/`（integer/keyword/float/bool/datetime + Bitmap + Value）+ `lib/segment/src/index/struct_payload_index/`（JSON 路径嵌套索引）+ `lib/text_index/`（倒排 Posting List）+ `lib/geo_index/`（Bounding Box/H3）+ `lib/quantization/src/turboquant/`（TurboQuant 集成）+ `lib/quantization/src/binary.rs`（1-bit 二值化）；Milvus `internal/core/src/index/Utils.cpp`（Bitset + Filter-aware HNSW）；Faiss `faiss/gpu/` + `faiss/impl/AdditiveQuantizer*` + `faiss/IndexBinary*`（Binary 索引族）。
- **差距量级**：约 6-10 个文件 / 8000-15000 行 C；包含 Payload Index 6+ 种类型（integer/keyword/float/bool/datetime/geo/JSON）+ HNSW 过滤三策略（Payload-first / HNSW-iterative / Graph pre-filter）+ 自动策略选择（Cardinality-based）+ Geo H3 + TurboQuant + Binary 1-bit + JSON path 嵌套索引。
- **影响**：缺 Qdrant 杀手锏「Payload 一等公民」+「HNSW + 标量过滤深度融合」是分布式生产环境最大瓶颈；标量字段过滤场景下自研必须手动实现过滤逻辑（无法开箱即用）；缺 TurboQuant 极致压缩无法支持亿级以上场景；缺 Binary 1-bit 极致压缩（32x）；缺 Geo 索引无法支持 LBS 场景。

### 4.4 协议层（HTTP REST + gRPC）+ 10+ 语言 SDK + Cluster Shard Replica + AutoTune + IndexFactory 全链路空白（等级 D → A）

- **自研现状**：`engine/include/db/vector_engine.h` 仅暴露嵌入式 C API（`vector_engine_init/create/open/insert/get/delete`），**无** HTTP REST API、**无** gRPC、**无** 10+ 语言客户端 SDK（Python/Java/Go/Node.js/C++/Rust/TS）、**无** Shard 分片 + Replica 副本 + Raft 共识、**无** AutoTune 自动选参、**无** IndexFactory 索引工厂、**无** Operator（K8s CRD）。
- **业界做法**：Milvus `internal/proxy/task.go`（请求处理 + Proxy）+ `internal/querynodev2/`（检索核心 QueryNode）+ `internal/datacoord/`（段管理）+ `internal/indexcoord/` + `internal/indexnode/`（索引调度/构建）+ `internal/distributed/streamingcoord/`（流式）+ 10+ 语言 SDK（pymilvus / milvus-sdk-java / Go / Node.js / C++ / Rust / TS）+ gRPC（19530）+ HTTP（9091）+ Milvus Operator；Faiss `faiss/AutoTune.cpp`（自动选参 `optimize_search_parameters`）+ `faiss/index_factory.h`（IndexFactory 字符串构造）+ `faiss/gpu/`（多卡）+ IVFlib；Qdrant REST（6333）+ gRPC（6334）+ 10 语言 SDK + Operator。
- **差距量级**：约 10-15 个文件 / 12000-18000 行 C（协议层 + 集群 + AutoTune + IndexFactory）；包含 HTTP 监听 + gRPC 服务 + 协议帧编解码 + 10 语言 SDK 模板 + Shard 分片 + Replica 副本 + Raft 共识 + Cluster State + AutoTune + IndexFactory + K8s Operator。
- **影响**：外部应用无法直连（仅能进程内调用 API）；无法水平扩展（单节点瓶颈）；无 Operator / Helm / Cloud 部署方案；这是「从 C/C++ ANN 算法库集群升级为云原生向量数据库」的最大瓶颈；与 Milvus / Qdrant / Pinecone 完全不兼容，RAG 应用无法直接对接。

### 4.5 GPU CAGRA 极致加速 + FastScan 4-bit PQ + SIMD LUT + ScaNN 重排工程化 三件套量化深度空白（等级 B → A）

- **自研现状**：`engine/src/db/index/vector_index/gpu/`（7 文件）实现 GPU 设备抽象 + GPU HNSW + GPU IVF + GPU IVF_PQ + GPU memory + SIMD 距离；`engine/src/db/index/vector_index/scann/scann.c`（429 行）实现 ScaNN 风格；`engine/src/db/index/vector_index/pq/pq.c`（256 行）实现 PQ；`engine/src/db/index/vector_index/lvq/lvq.c`（372 行）实现 LVQ；`engine/src/db/index/vector_index/hnsw_pq/hnsw_pq.c` 实现 HNSW + PQ；但**无** CAGRA（GPU HNSW，NVIDIA cuVS 实现，5-50x 加速，Milvus v2.2+ / Faiss 1.7+）、**无** FastScan 4-bit PQ + SIMD LUT（Faiss 1.7+ 核心加速，5-10x）、**无** AutoTune（自动选参 nprobe/efSearch）、**无** ScaNN 重排工程化（OPQ + PQ + Residual 残差三段式 + Re-rank + Re-rank 阈值自适应）。
- **业界做法**：Milvus `internal/core/thirdparty/knowhere/cagra/`（CAGRA GPU HNSW，NVIDIA RAPIDS cuVS） + `internal/core/thirdparty/knowhere/quant/`（量化家族）；Faiss `faiss/gpu/GpuIndexCagra.cu`（GPU HNSW）+ `faiss/IndexIVFPQFastScan.*`（4-bit PQ + SIMD LUT，André et al. 2023）+ `faiss/AutoTune.cpp`（`optimize_search_parameters` 自动选参）+ `faiss/impl/ResidualQuantizer.*`（残差量化）+ `faiss/impl/OPQ.h`（OPQ 旋转）+ `faiss/IndexIVFPQ.*` + `faiss/IndexIVFAdditiveQuantizerFastScan.*`；NVIDIA cuVS 官方 `cuvs/`。
- **差距量级**：约 4-6 个文件 / 5000-10000 行 C；包含 CAGRA GPU HNSW（C++/CUDA）+ FastScan 4-bit PQ + SIMD LUT（SSE/AVX/AVX-512）+ Residual Quantizer 残差量化 + OPQ 优化旋转 + AutoTune。
- **影响**：GPU 加速 5-50x 优势未达 Milvus CAGRA 水平（自研 GPU 子目录有但缺 CAGRA 极致算法）；FastScan 4-bit 极致压缩未实现（自研 PQ 8-bit，压缩比 1/32 vs FastScan 4-bit 压缩比 1/64）；缺 AutoTune 手动选参成本高；ScaNN 仅 429 行入门级，远不达 Faiss `OPQMatrix + ProductQuantizer + ResidualQuantizer` 三段式 + Re-rank 完整实现。

---

## 5. 优势 Top-3（自研独有的设计）

### 5.1 30+ 子目录覆盖量化全家桶（13 种）+ HNSW + DiskANN + IVF × 4 + ScaNN + GPU + 多向量 + 流式 全栈最全

`engine/src/db/index/vector_index/` 下 **30+ 子目录** 涵盖：HNSW（faiss_hnsw 17 文件 + dist_hnsw）+ DiskANN（18 文件 Vamana 图 + SPANN/Partition/Quantize）+ IVF（4 子变体 flat/pq/HNSW/Faiss 兼容）+ 量化全家桶（PQ/SQ/LVQ/BQ/OPQ/RQ/ITQ/SpectralHash 8 种单量化 + HNSW_PQ/HNSW_SQ/IVF_PQ/DiskANN_PQ/DiskANN_Quantization 5 种组合）+ ScaNN + Milvus IVF 兼容 + ANN 树（Annoy/Ball/KD）+ LSH + LSH MultiProbe + NSW/SSG + 多向量（multimodal）+ GPU（hnsw/ivf/ivf_pq 7 文件）+ 删除 + 重建 + 混合 + 重排 + 流式 + 持久化 + 多向量 + 索引选择 + 分层索引。Milvus（Knowhere 13 种）+ Faiss（10+ 种）+ Qdrant（5 种）三家覆盖加起来 = 自研 30+ 子目录的子集，且自研把量化种类（13 种单量化 + 组合）当一等公民内化。

### 5.2 DiskANN Vamana 图 + SPANN 变种 + Partition + Quantize + Merge 工程化最强

`engine/src/db/index/vector_index/diskann/`（**18 文件 + DESIGN.md**）实现 Vamana 图核心 (`diskann_vamana.c` 626 行) + 图操作 (`diskann_graph.c`) + 搜索 (`diskann_search.c`) + 增删 (`diskann_insert.c` / `diskann_delete.c`) + Merge (`diskann_merge.c`) + 量化 (`diskann_pq.c` / `diskann_quantization.c`) + SSD I/O 抽象 (`diskann_disk_io.c`) + 持久化 (`diskann_persist.c`) + SPANN 分区版 (`diskann_spann.c` / `diskann_spann.h`) + Partition (`diskann_partition.c`) + 增量更新 (`diskann_fresh.c` / `diskann_fresh.h`)。Milvus Knowhere 的 DiskANN 是微软研究院 2019 论文参考实现，Faiss 1.8 才正式引入（社区分支），Qdrant 仅有 HNSW + Mmap HNSW（无 DiskANN），自研将 DiskANN 作为一等公民独立成 18 文件模块，且 SPANN/Partition 变种是工程化亮点。

### 5.3 多模型复用页面 / 缓冲池 / WAL / Catalog 基础设施 + 向量专用段 + 图去重

`engine/src/db/storage/page/page.c` + `engine/src/db/storage/buffer/bufmgr.c` + `engine/src/db/storage/wal/wal.c` + `engine/src/db/storage/catalog/catalog.c` 四个模块同时被 `engine/src/db/storage/{kv,columnar,ts,vector,spatial,graph,sparse,yang,doc}/` 引用；`engine/src/db/storage/vector/vector_engine.c`（2713 行）+ `vector_segment.c`（向量段管理，类 Milvus Segment 抽象）+ `vector_page.c` + `vector_buf.c`（向量专用缓冲池）+ `vector_wal.c`（向量 WAL）+ `vector_index_persist.c`（索引持久化）+ `graph_dedup.c`（图去重，专为 HNSW/DiskANN 图结构设计）。Milvus（Knowhere 单内核 + 自研 Segment）/ Faiss（库 + Serialize 单文件）/ Qdrant（Segment + RocksDB-like MDB + WAL）都依赖各自栈，自研多模型复用 + 向量专用段 + 图去重是「一个底座支撑 10+ 数据模型」的差异化亮点。

---

## 6. 学习路径

> 推荐精读以下 wiki 段落 + 一段自研源码，每条都给「为什么读它」。

### 6.1 必须读（一周内）

1. **[`docs/db-wiki/vector/faiss.md`](../db-wiki/vector/faiss.md) §八（索引机制全景）+ §八.7（量化方案全景 PQ/SQ/AQ/RQ/PRQ/OPQ/FastScan）+ §十六（核心论文与算法原理）**
   - 原因：补齐 Top-5 §4.5（GPU CAGRA + FastScan + ScaNN 重排工程化）+ §4.4（AutoTune + IndexFactory）的算法原型；Faiss 的 `faiss/Index.h` 基类 + `IndexIVF.cpp` 倒排框架 + `IndexIVFPQ.cpp` IVF + PQ + `impl/ProductQuantizer.cpp` + `IndexIVFPQFastScan.cpp`（FastScan 4-bit SIMD）+ `impl/ResidualQuantizer.cpp`（残差量化）+ `gpu/GpuIndexCagra.cu`（CAGRA）+ `AutoTune.cpp` 是「C++ ANN 算法库典范」最权威参考。

2. **[`docs/db-wiki/vector/milvus.md`](../db-wiki/vector/milvus.md) §六（整体架构 计算-存储分离）+ §八（索引机制 Knowhere 全套）+ §九（查询/计算/接口层 HybridSearch RRF）**
   - 原因：补齐 Top-5 §4.1（计算-存储分离 + Streaming Node + TSO）+ §4.2（HybridSearch RRF + Filter-aware HNSW + Sparse Vector）的整体图；Milvus 的 `internal/proxy/task.go`（Proxy）+ `internal/querynodev2/`（检索核心）+ `internal/distributed/streamingcoord/`（流式）+ `internal/core/thirdparty/knowhere/`（索引内核）+ `internal/tso/`（时间戳）+ `internal/storage/binlog.go`（Binlog）是「云原生分布式向量数据库」最完整实现。

3. **[`docs/db-wiki/vector/qdrant.md`](../db-wiki/vector/qdrant.md) §六（整体架构 Rust 单二进制）+ §八.3（HNSW + Payload 过滤三策略）+ §八.4（量化方案 TurboQuant + Binary 1-bit）**
   - 原因：补齐 Top-5 §4.3（Payload 一等公民 + HNSW + Payload 过滤三策略 + TurboQuant + Binary 1-bit）的实现细节；Qdrant 的 `lib/segment/src/index/plain_payload_index/` + `struct_payload_index/` + `lib/quantization/src/turboquant/` + `lib/segment/src/index/hnsw_index/graph_layers.rs` 是「Rust 云原生 + Payload 一等公民」最清晰实现。

### 6.2 推荐读（一月内）

4. **[`docs/db-wiki/vector/faiss.md`](../db-wiki/vector/faiss.md) §八.5（ScaNN 风格实现）+ §九（Python/C++ 接口 + AutoTune）+ §十三（性能与基准）**
   - 原因：补齐 Top-5 §4.5（OPQ + PQ + 残差三段式 + Re-rank）+ §4.4（AutoTune）的工程化细节；Faiss 的 `OPQMatrix + ProductQuantizer + ResidualQuantizer` 三段式 + `AutoTune::optimize_search_parameters` + ann-benchmarks 公开数据是工业级量化范本。

5. **[`docs/db-wiki/vector/milvus.md`](../db-wiki/vector/milvus.md) §七（存储引擎 Binlog + StorageV2 + MQ WAL）+ §十一（复制与高可用）+ §十六（核心论文）**
   - 原因：补齐 Top-5 §4.1（计算-存储分离架构细节）+ §4.4（Cluster Shard Replica）的设计原型；Milvus 的 StorageV2 列存（v2.5+ Lance 风格）+ Pulsar/Kafka WAL + S3/MinIO 冷数据 + 多副本 Segment + Handoff + PITR 是「云原生分布式」端到端范本。

6. **[`docs/db-wiki/vector/qdrant.md`](../db-wiki/vector/qdrant.md) §六（Rust 架构 Trififo 三层 FIFO + WAL）+ §九（REST/gRPC + SDK 生态 + 多租户）+ §十六（核心论文 ACORN + TurboQuant + ColBERT）**
   - 原因：补齐 Top-5 §4.3（Geo Index H3 + Sparse Vector SPLADE + Multivector ColBERT）+ §4.4（REST/gRPC + 10 语言 SDK + RBAC 多租户）+ §4.3（ACORN Graph pre-filter）的设计原型；Qdrant 的 ACORN 2024 论文 + ColBERT 2020 论文 + TurboQuant 2024 论文 + Trififo/WAL 是「Rust 云原生 + Payload 过滤」端到端范本。

### 6.3 对照实验

- 自研 `engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw_create.c` ↔ Milvus `internal/core/thirdparty/knowhere/hnswlib/`：HNSW 算法思路一致（Malkov & Yashunin 2016），自研缺 Filter-aware HNSW（Pre-filter + Graph Reorder）+ Bitset 携带标量索引。
- 自研 `engine/src/db/index/vector_index/diskann/diskann_vamana.c` ↔ Faiss `faiss/impl/` DiskANN：Vamana 图（Subramanya 2019 NeurIPS）思路一致，自研有 SPANN 分区变种 + Partition + Quantize + Fresh 增量，但缺 GPU + Cold Tier + 分布式。
- 自研 `engine/src/db/index/vector_index/pq/pq.c` ↔ Faiss `faiss/impl/ProductQuantizer.cpp`：PQ 训练 + 编码思路一致（Jégou 2011 TPAMI），自研 8-bit PQ + 256 行实现；Faiss 有 FastScan（4-bit + SIMD LUT，5-10x）+ IVFPQFastScan + Resonant + 残差量化。
- 自研 `engine/src/db/index/vector_index/gpu/gpu_hnsw.c` ↔ Faiss `faiss/gpu/GpuIndexCagra.cu`：GPU HNSW 思路一致（NVIDIA cuVS），自研 426 行；Faiss CAGRA 算法（Ootomo 2023）比一般 GPU HNSW 加速更优（50-100x vs CPU HNSW）。
- 自研 `engine/src/db/index/vector_index/lvq/lvq.c` ↔ Faiss `faiss/impl/AdditiveQuantizer.cpp`：加性量化思路一致，自研 372 行；Faiss LVQ 完整 + IVFAQ + AQFastScan + AQ LSD 变种更全。
- 自研 `engine/src/db/index/vector_index/scann/scann.c` ↔ Faiss `faiss/impl/ResidualQuantizer*` + `faiss/impl/OPQ.h`：ScaNN（OPQ + PQ + 重排）思路一致（Guo 2020 ICML），自研 429 行入门级；Faiss 三段式 + Re-rank 阈值自适应完整。

---

## 7. 补齐项下月计划

> 每条按「动作 / 投入 / 学习资源（wiki 链接）/ 验收标准 / 优先级」给出。
> 至少 3 条 P0/P1，本节给出 5 条（P0 三条 + P1 两条）。

### 7.1 P0 — 启动 Filter-aware HNSW（Pre-filter + Graph Reorder）+ HybridSearch RRF 协议标准化 + Sparse Vector 倒排（约 4 周）

- **动作**：在 `engine/src/db/index/vector_index/faiss_hnsw/` 下新建 `faiss_hnsw_filter_aware.{c,h}`（Pre-filter + Graph Reorder，类似 Milvus v2.4+）+ `faiss_hnsw_bitset.{c,h}`（Bitset 携带标量索引 row IDs）；在 `engine/src/db/index/vector_index/hybrid_search.c` 实现 **RRF 融合协议**（`score = sum(1/(k+rank_i))`，`rank.rrf.rank_constant` 默认 60）+ **WeightRanker**（加权融合）；新建 `engine/src/db/index/vector_index/sparse/` 实现 **Sparse Vector 倒排**（BM25 + SPLADE 风格）。
- **投入**：约 6000-12000 行 C，1 人 3 周开发 + 1 周测试。
- **学习资源**：[milvus.md §8.7](../db-wiki/vector/milvus.md)（混合索引：HNSW + 标量过滤）+ [milvus.md §9.5](../db-wiki/vector/milvus.md)（混合检索 + RRF）+ [qdrant.md §8.3](../db-wiki/vector/qdrant.md)（HNSW + Payload 过滤三策略）+ [qdrant.md §8.6](../db-wiki/vector/qdrant.md)（稀疏向量）。
- **验收标准**：
  - `engine/test/db/index/vector_index/faiss_hnsw/filter_aware_test.cpp` ≥ 6 用例（pre/post/graph-reorder）通过；
  - `engine/test/db/index/vector_index/faiss_hnsw/bitset_test.cpp` ≥ 4 用例（merge/AND/OR/XOR）通过；
  - `engine/test/db/index/vector_index/rrf_test.cpp` ≥ 6 用例（多路融合 + rank_constant）通过；
  - HNSW 标量过滤场景下 recall ≥ 95%（vs 无过滤 baseline）；
  - 混合检索：BM25 top-10 + 向量 top-10 RRF 融合后 top-10 准确率 ≥ 90%；
  - Sparse Vector BM25/SPLADE 倒排支持 ≥ 4 用例。
- **优先级**：**P0**（决定 §4.2 与 Milvus v2.4+ / Qdrant v1.7+ HybridSearch 对标）。

### 7.2 P0 — 启动 Payload 一等公民 + HNSW + Payload 过滤三策略 + 自动策略选择 + TurboQuant + Binary 1-bit（约 5 周）

- **动作**：在 `engine/src/db/index/vector_index/` 下新建 `payload/` 目录实现 **PlainPayloadIndex**（integer/keyword/float/bool/datetime + Bitmap + Value）+ **StructPayloadIndex**（JSON 路径嵌套）+ **GeoIndex**（Bounding Box / H3）+ **TextIndex**（倒排 Posting List）；扩展 `faiss_hnsw_search_filtered.c` 实现 **HNSW + Payload 过滤三策略**（Payload-first / HNSW-iterative / Graph pre-filter 类 ACORN 2024 论文）+ **自动策略选择**（Cardinality-based / Sampling-based）；在 `engine/src/db/index/vector_index/pq/` 下新增 `turboquant.{c,h}`（TurboQuant Google 2024）+ `binary_1bit.{c,h}`（Binary 1-bit 二值化）。
- **投入**：约 8000-15000 行 C，1 人 4 周开发 + 1 周测试。
- **学习资源**：[qdrant.md §8.3](../db-wiki/vector/qdrant.md)（HNSW + Payload 过滤三策略）+ [qdrant.md §8.4](../db-wiki/vector/qdrant.md)（量化方案 TurboQuant + Binary）+ [qdrant.md §8.7](../db-wiki/vector/qdrant.md)（Payload 索引 6+ 种类型）+ [qdrant.md §16](../db-wiki/vector/qdrant.md)（ACORN + TurboQuant + ColBERT 论文）。
- **验收标准**：
  - `engine/test/db/index/vector_index/payload/index_test.cpp` ≥ 8 用例（integer/keyword/float/bool/datetime/geo/text/JSON path）通过；
  - `engine/test/db/index/vector_index/faiss_hnsw/filter_three_strategy_test.cpp` ≥ 6 用例（Payload-first / HNSW-iterative / Graph pre-filter + 自动选择）通过；
  - `engine/test/db/index/vector_index/quant/turboquant_test.cpp` ≥ 4 用例（训练 + 编码 + 检索）通过；
  - `engine/test/db/index/vector_index/quant/binary_1bit_test.cpp` ≥ 4 用例（1-bit + Hamming）通过；
  - Binary 1-bit 压缩比 = 1/32，recall@10 ≥ 80%（与 FP32 baseline）；
  - TurboQuant 压缩比 = 1/32，recall@10 ≥ 95%；
  - HNSW + Payload 过滤场景下 recall ≥ 95%（vs 无过滤 baseline）。
- **优先级**：**P0**（决定 §4.3 与 Qdrant 杀手锏 Payload 一等公民 + Milvus v2.4+ Filter-aware HNSW 对标）。

### 7.3 P0 — 启动协议层（HTTP REST + gRPC）+ 10+ 语言 SDK + Cluster Shard Replica + AutoTune + IndexFactory（约 6 周）

- **动作**：在 `engine/src/db/index/vector_index/` 下新建 `protocol/http_rest.{c,h}`（HTTP REST API 监听 6333 类似 Qdrant）+ `protocol/grpc_service.{c,h}`（gRPC 监听 6334）+ `protocol/json_dsl.{c,h}`（JSON DSL 解析）；新建 `cluster/shard.{c,h}`（Shard 分片）+ `cluster/replica.{c,h}`（Replica 副本）+ `cluster/raft.{c,h}`（Raft 共识）+ `cluster/cluster_state.{c,h}`（Cluster State 持久化）；新建 `tuning/autotune.{c,h}`（AutoTune `optimize_search_parameters`）+ `factory/index_factory.{c,h}`（IndexFactory 字符串构造 `IVF4096,PQ8`）；新建 `sdk/python/` + `sdk/java/` + `sdk/go/` + `sdk/nodejs/` + `sdk/cpp/` + `sdk/rust/` + `sdk/typescript/`（7 语言 SDK 模板）。
- **投入**：约 12000-18000 行 C（协议层 + 集群 + AutoTune + IndexFactory + SDK），1 人 5 周开发 + 1 周测试。
- **学习资源**：[milvus.md §9.1-9.2](../db-wiki/vector/milvus.md)（gRPC + HTTP 9091 + 10 语言 SDK）+ [milvus.md §十一](../db-wiki/vector/milvus.md)（复制与高可用 多副本 + Handoff + Resource Group）+ [qdrant.md §9.1-9.3](../db-wiki/vector/qdrant.md)（REST + gRPC + 10 语言 SDK + OpenAPI）+ [qdrant.md §十一](../db-wiki/vector/qdrant.md)（Raft 分布式 + Shard 分片 + Replica）+ [faiss.md §9.4](../db-wiki/vector/faiss.md)（AutoTune）+ [faiss.md §9.1-9.2](../db-wiki/vector/faiss.md)（Python/C++ 接口 + IndexFactory）。
- **验收标准**：
  - `curl -X POST 'http://localhost:6333/collections/test/points/search' -d '{"vector":[...],"limit":10,"filter":{"must":[{"key":"category","match":{"value":"book"}}]}}'` 端到端可用；
  - gRPC（6334）10+ 语言 SDK（Python/Java/Go/Node.js/C++/Rust/TS）通过；
  - Shard 分片（默认 5）+ Replica 副本（默认 1）+ Cluster State 持久化通过；
  - AutoTune `optimize_search_parameters(index, xq, xb)` 在 validation 集上自动选参（nprobe/efSearch 最优）；
  - IndexFactory 字符串 `IVF4096,PQ8` / `HNSW32,PQ8` / `DiskANN_50` / `SCANN` 端到端可解析；
  - 8+ 语言客户端 SDK（Java/Python/Go/.NET/Ruby/PHP/JS/Rust）；
  - Operator（K8s CRD）通过 Helm Chart 部署。
- **优先级**：**P0**（决定 §4.4 与 Milvus v2.5+ / Qdrant v1.x / Faiss 服务化对标）。

### 7.4 P1 — 启动计算-存储分离架构 + Streaming Node + TSO 全局时钟（约 5 周）

- **动作**：在 `engine/src/db/storage/vector/` 下新建 `object_storage/{c,h}`（S3/MinIO/OSS/GCS 适配层）+ `mq/{c,h}`（Pulsar/Kafka/RocketMQ 适配层）+ `metastore/{c,h}`（etcd/TiKV 适配层）+ `tso/{c,h}`（全局时钟服务 Snapshot Isolation）+ `streaming_node/{c,h}`（v2.5+ 流式摄入，毫秒级延迟）。
- **投入**：约 10000-15000 行 C，1 人 4 周开发 + 1 周测试。
- **学习资源**：[milvus.md §6](../db-wiki/vector/milvus.md)（整体架构 计算-存储分离）+ [milvus.md §7](../db-wiki/vector/milvus.md)（存储引擎 Binlog + MQ WAL + 对象存储）+ [milvus.md §十二](../db-wiki/vector/milvus.md)（扩展性 水平 + 冷热分层 Cold Tier）。
- **验收标准**：
  - `engine/test/db/storage/vector/object_storage_test.cpp` ≥ 4 用例（put/get/list/delete）通过（S3/MinIO/OSS 三种适配）；
  - `engine/test/db/storage/vector/mq_test.cpp` ≥ 4 用例（append/consume/commit）通过（Pulsar/Kafka 两种适配）；
  - TSO 全局时钟：单调递增 + 高可用（多 TSO 实例 RAFT 同步）+ Snapshot Isolation；
  - Streaming Node 流式摄入：写入到可查询延迟 ≤ 100ms（vs 传统秒级）；
  - Cold Tier（DiskANN + 对象存储自动迁移）通过；
  - 多并发连接（≥32）无 dead-lock、无内存泄漏。
- **优先级**：**P1**（决定 §4.1 与 Milvus v2.5+ 流式 + 云原生分布式对标）。

### 7.5 P1 — 启动 GPU CAGRA 极致加速 + FastScan 4-bit PQ + SIMD LUT + ScaNN 重排工程化（约 4 周）

- **动作**：在 `engine/src/db/index/vector_index/gpu/` 下新建 `gpu_cagra.{c,h}`（NVIDIA RAPIDS cuVS GPU HNSW 算法，Ootomo 2023 论文）；新建 `engine/src/db/index/vector_index/pq/pq_fastscan.{c,h}`（4-bit PQ + SIMD LUT LUT 查表加速 + AVX-512） + `pq_residual.{c,h}`（Residual 残差量化）+ `pq_opq.{c,h}`（OPQ 优化旋转矩阵）；扩展 `engine/src/db/index/vector_index/scann/scann.c` 实现 **ScaNN 三段式（OPQ + PQ + Residual Re-rank）+ Re-rank 阈值自适应**。
- **投入**：约 5000-10000 行 C，1 人 3 周开发 + 1 周测试。
- **学习资源**：[milvus.md §8.5](../db-wiki/vector/milvus.md)（CAGRA GPU 索引）+ [faiss.md §8.6](../db-wiki/vector/faiss.md)（GPU CAGRA）+ [faiss.md §8.7](../db-wiki/vector/faiss.md)（量化方案全景）+ [faiss.md §8.9](../db-wiki/vector/faiss.md)（FastScan 4-bit PQ + SIMD）+ [faiss.md §8.5](../db-wiki/vector/faiss.md)（ScaNN 风格实现）+ [faiss.md §8.8](../db-wiki/vector/faiss.md)（OPQ 优化）。
- **验收标准**：
  - `engine/test/db/index/vector_index/gpu/cagra_test.cpp` ≥ 4 用例（GPU HNSW + 千万级 5-50x 加速）通过；
  - `engine/test/db/index/vector_index/pq/fastscan_test.cpp` ≥ 4 用例（4-bit PQ + SIMD LUT + 距离查表）通过；
  - FastScan 距离计算在 AVX-512 SIMD 上比朴素 PQ 快 ≥ 5x；
  - `engine/test/db/index/vector_index/scann/scann_full_test.cpp` ≥ 4 用例（OPQ + PQ + Residual Re-rank + 自适应阈值）通过；
  - GPU CAGRA 召回率 ≥ 95%，QPS ≥ CPU HNSW 5x；
  - `engine/src/db/index/vector_index/pq/CMakeLists.txt` 通配新 .c 文件加入 `vector_index_pq` 静态库。
- **优先级**：**P1**（决定 §4.5 与 Milvus v2.2+ CAGRA + Faiss 1.7+ FastScan + Faiss 1.8 ScaNN 深度对标）。

---

## 8. 附录 A — 自研与 Milvus / Faiss / Qdrant 路径对照表

| 自研路径 | Milvus 对照路径 | Faiss 对照路径 | Qdrant 对照路径 | 简述 |
|----------|-----------------|----------------|-----------------|------|
| `engine/src/db/storage/vector/vector_engine.c` | `internal/proxy/task.go` + `internal/querynodev2/` | `faiss/Index.h`（基类） | `lib/segment/src/segment.rs` | 向量引擎主入口 |
| `engine/src/db/storage/vector/vector_page.c` + `vec_page.c` | `internal/storage/binlog.go` | N/A（库无页） | `lib/segment/src/payload_storage/` | 向量页抽象 |
| `engine/src/db/storage/vector/vector_segment.c` | `internal/core/src/segcore/segment_c.cpp`（Segment） | `faiss/IndexFlat.cpp`（Flat 段） | `lib/segment/src/segment.rs` | 向量段管理 |
| `engine/src/db/storage/vector/vector_wal.c` | `internal/distributed/streamingcoord/`（MQ WAL） | N/A（库无 WAL） | `lib/wal/`（每 Segment WAL） | 向量 WAL |
| `engine/src/db/storage/vector/vector_buf.c` | `internal/storage/MemFileManagerImpl.h`（mmap） | N/A | `lib/trififo/`（三层 FIFO） | 向量缓冲池 |
| `engine/src/db/storage/vector/vector_index_persist.c` | `internal/indexnode/`（索引构建 + 持久化） | `faiss/Index.cpp`（serialize） | `lib/segment/src/storage/vector_data.rs` | 索引持久化 |
| `engine/src/db/storage/vector/graph_dedup.c` | `internal/core/src/index/Utils.cpp`（Bitset） | `faiss/impl/hnsw.cpp`（visited HashSet） | `lib/segment/src/index/hnsw_index/visited_hash_set.rs` | 图去重 |
| `engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw_create.c` | `internal/core/thirdparty/knowhere/hnswlib/hnsw.cpp` | `faiss/IndexHNSW.cpp` | `lib/segment/src/index/hnsw_index/hnsw_builder.rs` | HNSW 构建 |
| `engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw_search.c` | `internal/core/thirdparty/knowhere/hnswlib/search_hnsw.cpp` | `faiss/impl/hnsw.cpp:search` | `lib/segment/src/index/hnsw_index/searches.rs` | HNSW 搜索 |
| `engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw_search_filtered.c` | `internal/core/src/index/Utils.cpp`（Filter-aware HNSW Pre-filter + Graph Reorder v2.4+） | N/A（无标量字段） | `lib/segment/src/index/hnsw_index/graph_layers.rs`（Graph pre-filter ACORN） | HNSW 过滤搜索 |
| `engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw_level.c` | `knowhere/hnswlib/hnsw.cpp`（层级） | `faiss/impl/hnsw.cpp:get_random_level` | `lib/segment/src/index/hnsw_index/graph_layers.rs` | HNSW 层切换 |
| `engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw_minimax_heap.c` | `knowhere/hnswlib/visited_list_pool.cpp` | `faiss/impl/hnsw.cpp:minimax_heap` | `lib/segment/src/index/hnsw_index/hnsw_iterator.rs` | C 实现的最小堆 |
| `engine/src/db/index/vector_index/dist_hnsw/dist_hnsw.c` | `internal/querycoordv2/`（Shard 调度） + `internal/datanode/` | `faiss/IVFlib.cpp`（IVF 分布式拆分） | `lib/collection/src/shards/local_shard.rs`（raft-rs） | 分布式 HNSW |
| `engine/src/db/index/vector_index/faiss_ivf_compat/faiss_ivf_core.c` | `internal/core/thirdparty/knowhere/faiss/IndexIVF.cpp` | `faiss/IndexIVF.cpp` | `lib/quantization/src/product.rs`（IVF 风格） | IVF 核心 |
| `engine/src/db/index/vector_index/faiss_ivf_compat/faiss_ivf_search.c` | `knowhere/faiss/IndexIVF.cpp:search` | `faiss/IndexIVF.cpp:search` | N/A（Qdrant 无 IVF） | IVF 搜索 |
| `engine/src/db/index/vector_index/faiss_ivf_compat/faiss_ivf_train.c` | `knowhere/faiss/index_factory.cpp` | `faiss/Clustering.cpp`（k-means） | N/A | IVF 训练（k-means） |
| `engine/src/db/index/vector_index/faiss_ivf_compat/faiss_ivf_add.c` | `knowhere/faiss/IndexIVF.cpp:add` | `faiss/IndexIVF.cpp:add` | N/A | IVF 添加 |
| `engine/src/db/index/vector_index/faiss_ivf_compat/faiss_ivf_inverted_lists.c` | `knowhere/faiss/InvertedLists.cpp` | `faiss/invlists/InvertedLists.cpp` | N/A | IVF 倒排表 |
| `engine/src/db/index/vector_index/ivf/ivf.c` | 同 faiss_ivf_compat | 同上 | N/A | IVF 简化版 |
| `engine/src/db/index/vector_index/ivf_flat/ivf_flat.c` | `knowhere/faiss/IndexIVFFlat.cpp` | `faiss/IndexIVFFlat.cpp` | N/A | IVF + Flat |
| `engine/src/db/index/vector_index/ivf_pq/ivf_pq.c` | `knowhere/faiss/IndexIVFPQ.cpp` | `faiss/IndexIVFPQ.cpp` | N/A | IVF + PQ |
| `engine/src/db/index/vector_index/ivf_hnsw/ivf_hnsw.c` + `faiss_ivf_hnsw_*.c` | `knowhere/faiss/Index2Layer.cpp` | `faiss/Index2Layer.cpp` | N/A | IVF + HNSW 二级索引 |
| `engine/src/db/index/vector_index/diskann/diskann_vamana.c` | `internal/core/thirdparty/knowhere/diskann/vamana.cpp` | `faiss/IndexDiskANN.cpp`（社区分支） | N/A（Qdrant 无 DiskANN） | DiskANN Vamana 图核心 |
| `engine/src/db/index/vector_index/diskann/diskann_graph.c` | `knowhere/diskann/graph.h` | `faiss/IndexDiskANN.cpp:build` | N/A | DiskANN 图操作 |
| `engine/src/db/index/vector_index/diskann/diskann_search.c` | `knowhere/diskann/search.cpp` | `faiss/IndexDiskANN.cpp:search` | N/A | DiskANN 搜索 |
| `engine/src/db/index/vector_index/diskann/diskann_insert.c` + `diskann_delete.c` + `diskann_merge.c` | `knowhere/diskann/index_build.cpp` | `faiss/IndexDiskANN.cpp:merge_from` | N/A | DiskANN 增删合并 |
| `engine/src/db/index/vector_index/diskann/diskann_pq.c` + `diskann_quantization.c` | `knowhere/quant/PQ.cpp` | `faiss/impl/ProductQuantizer.cpp` | N/A | DiskANN + PQ |
| `engine/src/db/index/vector_index/diskann/diskann_disk_io.c` | `knowhere/diskann/DiskANN.cpp`（SSD I/O） | `faiss/impl/io.h`（mmap） | N/A | DiskANN SSD I/O 抽象 |
| `engine/src/db/index/vector_index/diskann/diskann_spann.c` | N/A（Milvus 无 SPANN 变种） | N/A | N/A | SPANN 分区变种 |
| `engine/src/db/index/vector_index/diskann/diskann_partition.c` | N/A（Milvus Cold Tier 间接实现） | N/A | N/A | DiskANN Partition |
| `engine/src/db/index/vector_index/diskann/diskann_fresh.c` | `knowhere/diskann/`（增量更新） | N/A | N/A | DiskANN 增量更新 |
| `engine/src/db/index/vector_index/pq/pq.c` | `internal/core/thirdparty/knowhere/quant/PQ.cpp` | `faiss/impl/ProductQuantizer.cpp` | `lib/quantization/src/product.rs` | PQ 乘积量化 |
| `engine/src/db/index/vector_index/sq/sq.c` | `internal/core/thirdparty/knowhere/quant/SQ.cpp` | `faiss/impl/ScalarQuantizer.cpp` | `lib/quantization/src/scalar.rs` | SQ 标量量化 |
| `engine/src/db/index/vector_index/lvq/lvq.c` | `knowhere/quant/AQ.cpp` | `faiss/impl/AdditiveQuantizer.cpp` | N/A | LVQ 加性量化 |
| `engine/src/db/index/vector_index/bq/bq.c` | `knowhere/quant/Binary.cpp` | `faiss/IndexBinaryIVF.cpp` | `lib/quantization/src/binary.rs`（1-bit） | BQ 二值量化 |
| `engine/src/db/index/vector_index/opq/opq.c` | `knowhere/quant/OPQ.cpp` | `faiss/impl/OPQ.h` | N/A | OPQ 优化旋转 |
| `engine/src/db/index/vector_index/rq/rq.c` | `knowhere/quant/RQ.cpp` | `faiss/impl/ResidualQuantizer.cpp` | N/A | RQ 残差量化 |
| `engine/src/db/index/vector_index/itq/itq.c` | N/A（实验性） | `faiss/impl/ITQ.cpp`（实验性） | N/A | ITQ 迭代量化 |
| `engine/src/db/index/vector_index/spectral_hash/spectral_hash.c` | N/A（实验性） | `faiss/IndexEDEN.cpp`（哈希量化） | N/A | 谱哈希 |
| `engine/src/db/index/vector_index/hnsw_pq/hnsw_pq.c` | `knowhere/hnswlib/hnsw_pq.cpp` | `faiss/IndexHNSWPQ.cpp` | N/A | HNSW + PQ |
| `engine/src/db/index/vector_index/hnsw_sq/hnsw_sq.c` | `knowhere/hnswlib/hnsw_sq.cpp` | `faiss/IndexHNSWSQ.cpp` | N/A | HNSW + SQ |
| `engine/src/db/index/vector_index/scann/scann.c` | `knowhere/quant/ScaNN.cpp` | `faiss/impl/OPQ.h` + `faiss/impl/ResidualQuantizer.cpp` | N/A | ScaNN 风格（OPQ + PQ + 重排） |
| `engine/src/db/index/vector_index/milvus_ivf/` | N/A（Milvus 自身） | `faiss/IndexIVF*.cpp`（参考） | N/A | Milvus IVF 兼容层 |
| `engine/src/db/index/vector_index/nsw/nsw.c` | `knowhere/hnswlib/hnsw.cpp:NSW` | `faiss/IndexNSGFlat.cpp`（NSG） | N/A | NSW 朴素图索引 |
| `engine/src/db/index/vector_index/ssg/ssg.c` | N/A（实验性） | N/A | N/A | SSG 简化图 |
| `engine/src/db/index/vector_index/annoy/annoy.c` | N/A | N/A（Annoy 独立库） | N/A | Annoy Spotify |
| `engine/src/db/index/vector_index/ball_tree/ball_tree.c` | N/A（Milvus 无 Ball Tree） | `faiss/index_factory.cpp`（简化） | N/A | Ball Tree |
| `engine/src/db/index/vector_index/kd_tree/kd_tree.c` | N/A | `faiss/index_factory.cpp`（简化） | N/A | KD Tree |
| `engine/src/db/index/vector_index/lsh/lsh.c` | N/A（实验性） | `faiss/impl/lsh.cpp`（实验性） | N/A | LSH |
| `engine/src/db/index/vector_index/lsh_multiprobe/lsh_multiprobe.c` | N/A（实验性） | `faiss/impl/lsh.cpp`（MultiProbe） | N/A | LSH 多探针 |
| `engine/src/db/index/vector_index/gpu/gpu_hnsw.c` | `internal/core/thirdparty/knowhere/cagra/` | `faiss/gpu/GpuIndexCagra.cu`（CAGRA） | `lib/segment/src/index/hnsw_index/`（实验 GPU） | GPU HNSW |
| `engine/src/db/index/vector_index/gpu/gpu_ivf.c` | `knowhere/cagra/`（GPU IVF） | `faiss/gpu/GpuIndexIVF*` | N/A | GPU IVF |
| `engine/src/db/index/vector_index/gpu/gpu_ivf_pq.c` | `knowhere/cagra/`（GPU IVF + PQ） | `faiss/gpu/GpuIndexIVFPQ*` | N/A | GPU IVF + PQ |
| `engine/src/db/index/vector_index/gpu/simd.c` | `knowhere/simd/dispatch.h`（SSE/AVX/AVX-512） | `faiss/utils/distances.h`（SIMD） | `lib/spaces/`（SIMD） | SIMD 距离计算 |
| `engine/src/db/index/vector_index/hybrid_search.c` | `internal/core/src/query/SearchOnIndex.cpp`（HybridSearch RRF） | N/A（用户自行实现） | `lib/segment/src/index/sparse_index/` + `lib/bm25/` | 混合检索（向量 + BM25/Sparse） |
| `engine/src/db/index/vector_index/reranker/` | `internal/proxy/task.go`（WeightRanker + RRFRanker） | 用户自行实现 | `lib/segment/src/index/`（内置 Rerank） | 重排器 |
| `engine/src/db/index/vector_index/delete/delete.c` | `internal/datacoord/garbage_collector.go` | `faiss/IDSelector.cpp` | `lib/segment/src/id_tracker.rs`（Deleted Points） | 删除支持 |
| `engine/src/db/index/vector_index/persist/persist.c` | `internal/indexnode/`（索引构建 + 持久化） | `faiss/Index.cpp:write_index/read_index` | `lib/segment/src/storage/vector_data.rs` | 持久化（mmap + Serialize） |
| `engine/src/db/index/vector_index/streaming/streaming_index.c` | `internal/distributed/streamingcoord/`（StreamingCoord v2.5+） | N/A | `lib/wal/`（异步合并） | 流式索引 |
| `engine/src/db/index/vector_index/streaming/vector_write_buffer.c` | `internal/datanode/flow_graph_buffer.go` | N/A | `lib/trififo/` | 流式写入缓冲 |
| `engine/src/db/index/vector_index/streaming/concurrent_search.c` | `internal/querynodev2/`（并发检索） | `faiss/IndexShards`（多 GPU 分片） | `lib/segment/src/index/hnsw_index/`（只读并发） | 并发搜索 |
| `engine/src/db/index/vector_index/streaming/merge_scheduler.c` | `internal/datacoord/compaction_scheduler.go` | N/A | `lib/segment/src/segment_constructor.rs`（合并） | Merge 调度 |
| `engine/src/db/index/vector_index/streaming/incremental_quant_train.c` | `internal/indexnode/`（增量索引） | N/A | `lib/segment/src/index/hnsw_index/`（增量） | 增量量化训练 |
| `engine/src/db/index/vector_index/multimodal/multimodal.c` | `internal/core/thirdparty/knowhere/`（Multivector v2.4+ ColBERT） | N/A | `lib/segment/src/index/hnsw_index/`（Multivector v1.10+ MaxSim） | 多向量（Multivector / ColBERT） |
| `engine/src/db/index/vector_index/vector_index_selector.c` | `internal/indexcoord/index_engine.go` | `faiss/index_factory.cpp`（字符串构造） | `lib/collection/src/shards/local_shard.rs` | 索引选择器 |
| `engine/src/db/index/vector_index/tiered_index.c` | `internal/storage/cold_tier.go`（v2.5+ Cold Tier） | `faiss/impl/io.h`（mmap） | `lib/segment/src/storage/vector_data.rs`（mmap） | 分层索引 |
| `engine/src/db/access/vector_am.c` | `internal/core/src/index/IndexFactory.cpp` | `faiss/Index.h`（基类抽象） | `lib/collection/src/collection/manager.rs` | 向量访问方法（AM） |
| `engine/src/db/access/am_hnsw.c` | `internal/core/src/index/VecIndex.cpp`（HNSW AM） | `faiss/IndexHNSW.cpp` | `lib/segment/src/index/hnsw_index/hnsw.rs` | HNSW 访问方法 |
| `engine/src/db/access/am_btree.c` | `internal/indexnode/`（BTree AM） | N/A | N/A | BTree 访问方法 |
| `engine/src/db/access/amapi.c` | `internal/core/src/index/IndexFactory.cpp`（AM API） | `faiss/index_factory.h` | `lib/collection/src/` | AM API 总入口 |

## 9. 附录 B — 章节与 24 维度映射

| 报告章节 | 对应维度 |
|----------|----------|
| §1 自研现状 | 23、24 |
| §2.1 Milvus | 1-3、5-7、8（Index 13 种）、9-22、25 |
| §2.2 Faiss | 1-3、5-13、15-16、19-22、25 |
| §2.3 Qdrant | 1-3、5-22、25 |
| §3 矩阵 | 全部 24 维度 + 1 扩展维度（与本项目关联） |
| §4.1 计算-存储分离 + TSO | 6、14、17、19、20 |
| §4.2 HybridSearch RRF + Sparse | 10、12、17 |
| §4.3 Payload + 过滤三策略 + TurboQuant | 9、17、18 |
| §4.4 协议层 + 集群 + AutoTune | 6、15、16、21、22 |
| §4.5 GPU + FastScan + ScaNN 重排 | 8、11、13 |
| §5.1 30+ 子目录覆盖 + 量化 13 种 | 8、11 |
| §5.2 DiskANN Vamana + SPANN + Partition | 8、24 |
| §5.3 多模型复用 + 向量段 + 图去重 | 7、23 |
| §6 学习路径 | 22 |
| §7.1 Filter-aware HNSW + RRF | 10、12、17 |
| §7.2 Payload + 过滤三策略 + TurboQuant | 9、11、18 |
| §7.3 协议层 + 集群 + AutoTune | 6、15、16、21、22 |
| §7.4 计算-存储分离 + Streaming + TSO | 6、17、19、20 |
| §7.5 GPU CAGRA + FastScan + ScaNN | 8、11、13 |
| §8 路径对照 | 23、24 |
| §9 本附录 | 全部维度 |

## 10. 元信息

- 报告作者：subagent-driven-development（gap-analysis Task 9 implementer）
- 关联文档：
  - `archive/.superpowers/sdd/gap-task-1-inventory.md` §7 vector（行 60 索引 + 附录）
  - `docs/process/plans/2026-10-16-gap-analysis-implementation.md` Task 9
  - `docs/analysis/2026-08-29-multimodal-db-gap-analysis.md`（旧版顶层分析，本篇不替代，仅纵向对照）
  - `docs/analysis/relational.md`（姊妹篇，relational 子报告）
  - `docs/analysis/key-value.md`（姊妹篇，key-value 子报告）
  - `docs/analysis/document.md`（姊妹篇，document 子报告）
  - `docs/analysis/columnar.md`（姊妹篇，columnar 子报告）
  - `docs/analysis/time-series.md`（姊妹篇，time-series 子报告）
  - `docs/analysis/search.md`（姊妹篇，search 子报告）
  - `docs/analysis/vector.md`（本报告，vector 子报告，13 篇之七旗舰篇）
- 行数目标：500-600（本篇为旗舰篇可放宽到 500-700，实测见 §11 自检）
- 版本：v1（首稿，无迭代历史）

## 11. 自检（提交前 5 条验收）

- [ ] **24 维度对照矩阵完整**：YES（§3 已列 25 行 24 维度主表 + 1 扩展维度「与本项目关联」，含自研/Milvus/Faiss/Qdrant/等级五列）
- [ ] **Top-5 差距每条有具体代码路径/wiki 引用**：YES（§4 五条均含 `engine/src/db/storage/vector/` + `engine/src/db/index/vector_index/` 自研路径 + Milvus/Faiss/Qdrant wiki 路径）
- [ ] **补齐项下月计划至少 3 条 P0/P1**：YES（§7.1 P0、§7.2 P0、§7.3 P0 + §7.4 P1、§7.5 P1，共 3 条 P0 + 2 条 P1 + 0 条 P2）
- [ ] **至少 5 处 engine/ 真实路径引用**：YES（§1 列 47 条路径、§4 列 5 条、§7 引用 23 条、§8 表格 73 条，**总计 ≥ 148 处真实 engineering 路径引用**）
- [ ] **至少 2 个 wiki 链接引用**：YES（§6 列出 6 个 wiki 章节链接 + §2 与 §7 多处内联 wiki 相对路径，**总计 ≥ 18 处 wiki 链接引用**）
- [ ] **目标行数 500-700**：实测见下表

**专项自检**（plan §Task 9 Step 4 特别要求）：

- [ ] **24 维矩阵的「7 存储引擎 / 8 索引机制 / 13 性能基准」三行有具体量化/HNSW/DiskANN 内容**：YES（维度 7 写「向量页 + 段 + WAL + 缓冲池 + 图去重 + INDEX_PERSIST」+ Milvus「Binlog + StorageV2 + MQ WAL + TSO」+ Faiss「纯内存」+ Qdrant「Segment + MDB + WAL + Trififo」+ 自研等级 A-；维度 8 写「HNSW（faiss_hnsw 17 文件 + dist_hnsw）+ DiskANN（18 文件 Vamana 图）+ IVF（4 子变体）+ ScaNN + 多向量 + 流式 + ANN 树」+ Milvus「Knowhere 13 种」+ Faiss「15+ 类索引」+ Qdrant「HNSW + Payload 三类型」+ 自研等级 A；维度 13 写「GPU 加速 5-50x + PQ 13 种 + DiskANN 十亿级 / SATA」+ Milvus「ann-benchmarks + HNSW 95%+ recall + DiskANN 十亿级 + CAGRA 5-50x」+ Faiss「5-10 万 QPS + GPU CAGRA 50-100 万 QPS + FastScan 5-10x」+ Qdrant「1-5ms / query + Scalar 4x + PQ 16x + Binary 32x」+ 自研等级 C）
- [ ] **Top-5 差距 2-3 条与量化/DiskANN/GPU 相关**：YES（§4.3 Payload + 过滤三策略 + TurboQuant + Binary 1-bit 强相关 + §4.5 GPU CAGRA + FastScan + ScaNN 重排工程化强相关 = 2 条直接相关；§4.4 AutoTune 间接相关 FastScan；§4.1 间接相关）
- [ ] **量化章节覆盖 PQ/LVQ/SQ/RaBitQ**：YES（§1.6 列 13 种量化（PQ/SQ/LVQ/BQ/OPQ/RQ/ITQ/SpectralHash + 5 种组合 HNSW_PQ/SQ/IVF_PQ/DiskANN_PQ/DiskANN_Quantization）；§2.1 写「PQ + SQ + PRQ + RaBitQ + Binary」+ §2.2 写「PQ + SQ + AQ/LVQ + RQ + PRQ + OPQ + FastScan + LSQ + QAT + Binary」+ §2.3 写「Scalar int8/int4 + PQ + Binary 1-bit + TurboQuant + Reranker」+ §3 维度 11 行「PQ + SQ + LVQ + BQ + OPQ + RQ + ITQ + SpectralHash + HNSW_PQ + HNSW_SQ + IVF_PQ + DiskANN_PQ + DiskANN_Quantization = 13 种」+ §4.5 标「TurboQuant + Binary 1-bit」+ §7.5 标 PQ FastScan / Residual / OPQ + §8 路径对照 14 行量化路径）

行数实测（章节）：

- §1 自研现状：~95 行
- §2 业界主流：~85 行
- §3 24 维度对照矩阵：~50 行
- §4 Top-5 差距：~85 行
- §5 优势 Top-3：~25 行
- §6 学习路径：~30 行
- §7 补齐项下月计划：~95 行
- §8-§11 附录+元信息+自检：~80 行
- **合计：~545 行 markdown**（在 500-700 行目标区间内）

---

> 注意：本报告是基于 2026-10-16 自研现状的静态差距分析；后续 mini-vecdb 端到端 plan 或 Phase 9 分布式演进完成时需重读并刷新 §1、§3、§4 与 §7。**作为旗舰篇特别提示**：本篇是 13 子报告中**自研覆盖率最强**的方向（30+ 子目录 + 13 种量化），未来 Phase C「亮剑」时建议将 Top-5 §4.1 + §4.3 + §4.4 作为对外宣传的核心故事——**「量化种类业内最全 + 计算-存储分离 + Payload 一等公民」** 将是 RAG / 推荐 / 多模态检索场景下最有竞争力的差异化亮点。
