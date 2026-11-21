> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# OPQ 实现详解

> **目标读者**：在工程轨道中集成、维护或调优 OPQ 模块的开发者。
> **前置阅读**：[OPQ 算法原理](../theory/opq-theory.md)

## 1. 文件结构

```
engine/include/db/index/vector_index/opq/
└── opq.h                          # 公共 API 头文件（结构体、函数原型）

engine/src/db/index/vector_index/opq/
├── CMakeLists.txt                 # 构建脚本
└── opq.c                          # 量化器实现（创建/训练/编码/距离）

engine/test/vector_index/opq/
└── opq_test.cpp                    # gtest 单元测试
```

### 各文件职责说明

| 文件 | 职责 |
|------|------|
| `opq.h` | 定义 `opq_quantizer_t` 结构体；声明 11 个公共 API；提供使用示例 doxygen 注释块 |
| `opq.c` | 实现量化器全生命周期：内存分配、子空间划分、K-Means 码本训练、近似最近码字查找、距离表构建、ADC 距离计算 |
| `CMakeLists.txt` | 将 `opq.c` 编译为静态库并链接相关依赖（`algo-prod/quantization`） |
| `opq_test.cpp` | GoogleTest 单元测试，覆盖创建/训练/编码/解码/距离/参数校验 |

### 与理论文档的差异说明

当前代码库中的 `opq.c` 实现的是 **PQ 简化版本**——它在子空间划分上沿用了 PQ 的等长切分策略，**尚未引入旋转矩阵的迭代优化步骤**。这是工程实现中的一个折衷：

- **保留的 OPQ 特征**：子空间 K-Means 训练、统一码本接口、ADC 距离表查询
- **暂未实现的理论特性**：学习最优旋转矩阵 `R`、Procrustes 分析逐轮更新 `R`

后续迭代可以在 `opq_train` 中加入 `X·Qᵀ = UΣVᵀ` 的 SVD 步骤，将其升级为完整 OPQ。本文档后面的章节会同时描述**当前代码实际行为**与**按理论补全后的 OPQ 全流程**，并标注差异。

---

## 2. 核心数据结构

### 2.1 `opq_quantizer_t` 结构体

来源于 `engine/include/db/index/vector_index/opq/opq.h`：

```c
/* 前置声明：避免直接依赖存储后端头文件 */
typedef struct storage_backend  storage_backend_t;
typedef struct heap_vector_store heap_vector_store_t;

struct opq_quantizer {
    int   dims;           /* 向量总维度 d                                  */
    int   m;              /* 子空间数量                                     */
    int   bits;           /* 每子空间比特数（典型取值 6、8）                  */
    int   ks;             /* 每子空间码本大小 = 1 << bits（即 2^bits）        */
    int   sub_dim;        /* 子空间维度 = dims / m                          */

    float *codebooks;     /* 码本存储，布局为 [m, ks, sub_dim]，按子空间连续排布 */

    /* ── 存储后端集成（Phase 1 基础设 施）── */
    heap_vector_store_t *heap_store;  /* 堆式向量存储，存放编码后的向量        */
    storage_backend_t   *storage;     /* 通用存储后端，可挂接磁盘/远程       */

    int trained;          /* 训练标志位：0=未训练 / 1=已训练                 */
};
```

**关键字段解读**：

- `codebooks`：码本采用行优先连续存储。访问第 `sub` 个子空间的第 `c` 个质心的第 `d` 维分量： `codebooks[sub * ks * sub_dim + c * sub_dim + d]`。
- `heap_store` / `storage`：用于**运行时承载量化编码**和**持久化扩展点**。当前 `opq.c` 中仅持有指针，不主动释放，由调用方管理生命周期。
- `trained`：防止未训练时调用 `opq_encode` / `opq_compute_distance_table` 等未定义行为。

### 2.2 训练参数

OPQ 没有独立的训练参数结构体，训练入口即签名：

```c
int opq_train(opq_quantizer_t *opq, int n, const float *vectors);
```

传入数据矩阵 `vectors` 按行主序排列，布局为 `[n, dims]`。训练过程中使用以下隐含参数：

