# AI Infra 学习轨：从零建立技术栈体系并跑通真实推理链路

**日期**：2026-11-26
**状态**：待审阅
**关联**：`docs/superpowers/plans/2026-11-12-study-hub-接口契约.md`（loader C 格式与判题契约）
**落地位置**：内容 → `learning/units/ai/`（learning 仓）；代码 → `apps/study-hub/`（本仓）
**前置**：无（本设计是 AI Infra 轨的起点，不是既有计划的延伸）

## 1. 为什么现在写这份设计

### 1.1 现状

study-hub 已有三条顶层轨（`algorithm` / `database` / `ai`），`ai` 轨现在装的是
`learning/scaffold/grokking/` 下 13 张卡：

```
cnn  decision_tree  dimensionality_reduction  gradient_boosting
linear_regression  logistic_regression  ml_fundamentals  neural_network
nlp  random_forest  recommendation  rnn  svm
```

这 13 张是**机器学习基础**（纯 numpy 手写卷积、池化、回归），**不是 AI Infra**。
按 AI Infra 的定义（支撑训练与推理的系统软件栈），`ai` 轨目前是空的：
没有 KV Cache、没有连续批处理、没有量化、没有引擎、没有分布式。

`learning/units/` 下现在只有 `database/`（4 个 `.assert.py`，没有 `.md`）；
`learning/units/ai/` **目录不存在**。

### 1.2 驱动力

用户要求「系统性了解 AI Infra 技术栈」，并明确三个约束：

1. **先建知识体系再动手** —— 「没有知识体系，直接跑出结果我也理解不了」
2. **每天约 1 小时，长期投入** —— 不能设计成需要整块时间的计划
3. **内容要集成进 study-hub** —— 复用已有的判题、掌握度、SM-2、供给流水线，不另造体系

同时明确自我定位为**编程零基础自评**（选项 B）。本设计不假设任何前置知识，
基线通过第 9 节的「基线自测单元」实测，而不是猜。

### 1.3 本机硬约束（实测，2026-11-26）

| 项 | 值 | 卡住什么 |
|---|---|---|
| GPU | RTX 4060 Laptop，**8188 MiB**，驱动 610.62 / CUDA UMD 13.3 | 7B AWQ 可跑；FP16 7B、>13B、大 batch 不可 |
| 架构 | Ada，`capability (8, 9)`（sm_89） | **支持 FP8 推理**、支持 FlashAttention-2；**不支持 FP8 训练** |
| CPU | i9-14900HX，24 核 / 32 线程 | Gloo CPU 多进程玩具分布式可行 |
| 内存 | **16 GB 物理**（WSL 现分得 7745 MB） | 大模型 CPU offload、多进程数据加载紧张 |
| 卡数 | **1 张** | 真 NCCL 多卡、张量并行、P2P 不可 |
| 磁盘 | C 盘余 452.8 GB；WSL `/` 余 954 GB | 充裕 |
| WSL2 | Ubuntu + Ubuntu-24.04 已建；**`nvidia-smi -L` 认到 RTX 4060** | GPU 直通已通，无需重装 |
| WSL 工具链 | gcc 13.3 / g++ / make / git 全在；网络可达 pypi（HTTP 200） | 编译不用另装 |
| `nvcc` | **无**（`/usr/local/cuda*` 不存在） | 只卡「手写 CUDA C++ kernel」，不卡 Triton |
| Windows Python | 3.12，torch **2.15.0+cpu**（`cuda.is_available()` = False），装在**系统级** `Python312`，非 venv | **不能原地覆盖**：同一 site-packages 里装着 sentence-transformers 5.1.2 |

### 1.4 已知会踩的坑

- **P-1**：WSL 现只分到 7.7 GB。加载 7B 权重时 CPU 侧要暂存约 5 GB safetensors，
  7.7 GB 会触发 **OOM killer 静默杀进程** —— 表现为「什么都没发生就退出了」，
  是这一类失败里最难查的形态。必须先改 `.wslconfig`。
- **P-2**：Windows 原生装不了 vLLM / Triton / FlashAttention（vLLM 官方明确不支持 Windows）。
  GPU 相关工作**全部放 WSL2**，Windows 侧的 torch 一字不动。
- **P-3**：`MAX_TIMEOUT_MS` 现为 60000（`judge/sandbox.js`），卡片脚本默认预算 30000
  （`judge/runner.js`）。AI Infra 实验真实耗时 60–600 秒，现有限幅**跑不完**。
- **P-4**：`api/diagnose` 的 AI 轨掌握度会把 13 张 ML 基础卡的流水混算，
  验收时分母会被污染。
- **P-5**：本仓**没有** `apps/study-hub/.env`，LLM 当前是关闭态。
  依赖 LLM 的验收环节（口述批改）在密钥配好前不可用。

## 2. 范围

### 2.1 本设计覆盖

1. **五层知识地图**的切法与顺序，41 个单元的完整清单与 `runs_local` 分级。
2. **单元文件形态**：沿用 loader C 的冻结 frontmatter，附加 4 个元数据字段。
3. **实验深度**：三段式（预测→实测→解释）+ 四道闸门 + 双算一致。
4. **与 study-hub 的接法**：6 个文件约 53 行代码改动，以及不改代码就能读入的部分。
5. **本机环境准备顺序**：5 步递进，全部在 WSL2，含每步的验证命令与失败判据。
6. **8GB 显存的降级参数 SOP**。
7. **验收方式**：三道验收线 + 每层检查点 + 8 项终期判定表。

### 2.2 本设计不覆盖

- **不写新代码子系统**。本设计是「学习轨」不是「工程特性」，代码改动只有第 5 节那 6 处。
- **不改动 13 张 ML 基础卡**。它们留在 `ai` 轨不动（见 P-4 的隔离方案）。
- **不做 LLM 批量生成**。前 5 个单元手写；批量流水线的模板改造见 §6.4，但不在首批范围。
- **不承诺训练侧真跑**。L4 的训练/平台/算子三块只做骨架级（见 §4.3 的 C 级判定）。
- **不覆盖多卡/租云**。计划里给出「如果要跑要租什么规格」，但不算本计划的交付物。

## 3. 已裁决的前提（用户确认，2026-11-26）

| 决策 | 结论 | 用户原话/依据 |
|---|---|---|
| 首要目标 | **四者都要**：进 study-hub、求职可用、建知识地图、代码为载体 | 「1 2 3 4 我都要」 |
| 起点 | **不假设任何前置知识**，用基线自测实测 | 「把我当成 0 基础」+ 选项 B |
| 投入节奏 | 长期，**每天约 1 小时** | 「长期投入，每天估计可能一个小时」 |
| 首阶段主攻 | **从知识体系开始**，不是先跑通项目 | 「先从知识体系开始吧…你把我当成 0 基础吧」 |
| 方案 | **知识地图先行 + 分层单元推进**（方案 A） | 「A」 |
| 终点档位 | **乙档**＝能讲清地图 + 本机跑通一条真实推理链路 | 「乙」 |
| 层序 | **五层顺序全部照准，不改** | 「顺序都对」 |
| 实验深度 | **做大做深** | 「实验做深一点」/「做大做深」 |
| 落点 | **内容集成到 study-hub** | 「内容集成到 study-hub 里面」 |
| 可跑性 | **本机不能跑的要标出来** | 「本机不能跑的需要标识出来」 |
| 时长 | 合理即可，不设上限 | 「合理就行不用管时间」 |
| 环境归属 | **GPU 工作全在 WSL2**，Windows 侧 torch 不动 | 「ok」 |
| 环境节奏 | 不集中装，**嵌进单元** | 「ok」 |
| 7B 上下文 | **1.5B 主力，7B 只做对照**，7B 上 `max_model_len=1024` | 「ok」 |

