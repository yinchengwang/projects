# TEXT 列长值落点评估

**日期**：2026-11-24
**状态**：待审阅
**关联**：`docs/superpowers/specs/2026-11-23-json-column-long-literal-design.md` §3.2
**内核位置**：`engine/db`

## 1. 结论摘要

**本轮不改代码。** TEXT 列的 255 字节上限保持为**显式校验**（超限报 `E_NOSPACE`），
不做隐式截断。

如果将来要支持长 TEXT，需要动**值层的值表示契约**，代价见 §4。
本文档把"要动什么、动多少、有哪些坑"写清楚，供将来决策。

## 2. 现状

### 2.1 TEXT 列的能力边界

| 层 | 限制 | 位置 |
|---|---|---|
| `DbValue.s[]` | 256 字节（`VAL_TEXT_MAX`）⇒ 内联最多 255 | `storage/value.h` |
| `val_text()` | 超过 255 **静默截断** | `storage/value.c:69` |
| `val_copy()` | 超过 255 静默截断 | `storage/value.c:104` |
| 记录变长区 | 无上限（受 `PAGE_SIZE` 限制） | `storage/record.c` |
| 批解码 | 截到 255（与逐行路径对齐） | `storage/batch.c` |
| `coerce_to_col` | 现在**显式报错**（超 255 ⇒ `E_NOSPACE`） | `query/exec_write.c` |

⇒ 现在的行为是：**长 TEXT 写入明确报错，不静默截断**。
这是 C5.3b-a 修好的状态（之前是截到 63 字节）。

### 2.2 为什么 TEXT 不能像 JSON 那样走"运行时外部值"

JSON 的 256–2048B 档能成立，是因为：
- JSONB 是**二进制**，长度精确；
- 它的落盘位置在 record 变长区里（record 支持任意长度的变长列）；
- 读回时 `dbvalue_from_record_var` 给出**外部值**（指向记录缓冲）。

TEXT 为什么不照抄？

**因为 TEXT 的"外部值"没有落盘位置。**
`val_external_text` 描述的是"内容在 ctx 指向的内存里"——
它需要一个**持久的来源**（页链、blob 帧、vector 存储）。
JSON 的页链是 `json_ovf.c` 提供的；TEXT 没有任何对应物。

⇒ 要支持长 TEXT，必须**给 TEXT 也建一套页链**（或者复用 JSON 的页链机制）。

## 3. 三条可能的路

### 路线 A：TEXT 也走页链（复用 `PT_JSON_OVF` 或新建 `PT_TEXT_OVF`）

**做法**：TEXT 列 >255B 时写进页链，tuple 里存溢出标记 + 链首页 + 长度。

**优点**：与 JSON 的 256–2048B 档同构，一套机制。

**代价**：
1. 页链机制本身还没实现（C5.3b-b 是它的前置）。
2. 页链机制实现后，TEXT 复用它几乎零成本 —— **但那时再决定也不迟**。
3. 需要新的页类型（或复用 JSON 的 —— 但语义不同：TEXT 是文本，JSON 是二进制；
   页链本身不关心载荷，可复用）。

⇒ **推荐：等 C5.3b-b 完成后再评估。** 页链机制一旦落地，TEXT 接入是小活。

### 路线 B：只放宽内联上限（`DbValue.s[]` 变大）

**做法**：`VAL_TEXT_MAX` 从 256 提到 4096 或更大。

**优点**：改动最小（一个常量）。

**代价**：
- `DbValue` 从 320 字节涨到 4KB ⇒ **每个值都背着 4KB**。
  一次 10 万行 × 4 列的扫描 = 1.6GB 的清零/拷贝开销。
  （`value.c` 的头注释里记着这个性能账：`val_reset` 改成"只清必要字段"
  就是为了省这 256 字节的清零。）
- 这会**破坏整个值层的设计前提** —— 值层刻意让 DbValue 保持小。

⇒ **不推荐。** 代价与收益完全不成比例。

### 路线 C：TEXT 保持 255B 上限（当前状态）

**做法**：什么都不改。

**依据**：
- SQLite 的 TEXT 也没有长度上限的说法，但我们的值层是"小值内联、大值外部"的结构，
  TEXT 没有外部落点 ⇒ 255B 是当前架构下的自然上限。
- 真正需要长文本的场景，应该用 **blob 模态**（`blob_frames`），那是它的本职。
- 显式报错（不是静默截断）已经足够好：用户知道"这条放不进 TEXT"，
  而不是拿到一个被改过的值。

⇒ **推荐：保持现状。**

## 4. 若将来要做（路线 A）的改动清单

供将来参考，不在本轮实施：

| # | 改动 | 位置 | 难度 |
|---|---|---|---|
| 1 | 页链机制落地 | `modalities/doc/json_ovf.c` | 大（C5.3b-b） |
| 2 | 抽出通用的溢出页链（载荷不透明） | 新文件或 `json_ovf.c` 改名 | 中 |
| 3 | record 编码支持 TEXT 的溢出标记 | `storage/record.c`、`value_record.c` | 中 |
| 4 | `val_to_record` / `val_from_record` 处理 TEXT 外部值 | `storage/value_record.c` | 中 |
| 5 | `coerce_to_col` 放开 255 限制 | `query/exec_write.c` | 小 |
| 6 | 批解码路径处理 TEXT 外部值 | `storage/batch.c`、`query/exec_batch.c` | 中 |
| 7 | 索引：TEXT 列建索引时的 key 编码 | `index/index_key.c` | 中 |
| 8 | VACUUM 回收 TEXT 链 | `txn/vacuum.c` | 中 |
| 9 | 测试 | 新增 | 中 |

⇒ **约 8 处改动，跨越值层、记录层、执行层、索引层、事务层。**
这不是小活，必须排在 C5.3b-b 之后。

## 5. 当前的对外契约（写进文档）

**TEXT 列的值超过 255 字节时，`INSERT` / `UPDATE` 报 `E_NOSPACE`，
错误文本说明 "列 X：文本超过 255 字节上限"。不写 heap，不留半行。**

需要长文本请用 blob 模态（`blob_frames` / `blob_objects`）。

## 6. 与已有文档的关系

- `test/unit/test_blob_am.c` 与 `test/integration/test_spi_it.c` 的注释已同步为
  "TEXT 列上限 255 字节"（C5.3b-a 提交 `0d4471143`）。
- `MODALITIES.md` 的坑 M7-36 记录了"外部 T_TEXT 进记录写成空串"这个坑
  及其防线；本文档记录的是**另一个问题**（TEXT 能不能更长）。

## 7. 未决与风险

1. **用户可能期望 TEXT 能存长文本**。这是产品期望问题，不是技术问题。
   如果用户反馈说"我的 4KB 文本存不进去"，第一反应应该是"用 blob 模态"，
   而不是"改值层"。
2. **路线 A 与 C5.3b-b 高度耦合**：两者共用同一套页链机制。
   如果 C5.3b-b 迟迟不落地，路线 A 也就无从谈起。