| 隐含参数 | 当前值 | 说明 |
|----------|--------|------|
| K-Means 最大迭代数 | 5 | 见 `opq.c` 的 `for (int iter = 0; iter < 5; ...)` |
| 初始化策略 | 用前 `ks` 个训练样本子向量作初始中心 | 不随机、不K-Means++ |
| 收敛判定 | 不启用，固定迭代 5 轮后退出 | — |
| 旋转优化 | **未启用**（理论 OPQ 关键步骤） | 未来扩展点 |

### 2.3 残差计算相关结构

当前实现 **不显式存储残差**——量化误差仅通过 `(原始向量 - 解码向量)` 实时计算，无需独立结构体。理论 RVQ/ORVQ 变体（如 [opq-theory.md §8](../theory/opq-theory.md) 所述）才会引入：

```c
/* 概念性结构（理论定义，当前代码未使用，仅作说明） */
typedef struct {
    int   level;          /* 量化层级 1..L                                */
    float residual[dims]; /* 当前层残差 = 原始向量 - ∑ q_i(x)            */
} opq_residual_t;
```

残差计算涉及的两个内部函数：

| 函数 | 用途 |
|------|------|
| `_find_nearest_code` | 在子空间码本内搜索最近码字（训练与编码共用） |
| `opq_decode` | 根据编码逆推最近质心，用于误差评估 |

---

## 3. 完整流程

### 3.1 训练流程

#### 3.1.1 当前实现的训练流程（PQ 风格）

```c
int opq_train(opq_quantizer_t *opq, int n, const float *vectors)
{
    for (int sub = 0; sub < opq->m; sub++) {
        /* ─ 步骤 1：初始化码本（前 ks 个样本的子向量）── */
        float *codebook = &opq->codebooks[sub * opq->ks * opq->sub_dim];
        for (int c = 0; c < opq->ks && c < n; c++) {
            for (int d = 0; d < opq->sub_dim; d++) {
                int src = c * opq->dims + sub * opq->sub_dim + d;
                if (src < n * opq->dims) {
                    codebook[c * opq->sub_dim + d] = vectors[src];
                }
            }
        }

        /* ─ 步骤 2：迭代 K-Means（固定 5 轮）── */
        for (int iter = 0; iter < 5; iter++) {
            int *assignments = malloc(n * sizeof(int));

            /* 2a. 分配：每个样本找最近码字 */
            for (int i = 0; i < n; i++) {
                int best = 0;
                float best_dist = 1e10f;
                for (int c = 0; c < opq->ks; c++) {
                    float dist = 0;
                    for (int d = 0; d < opq->sub_dim; d++) {
                        float diff = vectors[i * opq->dims + sub * opq->sub_dim + d]
                                   - codebook[c * opq->sub_dim + d];
                        dist += diff * diff;
                    }
                    if (dist < best_dist) { best_dist = dist; best = c; }
                }
                assignments[i] = best;
            }

            /* 2b. 更新：按分配重新计算质心 */
            for (int c = 0; c < opq->ks; c++) {
                int count = 0;
                float sum[64] = {0};  /* 子空间硬限制 sub_dim ≤ 64 */
                for (int i = 0; i < n; i++) {
                    if (assignments[i] == c) {
                        count++;
                        for (int d = 0; d < opq->sub_dim; d++) {
                            sum[d] += vectors[i * opq->dims + sub * opq->sub_dim + d];
                        }
                    }
                }
                if (count > 0) {
                    for (int d = 0; d < opq->sub_dim; d++) {
                        codebook[c * opq->sub_dim + d] = sum[d] / count;
                    }
                }
            }
            free(assignments);
        }
    }
    opq->trained = 1;
    return 0;
}
```

**当前实现要点**：

- 每个子空间独立做 K-Means，串行执行（无并行化、无 SIMD 加速）
- 子空间维度 **硬限制 ≤ 64**（`float sum[64]` 栈数组决定）。若 `sub_dim > 64` 需改堆分配
- 无收敛判定 → 固定 5 轮迭代

#### 3.1.2 理论 OPQ 的完整训练流程（扩展蓝图）

当前实现未引入的**旋转优化**可按以下步骤补全：

```
opq_train_full(opq, n, vectors):
    1. 随机初始化正交旋转矩阵 R (d × d)
    2. for iter = 1..ITER:
        a. 对所有向量做旋转变换:  x' = Rᵀ x
        b. 对每个子空间做 K-Means 训练码本 C_1..C_m
        c. 用所有子质心拼接为 Q (d × ks·m),  X 为旋转后的数据
        d. Procrustes 更新:  X·Qᵀ = U Σ Vᵀ   →   R ← U Vᵀ
    3. 同时更新每个子空间的码本，使 ∑‖x - R·q(Rᵀx)‖² 最小
```