## 4. 单元卡的形态与通关标准

### 4.1 单元文件：沿用 loader C 的冻结格式

落点 `learning/units/ai/<stem>.md`，格式与 `learning/units/database/` 完全同构。
loader C（`core/unit/loader-c.js`）已经在读这个目录，`ai` 已在 `core/track/tracks.js`
的 `TRACKS` 里，`ID_PATTERN` 由 `TRACKS` 派生 —— **内容放进去即被识别**。

```markdown
---
id: ai:kv-cache-formula
track: ai
kind: card
title: KV Cache：一次前向到底留下多少字节
difficulty: medium
topics: [infra:显存, infra:注意力]
source: generated
language: python
demo: units/ai/kv-cache-formula.demo.py
reference: ""
graded: script
script: units/ai/kv-cache-formula.assert.py
status: draft
progress: todo
reviewed: false
reviewed_at: ""
subtracks: []
runs_local: yes
needs: []
fallback: ""
timeout_s: 120
---

## 讲解

（正文分三节：解决什么问题 / 上游是谁 / 下游被谁支撑）

## 关联

- 层：L1
- 前置：ai:memory-traffic
- 后继：ai:prefill-vs-decode
```

**收尾的 `---` 是必需的**：`loader-c.js` 的主正则要求开头 `---` 之后、
收尾 `---` 之前**至少有一行**内容；缺收尾行会被 `diagnoseFrontmatter` 判成
`no-closing-fence`，拒绝解析并给一条中文 warning。

`status` 初始为 `draft`；经 `supply.js verify --track ai` 真跑 + 变异测试通过后写回 `verified`。

### 4.2 四个附加元数据字段（不进 Unit 的 17 个冻结字段）

沿用契约 §3.3.1 的**相同思路**（算法轨的 `negative:` / `source_id:` / `url:`
「都不进 Unit」），但载体改为一个嵌套袋，理由见 §6.3。四个字段写在**同一个 frontmatter 内**，
紧跟 `subtracks` 之后：

```yaml
subtracks: []            # —— 以上是契约 §3.3 冻结字段，止于此 ——
runs_local: yes          # 选填，yes|partial|no，缺省 no（对应 A/B/C）
needs: [vram]            # 选填，gpu-count|vram|nvcc|fp8-train|ram，缺省 []
fallback: ""             # 选填，跑不了时的降级路径，一句话
timeout_s: 300           # 选填，断言脚本预算（秒），缺省 30，上限 600
```

**为什么缺省是 `no` 而不是 `yes`**：宁可把能跑的误标成跑不了（用户会自己发现），
也不能把跑不了的误标成能跑 —— 后者会让学习者在深夜卡在一个永远跑不通的实验上。

`needs` 与 `fallback` 必须在 `## 关联` 里也写一句人话，形成**机器可读 + 人可读双份**：

```
## 关联

- **本机跑不了**：需要 ≥2 卡 NVLink，本机单卡无法复现多卡带宽瓶颈
- 降级：读 NCCL 源码的 ring / tree 算法 + 用 Gloo CPU 后端验证通信语义
```

### 4.3 可跑性三级

| 级 | `runs_local` | 判定标准 |
|---|---|---|
| **A** | `yes` | 该单元描述的**现象**能在本机复现并测量：有可执行命令，有可测量的输出（显存曲线、吞吐、精度、带宽） |
| **B** | `partial` | 只能玩具化 / 缩小版复现：结论可推广，但测不到规模效应（如 Gloo CPU 4 进程能学通信语义，学不到带宽瓶颈） |
| **C** | `no` | 只能读源码 + 看公开数据；`needs` 必须写明缺什么 |

**A 级成立的前提**：§7.2 步骤 4 的冒烟测试（vLLM + 1.5B AWQ）通过。
若该 gate 失败，L2 的 4 个实验与 L3 的 2 个实验全部降级为 B，`runs_local` 批量改写。
这是一条**显式的降级触发条件**，不是事后补救。

### 4.4 实验深度：三段式

现有 `learning/units/database/mvcc.assert.py` 是「stdout 里找某句话」式校验。
对 MVCC 够用，对 AI Infra **不够** —— AI Infra 的结论都是**数字**，数字抄错了照样「包含」。

每个实验单元按三段组织，**三段各有一条独立断言**，全部写在 `.assert.py` 内部。原因是
`runner.js:221` 的 `buildCardAsserts` 对卡片类只追加**两条固定的隐含断言**
（`exit 0` + `contains 成功标记`），其余靠脚本自己判。所以容差与区间逻辑必须落在
Python 脚本里，**不改 `judge/asserts.js` 的 cmp 四档**：

| 段 | 内容 | demo 打印 | assert 怎么验 |
|---|---|---|---|
| **预测** | 先用公式手算期望值（显存增量、理论 FLOPs、带宽下界） | `predict=<数值> <单位>` | **独立实现复算**（不复用 demo 的函数），比对容差 |
| **实测** | 真跑，读 `nvidia-smi` / CUDA event / 计时 | `measure=<数值> <单位>` | 落合理区间，且**预测与实测的偏差被显式打印** |
| **解释** | 差值来自哪（碎片？L2 命中？kernel launch？） | `explain=<主要原因名>` | 断言该原因名出现，且与偏差最大的项一致 |

**独立复算**是这一节的核心：`assert.py` 用自己的一套公式重算，不 `import` demo 的函数。
demo 算错了断言照样红 —— 否则就是自己蒙自己。

### 4.5 四道闸门

每个实验单元必须四条全绿才写回 `verified`：

| 闸门 | 判据 | 复用什么 |
|---|---|---|
| ① 双算一致 | assert 复算与 demo 的 `predict` 偏差 ≤ 单元声明容差 | 新增（脚本内实现） |
| ② 中间量对 | 不只最终结论：每 token 字节数、利用率、峰值显存逐条断言 | 新增（脚本内实现） |
| ③ 变异必红 | `verify.js` 的三种手法（改 `assert` / 反转 `if` / 提前 `return`）任一改坏 demo，断言必须变红 | `core/supply/verify.js` 已有 |
| ④ 退化必红 | 故意造 OOM 路径 / 缺 numpy 路径，断言必须分辨出来 | 部分复用（`runner.js` 已分流 numpy 缺失） |

③④ 合起来是「负例全过说明用例太弱」的 AI Infra 版 —— 算法轨已经用这一条刷过题库。

### 4.6 通关标准：机器关 + 口述关

| 关卡 | 怎么判 | 离线可用 |
|---|---|---|
| **机器关** | `graded: script` 跑断言 → `pass` / `wrong_answer` / `timeout` / `runtime_error`；四道闸门全绿 | ✅ 完全离线 |
| **口述关** | 卡点问题自己答一遍 → `POST /api/llm/grade-idea`，`verdict` 进 `attempts` 表 | ⚠️ 需密钥（当前无 `.env`，见 P-5） |

口述关**配了密钥才要求**；没配时界面只显示提示，不阻断。这守的是
`tests/llm-off.test.js` 钉住的硬约束「LLM 关掉后 app 照常可用」。

掌握度公式（`core/mastery/mastery.js`）算的是**两次关卡的提交流水**，不是「读过就算过」：