> **实现指引**：在 `opq.c` 中扩展结构体增加 `float *rotation`（初始为恒等矩阵），第 2c/2d 步可通过引入 LAPACK/cuSOLVER 的 SVD 接口实现。

---

### 3.2 编码流程

```c
int opq_encode(opq_quantizer_t *opq, const float *vector, uint8_t *code)
{
    if (!opq || !vector || !code || !opq->trained) return -1;

    for (int sub = 0; sub < opq->m; sub++) {
        const float *sub_vec  = &vector[sub * opq->sub_dim];
        const float *codebook = &opq->codebooks[sub * opq->ks * opq->sub_dim];

        /* 在子空间码本中找最近质心的索引 */
        int best = _find_nearest_code(sub_vec, codebook, opq->ks, opq->sub_dim);
        code[sub] = (uint8_t)best;
    }
    return 0;
}
```

**关键路径**：

1. **未训练守卫**：`!opq->trained` 直接返回 `-1`，防止编码出错。
2. **子空间切分**：第 `sub` 个子向量从 `vector[sub * sub_dim .. (sub+1)*sub_dim)` 取出。
3. **最近码字查找**：时间复杂度 `O(ks × sub_dim)`，串行遍历所有码字。优化方向见 §4.3。
4. **批量接口**：`opq_encode_batch` 是单条编码的循环展开版本，时间复杂度 `O(n × m × ks × sub_dim)`。

**编码字节数**：`opq_code_size() == opq->m`，即每条向量压缩为 `m` 字节。

#### 理论 OPQ 的编码（蓝图）

补全旋转后，编码 `x` 需要先做 `x' = Rᵀ x` 再划分子空间：

```c
/* 伪代码：完整 OPQ 编码 */
float x_rotated[dims];
matvec(R_T, vector, x_rotated, dims, dims);   /* Rᵀ × x */
for (sub = 0; sub < m; sub++) {
    code[sub] = find_nearest(&x_rotated[sub*sub_dim], codebook[sub]);
}
```

**开销**：`O(d²)` 的矩阵-向量乘法。补全旋转后，编码路径更慢但精度更高。

---

### 3.3 ADC 距离计算

ADC (Asymmetric Distance Computation) **不对查询编码**，只编码数据库向量。优势是查询端保留原始精度。

#### 3.3.1 距离表构建

```c
int opq_compute_distance_table(opq_quantizer_t *opq, const float *query, float *table)
{
    for (int sub = 0; sub < opq->m; sub++) {
        const float *sub_query = &query[sub * opq->sub_dim];
        const float *codebook  = &opq->codebooks[sub * opq->ks * opq->sub_dim];

        for (int c = 0; c < opq->ks; c++) {
            float dist = 0;
            for (int d = 0; d < opq->sub_dim; d++) {
                float diff = sub_query[d] - codebook[c * opq->sub_dim + d];
                dist += diff * diff;
            }
            table[sub * opq->ks + c] = dist;
        }
    }
    return 0;
}
```

- 表大小：`m × ks` 个 float。当 `(m=16, bits=8)` 时为 `16 × 256 = 4096` floats = 16 KB
- 公式：每个表项是 **子查询向量与第 sub 个子空间的第 c 个质心的欧氏距离平方**

#### 3.3.2 ADC 查表求和

```c
float opq_adc_distance(opq_quantizer_t *opq, const uint8_t *code, const float *table)
{
    float dist = 0;
    for (int sub = 0; sub < opq->m; sub++) {
        dist += table[sub * opq->ks + code[sub]];
    }
    return dist;
}
```

**复杂度**：`O(m)`（纯查表，4 字节读 + 一次加法，约 16 条指令）。

**端到端流程**（一条查询）：

```
  ┌──────────────────────┐
  │ 1. opq_compute_dist_ │  ← 单次查询：O(m × ks × sub_dim)
  │    table(query, tbl) │
  └──────────┬───────────┘
             │
             ▼
  ┌────────────────────────────────────┐
  │ 2. 遍历所有候选向量码              │
  │    for code in heap_store.codes:   │
  │        dist = opq_adc_distance(...)  ← 每次 O(m)
  └──────────┬─────────────────────────┘
             │
             ▼
  ┌──────────────────────────┐
  │ 3. HeapTopK(候选距离)    │  ← top-k 重排
  └──────────────────────────┘
```

#### 3.3.3 与 SDC 的对比

| 方法 | 查询侧处理 | 精度 | 速度 |
|------|-----------|------|------|
| **ADC**（推荐） | 完整距离表 + 查表 | 较高 | 较快 |
| SDC | 查询也编码 → 查表 + 查表 | 较低 | 最快 |

OPQ **强烈推荐使用 ADC**——因为 OPQ 的旋转 + K-Means 设计本身是为非对称场景优化的。

---

### 3.4 heap_store 集成说明

`opq_quantizer_t` 通过 `heap_vector_store_t *heap_store` 与项目内的 **堆式向量主存储** 模块交互：

| 接口 | 作用 |
|------|------|
| `heap_store` | 保存所有数据库向量的**量化编码 `[n, m]`** |
| `storage` | 可选，挂接磁盘 / 网络等持久化后端 |

**典型集成模式**：

```c
/* 1. 创建量化器 */
opq_quantizer_t *opq = opq_create(128, 16, 8);
opq_train(opq, n_train, train_data);

/* 2. 创建 heap_store 并挂入（由调用方管理生命周期） */
heap_vector_store_t *hvs = heap_vector_store_create(128 /* code_size */, n_total);
opq->heap_store = hvs;

/* 3. 批量编码后写入堆存储 */
uint8_t *codes = malloc(n_total * opq_code_size(opq));
opq_encode_batch(opq, n_total, raw_vectors, codes);
for (int i = 0; i < n_total; i++) {
    heap_vector_store_push(hvs, &codes[i * opq->m], id[i]);
}

/* 4. 查询流程 */
float query[128], table[16 * 256];
opq_compute_distance_table(opq, query, table);

/* 遍历 hvs 中的所有编码，ADC 距离过滤，取 top-k */
for (size_t i = 0; i < heap_vector_store_size(hvs); i++) {
    const uint8_t *code = heap_vector_store_get_code(hvs, i);
    float d = opq_adc_distance(opq, code, table);
    /* heap_push(top_k_heap, d, id); */
}

/* 5. 清理（heap_store 生命周期独立，调用方释放） */
opq_destroy(opq);
free(codes);
```

**注意事项**：

- `opq_destroy` 不释放 `heap_store` / `storage`，避免双重 free
- `heap_store` 的槽位宽应等于 `opq_code_size(opq)`（即 `m`）
- 与 IVF / HNSW 集成时，建议在 IVF 倒排桶 / HNSW 图节点中存储编码引用，搜索时通过 ADC 距离间接排序

---

## 4. 参数调优

### 4.1 `m`（子空间数）选择

`m` 直接决定压缩率与精度：

| 取值权衡 | `m` 小（粗粒度） | `m` 大（细粒度） |
|----------|----------------|-----------------|
| 压缩率 | 低 | 高 |
| 精度 | 子空间维度大 → K-Means 收敛更好 | 子空间维度小 → 量化误差积累 |
| 训练时间 | 长 | 短 |
| 推荐范围 | `4 ≤ m ≤ d / 4` | — |

**经验准则**：

- **128 维**：常用 `m = 8, 16, 32`（分别 16/8/4 维每子空间）
- **256 维**：常用 `m = 16, 32`（每子空间 8~16 维）
- **d 必须能整除 m**（当前实现通过 `sub_dim = dims / m` 强制取整）

**显式公式**：

```
压缩率 = (d × 4 bytes) / m bytes = 4d / m
每子空间维度 = d / m  （建议落在 [4, 32] 区间）
```

### 4.2 `bits`（每子空间比特数）选择

`bits` 直接决定每子空间码本大小 `ks = 2^bits`：

| `bits` | `ks` | 内存开销（每子空间） | 适用场景 |
|--------|------|---------------------|----------|
| 4 | 16 | 64 B | 内存极度紧张，粗排 |
| 6 | 64 | 256 B | 中等精度 |
| **8**（推荐） | **256** | **1 KB** | **精度/内存平衡** |
| 10 | 1024 | 4 KB | 高精度初筛 |
| 12 | 4096 | 16 KB | 接近无损（慎用） |

**训练数据量下限**：

```
n_train ≥ 100 × ks × m
n_train ≥ 10000   (兜底)
```