```
首次通过率 = firstPass / attempts
遗忘因子   = 0.5 ^ (距上次练习天数 / 14)
掌握度     = (0.7 × 首次通过率 + 0.3 × e^(-失误次数/2)) × 遗忘因子
```

## 5. 五层知识地图

### 5.1 地图的形状：依赖图，不是清单

每个单元必须能回答三个问题，答不全不配进地图：

1. **它解决什么问题** —— 没有它，系统会在哪一步崩掉或浪费
2. **它的上游是谁** —— 不理解上游，这个单元就是死记
3. **它的下游被谁支撑** —— 不知道谁受益，就判断不出取舍

**地图的边比节点更重要。** 物理产出物是两个地图单元（`ai:map-dependency` /
`ai:map-positioning`）：一张 41 节点的 Mermaid 依赖图（层间 + 层内边，每节点带 A/B/C 标签），
一张一句话定位表（每行 `id | 解决什么问题 | 上游 | 下游 | runs_local`）。

### 5.2 五层与卡点问题

每层用一个**具体的、能算的**问题收口，不是「了解 XXX」：

| 层 | 卡点问题 | 讲 | 实验 | 合计 | yes/partial/no |
|---|---|---|---|---|---|
| **L0 数值与显存底座** | 为什么所有约束最后都收敛成「显存容量 + 带宽」这两个数？ | 3 | 0 | 3 | 3/0/0 |
| **L1 一次前向的真实代价** | 一次 Transformer 前向，算了多少 FLOPs、读写了多少字节、产出了多少 KV？ | 5 | 2 | 7 | 7/0/0 |
| **L2 引擎怎么不浪费** | 8GB 卡怎么同时服务 20 个并发而不 OOM，且 GPU 利用率不塌？ | 7 | 4 | 11 | 10/1/0 |
| **L3 更少显存 + 更专业务** | 量化在数学上损失了什么？TTFT / TPOT 各由哪个环节决定？ | 6 | 3 | 9 | 6/2/1 |
| **L4 另外三个方向** | 训练分布式、算子编译、检索与平台，各自解决什么、为什么本机跑不了？ | 7 | 0 | 7 | 3/2/2 |
| **地图 + 环境（非讲）** | — | — | — | 4 | 4/0/0 |
| **合计** | | **28** | **9** | **41** | **33/5/3** |

> **修正记录**：设计讨论中一度估为 25/9/7，随后在草稿里算成 34/4/3。
> 逐条判定后实际是 **33 yes / 5 partial / 3 no**，即 **80% A 级**。
> 差异来自 L3 的 `ai:quant-methods` 与 `ai:speculative-decoding` 判 `partial` 而非 `yes`：
> 两者都要跑标定/草稿模型，1.5B 上可行但 7B 上会被内存掐死，够不上 A 级的
> 「现象可复现并测量」。
> **80% A 级不是乐观估计**，因为 A 级的定义就是「现象可复现」——
> 重头戏只在 9 个实验单元，其余讲单元的小规模 demo 都是 numpy / torch 小矩阵 + `nvidia-smi` 读数。

### 5.3 每层为什么这么排

- **L0 → L1**：L0 只讲「一个字节在 GPU 上走一趟要多久」（本机 HBM 理论 272 GB/s、
  SRAM/L2/HBM 三级、FP16/FP8/INT4 各存什么）。L1 立刻拿这把尺子去量一次 attention
  —— **不先把尺子造好，L1 只能变成公式背诵**。
- **L1 → L2**：L1 结束会得到两个数：**每生成一 token 要读多少 KV 字节**、
  **单请求 GPU 利用率为什么天然低于 20%**。这两个数直接推出 L2 的全部动机 ——
  **连续批处理不是「一种技术」，是「单请求利用率太低」这个事实的必然结果**。
- **L2 → L3**：L2 结束你会亲手把 8GB 打满、看到 OOM、再救活。L3 才有资格问
  「怎么在同样的卡上多塞 3 倍请求」。量化与 PagedAttention 的区别也在这里分清：
  **一个是让每个字节变小，一个是让浪费的字节变少**。
- **L4 独立成层**：前四层是**一条可验证的因果链**，L4 是另外三条平行链。
  混着学会让你在前 8 周被「NCCL 拓扑」「Triton IR」这类当前摸不到的东西打散注意力。
  L4 只做骨架级，正好对应「本机缺什么」。

### 5.4 41 个单元完整清单

`id` 一律 `<trackCode>:<stem>`，stem 满足 `[a-z0-9][a-z0-9._-]*`。

#### 非讲单元（4）

| id | 名称 | 交付物 | runs_local |
|---|---|---|---|
| `ai:map-dependency` | 依赖图 | 41 节点 Mermaid 图，边带标注 | yes |
| `ai:map-positioning` | 定位表 | 41 行一句话表 | yes |
| `ai:env-windows` | 环境 A：Windows 侧 | `.wslconfig` + WSL venv + CUDA torch | yes |
| `ai:env-wsl` | 环境 B：WSL 侧 | vLLM + Triton + 模型下载 + 冒烟 | yes |

#### L0 数值与显存底座（3 讲）

| id | 名称 | 核心内容 | runs_local |
|---|---|---|---|
| `ai:storage-hierarchy` | 三级存储层级 | SRAM / L2 / HBM 的容量与延迟；为什么 HBM 是瓶颈 | yes |
| `ai:roofline` | 算术强度与 roofline | 算力÷带宽 = 可负担的算术强度；batch 如何提高它 | yes |
| `ai:numerics-dtype` | 数值格式 | FP32/TF32/FP16/BF16/FP8/INT8/INT4 各存什么、精度损失在哪 | yes |

#### L1 一次前向的真实代价（5 讲 + 2 实验）

| id | 名称 | 核心内容 | runs_local |
|---|---|---|---|
| `ai:transformer-anatomy` | 一次前向的数据流 | embedding → QKV → attention → FFN → logits，每步的张量形状 | yes |
| `ai:flops-counting` | 参数量 → FLOPs | `2 × 参数量 × token 数` 的推导与例外（attention 项随长度平方） | yes |
| `ai:memory-traffic` | 每 token 字节数 | 权重读取字节 = 参数量 × 每参数字节；为什么 decode 是带宽问题 | yes |
| `ai:kv-cache-formula` | KV Cache 公式 | `2 × 层数 × head 数 × head_dim × 每元素字节 × token 数` | yes |
| `ai:prefill-vs-decode` | 两阶段的资源画像 | prefill 算力密集、decode 带宽密集；两者对 batch 的响应相反 | yes |
| `ai:exp-flops-vs-measured` | **实验**：手算 vs profiler | 手算 FLOPs 与 `torch.profiler` 实测偏差 < 20%；解释差额来源 | yes |
| `ai:exp-kv-growth` | **实验**：KV 随长度增长 | 4 档序列长度 × 3 档并发，实测显存斜率对上公式 | yes |

#### L2 引擎怎么不浪费（7 讲 + 4 实验）

| id | 名称 | 核心内容 | runs_local |
|---|---|---|---|
| `ai:single-request-utilization` | 单请求利用率 | 为什么 decode 时 GPU 利用率天然 < 20%（一个 token 搬完整套权重） | yes |
| `ai:batching-continuous` | 连续批处理 | 静态批 vs 连续批；请求长度异构时静态批浪费在哪 | yes |
| `ai:paged-attention` | PagedAttention | 预留 vs 按需；逻辑块与物理块的映射；碎片从哪来 | yes |
| `ai:scheduler` | 调度器 | 抢占 / 优先级 / 公平；调度粒度如何决定 TPOT | yes |
| `ai:chunked-prefill` | 分块预填充 | 长 prompt 不再一次性吃满显存；与 decode 交错的代价 | yes |
| `ai:pd-disaggregation` | P/D 分离 | 为什么拆开部署能提吞吐；单卡上拆的意义与代价 | partial |
| `ai:prefix-cache` | 前缀缓存 | 共享前缀的 KV 复用；命中率怎么测；什么场景才有收益 | yes |
| `ai:exp-oom-and-recover` | **实验**：亲手 OOM 再救活 | 触发 OOM → 说出直接原因 → 改参数跑起来 | yes |
| `ai:exp-batch-saturation` | **实验**：并发拐点 | 并发 1→32 的吞吐曲线；找出拐点并解释为何开始下降 | yes |
| `ai:exp-chunked-prefill` | **实验**：分块对照 | 开/关 chunked prefill 的 TTFT 与峰值显存差异 | yes |
| `ai:exp-kv-fp8` | **实验**：KV Cache 量化 | `kv_cache_dtype=fp8` 的显存减半与精度代价（Ada 支持 FP8 推理） | yes |

> `ai:pd-disaggregation` 判 `partial`：单卡上可以起两个进程，但两者抢同一块显存与带宽，
> 得到的是「拆开有额外开销」这个结论，而非「拆开能提吞吐」。真实收益需要 ≥2 卡。

#### L3 更少显存 + 更专业务（6 讲 + 3 实验）

| id | 名称 | 核心内容 | runs_local |
|---|---|---|---|
| `ai:quant-math` | 量化数学 | 舍入误差、异常值通道、group-wise 缩放、per-tensor 为何不够 | yes |
| `ai:quant-methods` | AWQ / GPTQ / SmoothQuant | 三者各自假设什么、在什么模型上强、校准需要什么 | partial |
| `ai:quant-kv` | KV Cache 量化 | 只量化 KV 不量化权重；对长上下文的意义 | yes |
| `ai:latency-metrics` | TTFT / TPOT / 吞吐 | 三者的定义、谁在排队、哪个指标对应哪类用户体验 | yes |
| `ai:speculative-decoding` | 投机解码 | draft 模型 + 验证；为什么 8GB 卡上收益反而可能为负 | partial |
| `ai:moe-parallel` | MoE 专家并行 | 专家切分、all-to-all、负载不均；为什么需要多卡 | no |
| `ai:exp-quant-3way` | **实验**：三组量化对照 | FP16 / INT8 / AWQ 的速度-精度曲线，偏差 < 5% | yes |
| `ai:exp-ttft-tpot` | **实验**：延迟归因 | 固定模型，扫 prompt 长度与并发，把 TTFT / TPOT 拆到环节 | yes |
| `ai:exp-prefix-cache` | **实验**：前缀命中 | 共享前缀 vs 独立 prompt 的 TTFT 差与命中率 | yes |

> `ai:quant-methods` 判 `partial`：AWQ/GPTQ 的校准需要跑一遍标定集，
> 1.5B 上可行（十几分钟），7B 上没做完就被内存掐死。`ai:speculative-decoding` 同理。

#### L4 另外三个方向（7 讲，全部骨架级）

| id | 名称 | 核心内容 | runs_local | needs |
|---|---|---|---|---|
| `ai:dp-tp-pp` | 数据/张量/流水线并行 | 切分维度、通信量对比、显存收益 | **no** | gpu-count |
| `ai:collective-comm` | 集合通信 | AllReduce / AllGather / ReduceScatter；ring 与 tree | partial | gpu-count |
| `ai:flash-attention` | FlashAttention | SRAM 分块、不落地中间矩阵、online softmax、结果等价 | yes | — |
| `ai:cuda-triton` | Triton 与 kernel | 用 Triton 手写一个融合 kernel；IR 与自动调优 | yes | — |
| `ai:operator-libraries` | 算子库 | cuBLAS / CUTLASS / FlashInfer 各自负责什么、怎么选 | partial | nvcc |
| `ai:vector-retrieval` | 向量检索与 RAG 栈 | HNSW vs IVF 取舍、延迟预算；对照本仓 `engine/src/index/` | yes | — |
| `ai:platform-observability` | 平台层 | GPU 调度粒度、gang scheduling、SM 利用率、长尾请求 | **no** | gpu-count |

> `ai:collective-comm` 判 `partial`：32 核跑 4–8 个 Gloo CPU 进程能验证**通信语义**，
> 验证不到 **NVLink/IB 带宽瓶颈**。`ai:operator-libraries` 判 `partial`：
> 读 CUTLASS 源码 + 用 Triton 写等价小 kernel，手写 CUDA C++ 需要 `nvcc`（本机无）。

**C 级共 3 个**：`ai:moe-parallel` / `ai:dp-tp-pp` / `ai:platform-observability`。
三个都不设实验单元，只要求「说得清 + 说得出本机缺什么」。

### 5.5 数量与时长

| 项 | 值 |
|---|---|
| 讲单元 | 28 |
| 实验单元 | 9（每个含 2–3 个可独立断言的场景） |
| 非讲单元 | 4 |
| **合计** | **41** |
| 单讲单元耗时 | 45–60 分钟 |
| 单实验单元耗时 | 1.5–2.5 小时（因「做大做深」） |
| **净投入** | **约 60–70 小时** |
| 有效学习日 | 每周约 5 天（留 2 天余量给中断） |
| 每周净时长 | 约 5 小时 |
| **周期** | **12–14 周**，折合约 **每周 3 个单元** |

> 口径说明：若按「每天 1 小时 × 7 天」算，60–70 小时只需 9–10 周。
> 本设计取 **12–14 周**，是因为长期计划必须留中断余量（生病、出差、状态差），
> 按每周 5 个有效学习日计。**排计划时按每周 3 个单元排，不是每周 4–5 个** ——
> 后者在遇到 OOM 调试那类「三小时无进展」的单元时会立刻崩盘。

## 6. 与 study-hub 的接法

### 6.1 归属：内容进 learning 仓，代码改本仓

study-hub 的设计是**内容与代码分离**：

| 什么 | 放哪 | 依据 |
|---|---|---|
| 学习单元内容（`.md` + `.assert.py` + `.demo.py`） | `learning/units/ai/` | 契约 §3.3 |
| study-hub 代码 | `apps/study-hub/` | 本仓 |
| 学习笔记正文 | `learning/` | `ai-docs/INDEX.md`「学习笔记与课程进度 → learning/」 |

`learning/` 是独立 git 仓；study-hub 只读它，写回时查脏，**绝不自动 commit**。
把内容塞进 `apps/study-hub/` 会破坏 `STUDY_HUB_LEARNING_ROOT` 这个单一内容根假设，
需要额外改 loader，改动量数倍。

### 6.2 零改动即可用的部分（逐条证据）