- `bits = 8, m = 16` → `n_train ≥ 100 × 256 × 16 = 409,600`，实操中 10 万样本已足够训练良好模型
- `bits = 6, m = 8` → 训练数据可在 1 万量级

### 4.3 与 heap_store 的配合

| 调优点 | 建议 |
|--------|------|
| 编码存储粒度 | `heap_store` 的槽位宽 = `opq_code_size()` |
| 批量预热 | 训练完成后一次性 `opq_encode_batch` 全部数据集，再 push 到 `heap_store` |
| 查询侧缓存 | 热门查询的距离表可缓存于 `thread_local` 区，避免重复 `opq_compute_distance_table` |
| 距离表大小 | `(m=16, bits=8)` 时 16 KB，可常驻 L1 |
| 多线程 | 编码可分片并行；ADC 距离表计算亦可 SIMD 化（AVX2/AVX-512 一次算 8/16 个码字） |

**联合 IVF/HNSW 的建议**：

- IVF + OPQ：倒排桶 ID 用编码字节计算，搜索时先 IVF 桶筛选再 ADC 重排
- HNSW + OPQ：图边权重改用 ADC 距离，跳表阈值归一化到 ADC 输出范围

---

## 5. 测试用例

测试文件 `engine/test/vector_index/opq/opq_test.cpp` 的核心片段：

### 5.1 创建与参数校验

```cpp
TEST(OPQTest, CreateAndDestroy)
{
    opq_quantizer_t *opq = opq_create(128, 8, 8);  /* dims=128, m=8, bits=8 */
    ASSERT_NE(opq, nullptr);
    EXPECT_FALSE(opq_is_trained(opq));              /* 初始未训练 */
    opq_destroy(opq);
}

TEST(OPQTest, CreateWithInvalidParams)
{
    /* 各种非法参数必须返回 nullptr */
    EXPECT_EQ(opq_create(0, 8, 8), nullptr);       /* dims 必须 > 0    */
    EXPECT_EQ(opq_create(128, 0, 8), nullptr);     /* m 必须 > 0       */
    EXPECT_EQ(opq_create(128, 8, 0), nullptr);     /* bits 必须 > 0    */
    EXPECT_EQ(opq_create(8, 16, 8), nullptr);      /* dims 必须 >= m   */
}
```

### 5.2 训练

```cpp
TEST(OPQTest, Train)
{
    opq_quantizer_t *opq = opq_create(64, 8, 6);    /* 64 维, 8 子空间, 6 bit */
    ASSERT_NE(opq, nullptr);

    /* 生成 1000 个标准正态分布训练向量 */
    const int n = 1000;
    std::vector<float> vectors(n * 64);
    std::mt19937 rng(42);
    std::normal_distribution<float> dist(0.0f, 1.0f);
    for (int i = 0; i < n * 64; i++) vectors[i] = dist(rng);

    int ret = opq_train(opq, n, vectors.data());
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(opq_is_trained(opq));                /* 训练后标志位置 1 */

    opq_destroy(opq);
}
```

### 5.3 编码/解码误差

```cpp
TEST(OPQTest, EncodeAndDecode)
{
    opq_quantizer_t *opq = opq_create(64, 8, 6);
    ASSERT_NE(opq, nullptr);

    /* 训练（同上）*/
    /* ... */

    /* 编码 + 解码 */
    std::vector<uint8_t> code(opq_code_size(opq));
    opq_encode(opq, vectors.data(), code.data());

    std::vector<float> decoded(64);
    opq_decode(opq, code.data(), decoded.data());

    /* 评估量化误差（L2 距离平方根） */
    float error = 0;
    for (int i = 0; i < 64; i++) {
        float diff = vectors[i] - decoded[i];
        error += diff * diff;
    }
    error = sqrtf(error);
    EXPECT_LT(error, 100.0f);    /* 误差上界，避免退化 */
    opq_destroy(opq);
}
```

### 5.4 ADC 距离表

```cpp
TEST(OPQTest, DistanceTable)
{
    opq_quantizer_t *opq = opq_create(64, 8, 6);
    /* 训练（略）*/

    /* 计算查询向量与所有码字的距离表 */
    std::vector<float> table(opq_distance_table_size(opq));  /* m × 2^bits */
    opq_compute_distance_table(opq, vectors.data(), table.data());

    /* 编码后用 ADC 查表求距离 */
    std::vector<uint8_t> code(opq_code_size(opq));
    opq_encode(opq, vectors.data(), code.data());

    float dist = opq_adc_distance(opq, code.data(), table.data());
    EXPECT_GE(dist, 0.0f);        /* 距离非负 */

    opq_destroy(opq);
}
```