| 能力 | 现状 | 依据 |
|---|---|---|
| 读 `units/ai/*.md` | ✅ `loadFromUnitsDir` 遍历 `TRACKS` 的每一轨 | `core/unit/index.js:78-99` |
| 校验 id 前缀 `ai:` | ✅ `ID_PATTERN` 由 `TRACKS` 派生 | `core/unit/unit.js:45` |
| `## 讲解` 小节 | ✅ loader C 认「讲解」与「题面」两种 | `core/unit/loader-c.js:193` |
| 卡片走断言脚本 | ✅ `graded: script` + `script:` | `judge/runner.js:250-270` |
| 脚本超时预算 | ✅ 卡片默认 30 秒（`SCRIPT_TIMEOUT_MS`） | `judge/runner.js:74` |
| numpy 缺失不误判 | ✅ 返回 `runtime_error`，提示「装上 numpy 即可运行」 | `judge/runner.js:309-312` |
| AI 轨进三轨统计 | ✅ `track-mastery.js:280` 与 `tracks.js` 的 `weight: 0.3` | 已存在 |
| 供给缺口扫描 | ✅ `missingOf` 对 card 判 `asserts` / `statement` | `core/supply/plan.js:112-121` |
| 变异测试 | ✅ `assert` / `if` / `return` 三手法 | `core/supply/verify.js:36` |

**唯一会「零结果」的原因**：`learning/units/ai/` 目录不存在。建目录、放文件，系统自动认。

### 6.3 必须改代码的 6 处

| # | 文件 | 改什么 | 行数 | 波及测试 |
|---|---|---|---|---|
| 1 | `core/unit/loader-c.js` | 把 4 个附加字段装进 `unit.extra`（缺省齐全） | ~20 | `unit-loader-c.test.js`、`p2-loader-b.test.js` |
| 2 | `core/unit/index.js` | `refreshUnitIndex` 把 `unit.extra.runs_local` 写进新列 | ~3 | `unit-index.test.js` |
| 3 | `server/db.js` | `units` 表加 `runs_local TEXT NOT NULL DEFAULT ''` + 迁移 | ~12 | `db.test.js`、`p1-db-session.test.js` |
| 4 | `server/api-units.js` | `GET /api/units` 支持 `runs_local` 过滤 | ~8 | `api-units.test.js` |
| 5 | `judge/runner.js` | 卡片脚本预算读 `unit.extra.timeout_s` | ~6 | `p2-runner.test.js` |
| 6 | `judge/sandbox.js` | `clampTimeoutMs(ms, cap)` 加第二档 `MAX_SCRIPT_TIMEOUT_MS` | ~4 | `p2-sandbox.test.js`、`sandbox.test.js` |

**总计约 53 行 + 约 10 个测试调整。** 相对 41 个单元的内容量，改动占比极小 ——
符合本仓「不为一次性需求造抽象层」的守则：能不改就不改，必须改的改到最小。

#### 关键设计：为什么 `extra` 是嵌套袋，而不是 4 个顶层字段

契约 §1.2 的 17 个字段是**冻结**的（三个 loader 共用，`createUnit` 校验，多处测试钉形状）。
加 4 个顶层字段要动 `core/unit/unit.js` 的 `createUnit`、字段表文档、以及所有断言
Unit 形状的测试。

改为：**loader C 在 `createUnit` 返回后挂一个 `unit.extra`**，形状固定为
`{ runs_local, needs, fallback, timeout_s }`，四个键**恒存在**（给缺省值）。
loader A / B 产出的单元 `extra` 为 `{}`。`createUnit` 与冻结字段表**一个字不动**。

后果与代价：

- `refreshUnitIndex` 直接读 `unit.extra.runs_local`，不需要重读单元文件（比契约 §3.3.1
  的「自己再读一遍 frontmatter」更省：41 个单元不用多 41 次 IO）。
- `runner.js` 直接读 `unit.extra.timeout_s`，同样不用重读文件。
- 新增 `unit.extra` 需要一条测试钉住「loader A/B 的 extra 恒为 `{}`」，
  防止将来有人往 loader A 里也塞东西导致形状漂移。

#### 关键设计：超时的第二档上限

现状 `judge/sandbox.js:35` 的 `MAX_TIMEOUT_MS = 60000` 是**全局**限幅，
抬高它会同时放宽用户提交代码的时限（OJ 判题契约的 2000ms 默认值靠它兜底）。

改为加第二档，**不动原档**：

```js
// judge/sandbox.js
const MAX_TIMEOUT_MS = 60000           // 不变：用户代码上限
const MAX_SCRIPT_TIMEOUT_MS = 600000   // 新增：断言脚本上限（10 分钟）

function clampTimeoutMs(ms, cap) {
  const n = Number(ms)
  const ceiling = Number.isFinite(Number(cap)) && Number(cap) > 0 ? Number(cap) : MAX_TIMEOUT_MS
  if (!Number.isFinite(n) || n <= 0) return DEFAULT_TIMEOUT_MS
  return Math.min(Math.floor(n), ceiling)
}
```

`runner.js` 只在卡片分支传第二档；用户代码分支传 `undefined`，行为不变。

### 6.4 供给流水线对 AI 轨的现状

| 子命令 | 对 AI 轨 | 说明 |
|---|---|---|
| `plan --track ai` | ✅ 能跑 | 扫 `units/ai/` 缺口（缺 assert / 缺讲解） |
| `generate --track ai` | ⚠️ 需改造 | `core/llm/prompts.js:745` 的 `ai:` 分支是 ML 基础卡模板 |
| `verify --track ai` | ✅ 能跑 | 真跑断言 + 三种变异，已通用 |
| `review --track ai` | ✅ 能跑 | 人工抽检 |
| `report --track ai` | ✅ 能跑 | 计数与欠账 |

**首批不启用 `generate`。** 理由：LLM 生成的是「看起来合理但没实测过」的内容，
而地图与第一批单元的准确性直接决定后面 33 个的质量。**前 5 个单元手写**
（2 地图 + 2 环境 + 1 基线探针，即 §9 的 `ai:baseline-probe`），
41 个正式单元里剩下 **37 个**再上批量流水线。这复用 P5 算法轨「先试点、再批量」的节奏。

`generate` 模板改造（换成三段式 + 容差断言 + `runs_local` 声明）列入本设计的
**后续项**，不在首批交付范围。

## 7. 本机环境准备顺序

### 7.1 总原则：GPU 工作全在 WSL2，Windows 侧不动

| 组件 | Windows 原生 | WSL2 |
|---|---|---|
| CUDA 版 torch | ✅ | ✅ |
| **vLLM** | ❌ 官方不支持 | ✅ |
| **Triton** | ❌ 只有第三方构建 | ✅ |
| **FlashAttention** | ❌ 需 Linux 编译 | ✅ |
| bitsandbytes（QLoRA） | ⚠️ 极不稳定 | ✅ |

**Windows 侧的 torch 2.15.0+cpu 原样保留** —— 同一 site-packages 里装着
sentence-transformers 5.1.2，覆盖安装会连带打坏（P-2）。

### 7.2 五步递进

#### 步骤 1｜WSL 内存配额（5 分钟）

`C:\Users\yinch\.wslconfig` 现不存在，新建：

```ini
[wsl2]
memory=11GB
swap=12GB
swapFile=D:\\wsl-swap.vhdx
```

理由：WSL 现只有 7.7 GB，7B 权重的 CPU 暂存约需 5 GB，会触发 OOM killer（P-1）。
11 GB 给 WSL，Windows 留约 4.7 GB。swap 放 `D:` 盘，不占 C 盘余量。

**验证**：

```bash
wsl --shutdown
wsl -d Ubuntu-24.04 -- bash -lc "free -g | head -2"
```

期望 `Mem: total` ≈ 10–11。

**失败判据**：仍为 7 → 检查文件名（必须是 `.wslconfig` 不是 `wslconfig`）、
位置（用户主目录不是仓库）、且必须 `wsl --shutdown` 才生效。

#### 步骤 2｜WSL venv + CUDA 版 torch（20–30 分钟，下载 2.5–3 GB）

```bash
sudo apt update && sudo apt install -y python3-venv python3-pip
mkdir -p ~/aiinfra && cd ~/aiinfra
python3 -m venv .venv
source .venv/bin/activate
```

**必须先探测可用 wheel，不猜版本号**：

```bash
pip index versions torch --index-url https://download.pytorch.org/whl/cu128 2>&1 | head -5
pip index versions torch --index-url https://download.pytorch.org/whl/cu130 2>&1 | head -5
```

选返回版本列表的那个索引装（驱动 610.62 / UMD 13.3 向下兼容 cu128 与 cu130）。

**验证**（必须真，不能靠「pip 说成功」）：

```bash
python -c "
import torch
print('torch', torch.__version__)
print('cuda_available', torch.cuda.is_available())
print('device', torch.cuda.get_device_name(0))
print('capability', torch.cuda.get_device_capability(0))
x = torch.randn(2048, 2048, device='cuda', dtype=torch.float16)
y = (x @ x).sum().item()
print('matmul_ok', y == y)
"
```

期望：`cuda_available True` / `device NVIDIA GeForce RTX 4060 Laptop GPU` /
`capability (8, 9)` / `matmul_ok True`。

**失败判据**：`cuda_available False` → 装成了 `+cpu` wheel（查版本号有无 `+cuXXX`），
或 WSL 直通坏了（回跑 `nvidia-smi -L`）。`capability` 不是 `(8, 9)` → 装错架构。

#### 步骤 3｜vLLM + Triton（15–40 分钟，约 2 GB）

```bash
pip install vllm
```

顺带装好 L2/L3 需要的全部组件：triton、flashinfer、vLLM 自带的 paged attention kernel
（**不需要单独编译 FlashAttention** 就能跑）。

**验证**：

```bash
python -c "import vllm; print('vllm', vllm.__version__)"
python -c "import triton; print('triton', triton.__version__)"
python -c "
import triton, triton.language as tl, torch
@triton.jit
def add(a, b, out, n, BLOCK: tl.constexpr):
    i = tl.program_id(0) * BLOCK + tl.arange(0, BLOCK)
    m = i < n
    tl.store(out + i, tl.load(a + i, mask=m) + tl.load(b + i, mask=m), mask=m)
n = 1000
a = torch.randn(n, device='cuda'); b = torch.randn(n, device='cuda')
out = torch.empty(n, device='cuda')
add[(triton.cdiv(n, 256),)](a, b, out, n, BLOCK=256)
print('triton_kernel_ok', torch.allclose(out, a+b))
"
```

**这是「能不能进 L4 算子层」的判定点**。20 行 Triton kernel 跑通 = 手写 kernel 这条路可行。

**常见失败**：`pip install vllm` 卡在编译 → 没匹配到预编译 wheel，通常因为 Python 太新。
解法：venv 用 3.11 或 3.12，**不要 3.13+**。

#### 步骤 4｜模型权重 + 冒烟测试（下载约 4 GB）—— **A 级 gate**

| 模型 | 量化后显存 | 用途 |
|---|---|---|
| `Qwen/Qwen2.5-1.5B-Instruct-AWQ` | ~1.2 GB | 连续批处理、KV Cache 观测（**主力**） |
| `Qwen/Qwen2.5-7B-Instruct-AWQ` | ~4.3 GB | 真实 7B 场景、对照 |
| `meta-llama/Llama-3.2-1B-Instruct` | ~2.6 GB (FP16) | 量化对照的 FP16 基准 |

```bash
pip install huggingface_hub
# 国内网络慢时先设镜像
export HF_ENDPOINT=https://hf-mirror.com
huggingface-cli download Qwen/Qwen2.5-1.5B-Instruct-AWQ --local-dir ~/aiinfra/models/qwen1.5b-awq
```

**冒烟测试（整个环境准备的最终验收点，也是 A 级的成立条件）**：

```bash
python -c "
from vllm import LLM, SamplingParams
m = LLM(model='~/aiinfra/models/qwen1.5b-awq', quantization='awq',
        max_model_len=2048, gpu_memory_utilization=0.85, max_num_seqs=16)
p = SamplingParams(max_tokens=64)
out = m.generate(['用一句话解释什么是 KV Cache。'], p)
print(out[0].outputs[0].text)
"
```

期望：打印中文解释，不 OOM。

**这条跑通的时刻，L2 全部实验的地基铺好。** 跑不通 → 触发 §4.3 的 A 级降级条件。

#### 步骤 5｜CUDA Toolkit（**只在 L4 算子层需要**，可选，3–6 小时）

```bash
sudo apt install -y nvidia-cuda-toolkit
nvcc --version
```

**明确：L0–L3 全部不需要它。** Triton 自己编译 PTX，不依赖 `nvcc`。
只有手写 CUDA C++ `__global__` kernel 时才是必需。装前先确认能出 sm_89 的代码。

### 7.3 环境步骤与单元的对应

**环境准备不单独成一个阶段，而是嵌进单元** —— 避免「先花一整天装环境然后才开始学」：

| 步骤 | 落在哪个单元 | 为什么放这儿 |
|---|---|---|
| 1 | `ai:env-windows` | 编号最小、最简单，第一个小时就能看到 `free -g` 变化 —— 长期计划的早期胜利 |
| 2 | `ai:env-windows` 后半 | 同上 |
| 3 | `ai:env-wsl` | 装完立刻用，不搁置 |
| 4 | `ai:env-wsl` | 与步骤 3 一起形成第一个闭环 |
| 5 | `ai:cuda-triton` 的前置 | 全程最重的一步，放最后且可选 |

### 7.4 8GB 显存的降级参数 SOP

9 个实验单元反复使用，先定死：

| 参数 | 通用默认 | 8GB 卡上的值 | 原因 |
|---|---|---|---|
| `gpu_memory_utilization` | 0.9 | **0.85** | 留 1.2 GB 给 CUDA context 与碎片 |
| `max_model_len` | 模型满长 | **2048（1.5B）/ 1024（7B）** | KV Cache 随长度线性增长，是最大的显存消耗 |
| `max_num_seqs` | 256 | **32（1.5B）/ 8（7B）** | 并发 × 每序列 KV = 总 KV |
| `dtype` | auto | `float16` | Ada 上 bf16 也能跑，fp16 兼容性最好 |
| `kv_cache_dtype` | auto | **`fp8`**（实验用） | Ada 支持 FP8 推理，KV 直接砍半 —— 8GB 卡最大的可用优势 |
| `enable_chunked_prefill` | 视版本 | **开** | 长 prompt 不再一次性吃满显存 |

恒定关系：**显存 = 权重 + KV Cache × 并发 × 长度**。
8GB 卡的生存法则就是压后两项 —— 而这正是 L2/L3 全部内容想让你理解的东西。
**环境参数和课程内容是同一件事。**

## 8. 验收方式

### 8.1 三道验收线

| 线 | 判定什么 | 判定方式 | 谁判 |
|---|---|---|---|
| **线 1** 单元覆盖 | 体系建成没有 | `verified` 比例 + 四闸门 + topics 掌握度 | 机器 |
| **线 2** 链路真跑通 | 做到乙档没有 | 六条固定命令的实测输出 | 机器（可复现） |
| **线 3** 能讲清 | 真懂还是背下来 | 9 个卡点问题的口述 + 自查表 | 半人工 |