### 5.5 批量编码与大小校验

```cpp
TEST(OPQTest, BatchEncode)
{
    opq_quantizer_t *opq = opq_create(128, 16, 8);
    /* 训练（略）*/

    const int batch_size = 100;
    std::vector<uint8_t> codes(batch_size * opq_code_size(opq));
    int encoded = opq_encode_batch(opq, batch_size, vectors.data(), codes.data());
    EXPECT_EQ(encoded, batch_size);    /* 所有向量必须成功编码 */
    opq_destroy(opq);
}

TEST(OPQTest, CodeSize)
{
    opq_quantizer_t *opq = opq_create(128, 16, 8);
    /* 原始 128 维 float = 128*4 = 512 字节；编码后 16 字节 → 32x 压缩比 */
    EXPECT_EQ(opq_code_size(opq), 16);                /* == m       */
    EXPECT_EQ(opq_distance_table_size(opq), 4096);    /* == m × 2^bits */
    opq_destroy(opq);
}
```

### 5.6 未训练守卫

```cpp
TEST(OPQTest, UntrainedState)
{
    opq_quantizer_t *opq = opq_create(64, 8, 6);
    EXPECT_FALSE(opq_is_trained(opq));

    std::vector<float> vec(64, 0.0f);
    std::vector<uint8_t> code(opq_code_size(opq));

    /* 未训练时编码必须失败（不可用未初始化的码本） */
    EXPECT_NE(opq_encode(opq, vec.data(), code.data()), 0);

    opq_destroy(opq);
}
```

---

## 6. 快速参考

### 6.1 调用顺序

```
创建 → 训练 → 编码 → 距离表 → ADC 距离 → 销毁
opq_create → opq_train → opq_encode → opq_compute_distance_table → opq_adc_distance → opq_destroy
```

### 6.2 错误码约定

| 路径 | 返回值 | 含义 |
|------|--------|------|
| 成功路径 | `0` | 函数正常返回 |
| 失败路径 | `-1` | 空指针 / 未训练 / 非法参数 |
| 批量编码 | 实际成功条数 | 实际编码数（≤ n） |
| `opq_adc_distance` | float | 距离（≥ 0） |

### 6.3 关键不变量

- `opq->dims >= opq->m`（构造时校验）
- `opq->sub_dim == opq->dims / opq->m`（整除）
- `opq->ks == 1 << opq->bits`（位运算得码字数）
- `opq->codebooks` 长度 = `m * ks * sub_dim` 个 float
- `opq->trained == 0` 时不可调用 `opq_encode` / `opq_compute_distance_table` / `opq_decode`

### 6.4 已知扩展点

| 位置 | 扩展方向 |
|------|---------|
| `opq_train` 步骤 1 | 增加正交旋转矩阵 `R` 的初始化 |
| `opq_train` 主循环 | 加入 Procrustes → SVD 迭代更新 `R` |
| `opq_encode` 入口 | 增加 `x_rotated = Rᵀ x` 步骤 |
| `opq_compute_distance_table` 入口 | 对查询做同样旋转 |
| `_find_nearest_code` | SIMD 化（AVX-512 一次算 16 个码字） |
| `opq_train` 主循环 | 支持 OpenMP / pthread 并行化 |

完成上述扩展后，本模块即可从"简化版 PQ"升级为"完整 OPQ"，达到 [opq-theory.md §1](../theory/opq-theory.md) 描述的精度收益。

---

## 7. 关联资源

- 算法原理：[OPQ 算法原理](../theory/opq-theory.md)
- 公共头文件：`engine/include/db/index/vector_index/opq/opq.h`
- 源文件：`engine/src/db/index/vector_index/opq/opq.c`
- 测试文件：`engine/test/vector_index/opq/opq_test.cpp`
- 相关索引实现：IVF 实现（`docs/arch/implementation/ivf-impl.md`）、DiskANN 实现（`docs/arch/implementation/diskann-impl.md`）、HNSW 实现（`docs/arch/implementation/hnsw-impl.md`）