三条**全过**才算达成。任一条不过，计划延长，不降低标准。

### 8.2 线 1：单元覆盖（完全离线）

```
① verified 比例 = units/ai/ 里 status=verified 的单元数 ÷ 41
   目标 ≥ 37/41（90%）
   依据：tools/supply.js verify --track ai 的逐单元结论

② 四闸门全绿 = 9 个实验单元 × 4 道 = 36 项
   目标 36/36

③ infra:* 类 topics 的掌握度 ≥ 0.5
   依据：GET /api/diagnose?view=radar
```

**为什么 90% 而不是 100%**：3 个 C 级单元本机跑不了，`verify` 判 `skipped`
（契约里 skipped 不算打回）；另有 1 个余量给返工。

**为什么第 ③ 条用 `infra:*` 而不是整条 AI 轨**：`track-mastery.js:280` 的 AI 轨
会把 13 张 ML 基础卡的流水混算，分母被污染（P-4）。**隔离方案是不改契约、
用 `topics` 前缀区分** —— AI Infra 单元的 topics 一律以 `infra:` 开头
（如 `infra:显存`、`infra:量化`、`infra:调度`），验收时只统计该前缀。

副作用需提前记：**AI 轨的轨级掌握度会一直偏低**（被没练的 ML 卡拉下来）。
这是可接受的 —— 换个契约改动来「修好看」，风险大于收益。

### 8.3 线 2：链路真跑通（六条命令）

L2 结束时跑一次、L3 结束时再跑一次，输出贴进 `learning/units/ai/_meta/acceptance.md`。

> **为什么在 `_meta/` 子目录而不是 `units/ai/` 根下**：`core/unit/index.js:36-54` 的
> `listMarkdown` 会列出 `units/<track>/` 下**所有** `.md` 文件（非递归，但不过滤 `_` 前缀），
> 逐个交给 loader C。放根下的 `_index.md` / `_acceptance.md` 没有 frontmatter，
> 会因为 id 非法被拒 **并在每次启动时产出两条警告**。子目录名不以 `.md` 结尾，
> 会被同一个过滤器排除，不污染索引。现有 `units/database/` 之所以没这个问题，
> 是因为它只放了 `.assert.py` 与 `plan.json`（后者不以 `.md` 结尾）。

```bash
cd ~/aiinfra && source .venv/bin/activate

# 1. GPU 与算力基线
nvidia-smi --query-gpu=name,memory.total,driver_version,cuda_version --format=csv

# 2. torch 真能用 GPU
python -c "import torch;print(torch.__version__, torch.cuda.is_available(), torch.cuda.get_device_capability(0))"

# 3. 实测 HBM 带宽（L0 的尺子）
python scripts/bench_hbm.py --size 512MB --repeat 20

# 4. 推理：TTFT / TPOT / 吞吐
python scripts/bench_infer.py --model qwen7b-awq --max-model-len 1024 \
    --max-num-seqs 8 --prompts 20 --report json

# 5. KV Cache 随并发增长（同时读 nvidia-smi）
python scripts/bench_kv_growth.py --model qwen1.5b-awq \
    --seq-lens 256,512,1024,2048 --concurrency 1,4,8,16

# 6. 量化三组对照
python scripts/bench_quant.py --models qwen1b-fp16,qwen1.5b-int8,qwen1.5b-awq \
    --same-prompt --tokens 128
```

| 命令 | 判定区间 | 判据来源 |
|---|---|---|
| 2 | `True` + `(8, 9)` | Ada 架构确认 |
| 3 | **200–300 GB/s** | RTX 4060 Laptop（128-bit GDDR6，理论 272 GB/s）；低于 200 说明测法错 |
| 4 | TTFT **< 1.5 s**、TPOT **< 40 ms** | 7B AWQ 在 8GB 卡上的合理区间 |
| 5 | KV 显存**随长度线性**，斜率 ≈ 每 token 字节 × 并发 | L1 手算预测 vs 实测 |
| 6 | INT4 比 FP16 **快 1.5–2.5×**，perplexity 上升 **< 5%** | 量化的一般规律 |

**第 5、6 条必须贴上手算预测值**，不是「跑出数就行」。
**预测与实测的偏差要写出来并解释** —— 这是乙档与「照着教程跑一遍」的区别。

**失败判据**：第 4 条 TTFT > 3 s 或 TPOT > 100 ms → 参数没调对
（`max_model_len` 太大、`gpu_memory_utilization` 太高导致 swap），
**不是「本机跑不动」，是参数问题**，修到区间内，不降低标准。

### 8.4 线 3：能讲清（9 个卡点问题）

| 层 | 卡点问题 | 必含要素（5 条中至少 4 条） |
|---|---|---|
| L0 | 为什么 AI Infra 的约束最后都收敛成「显存容量 + 带宽」？ | 三级存储层级 / 算力与带宽比 / 算术强度 / HBM 实测值 / batch 为何提高算术强度 |
| L1 | 一次 Transformer 前向算多少 FLOPs、读写多少字节、产出多少 KV？ | 参数量→FLOPs 换算 / 每 token 字节数 / KV 公式 / 实测对照 / decode 为何带宽瓶颈 |
| L2 | 8GB 卡怎么同时服务 20 并发而不 OOM 且利用率不塌？ | 单请求利用率计算 / 连续批处理机制 / PagedAttention 解决什么 / 碎片与预留 / 实测拐点 |
| L3-量化 | 量化在数学上损失了什么？ | 舍入误差 / 异常值通道 / group-wise 缩放 / 精度-速度曲线 / INT4 为何仍可用 |
| L3-延迟 | TTFT 与 TPOT 各由哪个环节决定？ | prefill 算力密集 / decode 带宽密集 / 队列等待 / 调度粒度 / chunked prefill 作用 |
| L4-训练 | 张量并行与数据并行各解决什么、代价是什么？ | 切分维度 / 通信量对比 / 显存收益 / ring AllReduce / 本机为何跑不了 |
| L4-算子 | FlashAttention 为何省显存而不改数学结果？ | SRAM vs HBM / tile 分块 / 不落地中间矩阵 / online softmax / 结果等价 |
| L4-检索 | 向量索引与训练/推理栈是什么关系？ | RAG 链路 / HNSW vs IVF 取舍 / 为何分两层 / 本仓 `engine/src/index/` 实现 / 延迟预算 |
| L4-平台 | 平台层（调度 / 可观测）解决什么？ | GPU 调度粒度 / gang scheduling / SM 利用率 / 长尾请求 / 与引擎解耦的原因 |

**流程**：

| 步骤 | 怎么做 | 需 LLM |
|---|---|---|
| ① 自己写 | 每题 200–500 字，**不看笔记**，限时 10 分钟 | ❌ |
| ② 机器批改（可选） | `POST /api/llm/grade-idea`，`verdict` 进 `attempts` | ✅ 当前无 `.env`，跳过 |
| ③ 自查表 | 每题 5 条必含要素逐条打勾 | ❌ |

**自查表怎么防作弊**：5 条要素里**至少 2 条必须是实测数字**（L0 要写 272 GB/s 或实测值，
L1 要写按公式算出的字节数）。**只讲概念、没讲数字的不算过** ——
这正好对应「跑出结果也理解不了」的反面。

### 8.5 每层检查点

| 层结束 | 检查点 | 判据 |
|---|---|---|
| L0 | 能用实测带宽解释一切约束 | 带宽落在 200–300，且能用它推出「单请求 GPU 利用率 < 20%」 |
| L1 | 手算能对上实测 | 手算 FLOPs 与 `torch.profiler` 偏差 < 20%；手算 KV 字节与 `nvidia-smi` 偏差 < 20% |
| L2 | 8GB 打满并复活 | 亲手触发一次 OOM、说出直接原因、改参数跑起来 |
| L3 | 量化曲线自证 | 三组曲线与手算趋势一致（INT4 最快、有精度损失） |
| L4 | 骨架级能讲清 | 4 个方向卡点问题各答对 ≥ 4/5，**且说得出每个方向本机缺什么** |

**L2 检查点特别重要**：必须**亲手触发一次 OOM**。OOM 是 AI Infra 最常见、
也最容易被「抄参数」蒙过去的坑。**没被 OOM 拷打过的人，参数永远是抄来的，不是算出来的。**

### 8.6 终期判定表（8 项）

| # | 项目 | 判据 | 判定方 | 不过怎么办 |
|---|---|---|---|---|
| 1 | 单元覆盖 | `verified ≥ 37/41` | 机器 | 补齐缺口单元 |
| 2 | 四闸门 | 36/36 全绿 | 机器 | 修断言 |
| 3 | topics 掌握度 | `infra:*` ≥ 0.5 | 机器 | 重练薄弱单元（SM-2 会排） |
| 4 | 六条实测命令 | 全部落区间 | 机器 | 调参数到达标 |
| 5 | 预测 vs 实测 | 偏差写清并解释 | 人工查 `_meta/acceptance.md` | 回补手算 |
| 6 | 9 个卡点问题 | 每题 ≥ 4/5 要素 | 半人工 | 重答 |
| 7 | 地图完整 | 41 节点图 + 定位表齐全，边有标注 | 人工 | 补全图 |
| 8 | 环境可重建 | 完整环境记录，能按它复现 | 人工 | 补记录 |

第 8 条容易被忽略但关键：**环境一旦重装或换租用 GPU，只有能按记录重建才算真掌握**。

## 9. 基线自测：起点不靠猜

用户自评为「编程零基础」，但仓库里已有大量 C/C++ 系统级产出。
不猜起点，用**一个单元**实测：

`ai:baseline-probe`（临时单元，放在 L0 之前，不计入 41 个正式单元）：
20 道小题，覆盖三块，15–20 分钟做完。

| 块 | 题数 | 例 |
|---|---|---|
| 编程基础 | 7 | 变量/指针/内存布局；Big-O；一个 for 循环的迭代次数 |
| Python 与张量 | 7 | list vs ndarray；形状与广播；`a[0]` 与 `a[0:1]` 的区别 |
| AI 概念 | 6 | 梯度是什么；attention 在算什么；显存里装了什么 |

**判定与分支**（`baseline-probe` 是 41 个正式单元之外的第 1 个探针单元，
三个分支都会产出它，所以下面的数字都已含它）：

| 得分 | 起点 | 交付单元总数 | 周期增量 |
|---|---|---|---|
| ≥ 16/20 | L0 直达 | 41 + 1 = **42** | 0 |
| 10–15 | L0 前加补基础 `ai:baseline-python` / `ai:baseline-autograd` | 41 + 1 + 2 = **44** | +1 周 |
| < 10 | L0 前加 5 个补基础单元 | 41 + 1 + 5 = **47** | +2 周 |

补基础单元**同样落 `learning/units/ai/`，同样有 demo + assert**，不另开体系。
`baseline-probe` 本身也带 assert（自评分数由脚本算，不靠感觉）。

## 10. 风险与降级触发条件

| 风险 | 触发信号 | 降级动作 |
|---|---|---|
| **R-1** vLLM 在 8GB 装不起来 / 冒烟失败 | §7.2 步骤 4 失败 | L2 的 4 个实验 + L3 的 2 个实验全部降 B；`runs_local` 批量改写；L2 讲单元改用 llama.cpp + gguf 观测（能力减半） |
| **R-2** WSL 内存不足导致 OOM killer | 步骤 4 出现「静默退出」 | 加物理内存条（16→32 GB，约 300–400 元，性价比最高的单笔投入）；或改 `max_model_len=1024` 只跑 1.5B |
| **R-3** 7B 在 1024 上下文下仍不稳 | 步骤 4 第 4 条命令频繁 OOM | 7B 降为「只跑一次冒烟」，所有量化/延迟实验改 1.5B；在 `_meta/acceptance.md` 记明 |
| **R-4** `MAX_SCRIPT_TIMEOUT_MS` 改动波及判题测试 | `p2-sandbox.test.js` 红 | 第二档上限对用户代码分支不生效，测试应原样通过；若红，说明改动越界，回退到「按 unit.extra 传 cap」 |
| **R-5** 实验脚本单次超过 10 分钟 | `timeout` verdict 频繁出现 | 拆单元（把「三组量化」拆成三个单元），**不抬上限** —— 抬上限会掩盖设计问题 |
| **R-6** 长期计划中断 | 连续 2 周无提交流水 | SM-2 会在 `/api/next` 排复习；若仍断，缩小单单元颗粒（讲单元拆成「读 30 分钟」+「答 1 题」） |

## 11. 交付物清单（供 writing-plans 展开）

| 阶段 | 交付物 | 验收 |
|---|---|---|
| **P0** 工装与骨架 | ① `learning/units/ai/` 目录<br>② 6 处代码改动（TDD，含约 10 个测试）<br>③ `ai:map-dependency` + `ai:map-positioning`（Mermaid 图与定位表就写在各自单元的 `## 讲解` 里，**不另存 `_meta/` 副本**）<br>④ `ai:env-windows` + `ai:env-wsl`<br>⑤ `ai:baseline-probe`<br>⑥ `_meta/acceptance.md` 模板 + 6 个 bench 脚本骨架 | `node tests/run-all.js` 全绿；`node tools/supply.js plan --track ai` 能列出 5 个单元且缺口判对；启动日志无「单元 id 非法」警告 |
| **P1** L0 + L1 | 10 个单元（含 2 个实验） | 每单元 `verify --track ai` 判 `verified`（变异测试变红） |
| **P2** L2 | 11 个单元（含 4 个实验） | 同上；外加线 2 的六条命令首跑，输出进 `_meta/acceptance.md` |
| **P3** L3 | 9 个单元（含 3 个实验） | 同上；量化三组曲线自证 |
| **P4** L4 + 收口 | 7 个单元（骨架级）+ 地图回填 + 终期判定表打分 | 8 项判定表全绿；线 2 六条命令复跑一致 |

## 12. 明确不在本设计范围

- **不改造 `generate` 的 AI 轨模板**（`core/llm/prompts.js:745`）。后续项。
- **不动 13 张 ML 基础卡**（`GROKKING_AI_CARDS` 已冻结在契约 §14.1）。
- **不改 `judge/asserts.js` 的 cmp 四档**（`exact`/`trim`/`json`/`lines`）——
  容差逻辑落在 `.assert.py` 内：`runner.js` 的卡片分支只追加两条固定隐含断言
  （`exit 0` + `contains 成功标记`），加 cmp 档位对卡片类用不上。
- **不改 `MAX_TIMEOUT_MS` 原档**（60000 保留给用户代码）。
- **不承诺多卡 / 租云**。L4 的三个 C 级单元只要求「说得清 + 说得出缺什么」。
- **不新建顶层目录**。内容进 `learning/`，代码进 `apps/study-hub/`，文档进 `docs/`。
