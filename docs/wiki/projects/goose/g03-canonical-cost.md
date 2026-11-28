---
project: goose
feature: canonical-cost
title: 代价解析与来源优先级
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/providers/canonical_cost.rs
    symbols: [estimate_model_cost, merge_pricing, pricing_from_model_info]
  - path: crates/goose/src/providers/canonical_cost.rs
    symbols: [is_price_gap, full_pricing_model, bundled_model_info]
---

# 代价解析与来源优先级

## 一句话职责

回答「这次调用花了多少钱」：按四级优先级解析出单价（provider 自报 > 用户声明 > 规范目录 > 内置声明），再按用量算出金额；其中每一级都有明确的「何时该被下一级填补」的判据。

## 机制是什么

模块的文档注释直接给出了完整的优先级表，从高到低：

1. **provider 自报的代价**（由调用方在本模块之前处理）。
2. **用户在自定义 provider 配置文件里声明的价格**——注释解释了理由：用自定义端点的人（谈好价、走网关、自托管）比按名字匹配到的目录条目更清楚真实价格。
3. **内置的规范注册表**（canonical registry，来自 models.dev 那条目录链）。
4. **内置声明式 provider 定义里写的价格**——这一级只填注册表的空档，因为内置定义是随包发布的、可能落后于注册表同步。

一段补充说明很重要：注册表通过**跨 provider 名字推断**得到的非零价格，仍然优先于内置声明价；而「推断匹配到一条免费条目」得出 0 或空，则算作空档，由下一级填。最后一句还说，若最终胜出的来源没声明缓存价格，规范缓存费率会被保留，避免按全价输入费率把缓存 token 估贵。

## 怎么实现的

**入口是两个，语义一致。** `estimate_model_cost(provider, model, usage)` 只做「解析单价 → 按用量算」。`resolve_usage_cost(provider, usage)` 先看 `usage.cost` 是否已有值，有就直接返回 `ProviderReported`；否则调 `estimate_model_cost` 并标记为 `Estimated`；都没有则返回两个 `None`。

注释说明后者的用途：两个 agent 循环都调它，所以「估算代价」与「provider 自报代价」在两条路径上行为一致。返回的 `CostSource` 让下游能区分「这是真的账单」还是「这是我们按公开价估的」——这在成本统计里是必要的，否则两种数字会被混在一起算平均。

**四级解析是两段合并。** `resolve_pricing` 分两步：

第一步把「规范注册表」与「内置声明式定义」合成一个 `base`：

- 两者都有：逐字段看规范值是否是空档（`is_price_gap`），是就填内置值。`cache_read` 与 `cache_write` 则用 `Option::or`——规范有就用规范的，没有才用内置的。这一步只补 `input` 与 `output` 两个字段的空档。
- 只有规范：直接用规范。
- 只有内置：用内置。

第二步把「用户配置文件声明」与 `base` 合成最终结果：

- 两者都有：`merge_pricing(declared, &base)`。
- 只有声明：用声明。
- 只有 base：用 base。

所以「用户声明」不是在 base 上叠加，而是**先占位再补空**。这个方向不能反：若先算 base 再让声明覆盖，则声明里缺失的字段会保留 base 值（行为相同），但声明里的零价会被 base 的非零值覆盖——`merge_pricing` 正是为了防这个。

**`is_price_gap` 只认两种形态：`None` 与 `Some(0.0)`。** 注释说明理由：「注册表里未设置或为零的价格提供了不可用的费率信号，例如按名字推断出的跨 provider 匹配撞上一条免费条目」。

注意这把「合法的零价」与「缺失」混为一谈了。0 在 `output` 侧是合理的（例如某些输入免费、输出收费的条目），但在这里 0 一律被当作空档。这是有意的：在目录数据里，0 更常见的是「这条记录来自一个免费服务，但该模型本身并非免费」这种推断失误。

**`merge_pricing` 的两条规则。** 第一，若声明的 `input` 与 `output` **都**是 `Some(0.0)`，直接返回声明，**不再补任何东西**。注释解释了这条：用户声明「这里免费」时，注册表的缓存费率不能把缓存 token 的成本重新引入。

第二，否则只补 `cache_read` 与 `cache_write` 两个空档（`is_none` 才填），`input`/`output` 一律用声明值。

所以合并的方向是：**声明的输入输出必胜，缓存费率只在声明没说时补**。第一条规则是这条规则的例外——当声明说的是「全免费」时，连缓存费率也不补，否则「免费」就不成立。

**`pricing_from_model_info` 的单位转换。** 它把声明式的 `ModelInfo`（每 token 的美元价）转成 `Pricing`（**每百万 token** 的美元价），做 `* 1_000_000.0`。

`input` 与 `output` 都用 `Option::map(...)?`——注意那个 `?`：任一为 `None` 整个函数返回 `None`。所以「只声明了输入价没声明输出价」不会被当成有效定价。注释在 `configured_model_info` 上写明了理由：「缺一侧的定价无法正确估算，会静默地把某个方向按零算」。

而 `cache_read`/`cache_write` 用普通的 `map`，缺失就是 `None`。这两项是可选的——没有缓存价就按无缓存算。

**`full_pricing_model` 的过滤。** 它先在 config 的 `models` 里按名字找，然后 `.filter(|info| info.input_token_cost.is_some() && info.output_token_cost.is_some())`。这是同一道「两侧都要有」的检查，只是做在查找阶段而不是转换阶段。

**两类定义的生命周期不同。** `custom_file_model_info` **每次调用都读盘**——注释写「reads are live: price edits take effect without a restart」。它读文件、反序列化，任何一步失败就记一条警告并返回 `None`（「cost fallback disabled」），所以坏配置文件不影响主流程，只是让价格回落到目录。

`bundled_model_info` 则用 `static CONFIGS: OnceLock<Vec<DeclarativeProviderConfig>>` 缓存——注释说内置定义是「embedded and immutable」，所以进程内只解析一次。

**币种显示是另一条独立路径。** `display_currency(info)` 拿声明里的 `currency` 字符串，过 `currency_symbol` 映射：空或 `USD` → `$`，`EUR` → `€`，`GBP` → `£`，`JPY` → `¥`，其他原样返回。

注释解释了这个映射的必要性：客户端把货币**当作符号逐字打印**，而配置文件里习惯写 ISO 代码，不映射就会出现「USD0.01」这种渲染。同时「其他原样返回」保证了不认识的币种也能显示（虽然可能不是符号）。

**测试里的探针值得看。** 有一组测试断言具体 provider/model 的费率，例如 `chatgpt_codex` 的 `gpt-5.6-sol` 应当与 `openai` 的同一模型费率完全相同——这验证了「别名 provider 走同一份目录行」。还有 `azure_foundry` 的测试断言它对 Azure 与开源权重模型用**宿主费率**而非上游费率，以及「未知模型没有估算」返回 `None`。

另外 `assert_cost` 这个辅助函数的注释说明了它的判据：「`expected` 读作该模型的基础输入加输出费率（每百万 token）；提示长度保持低于所有长上下文档位」——所以它刻意避开分层定价（`tiers`）的影响。

## 关键取舍

**源码事实：** `pricing_from_model_info` 要求 `input` 与 `output` 两侧都有价，任一缺失整个返回 `None`。缓存价是可选的。

**源码事实：** `is_price_gap` 把 `Some(0.0)` 与 `None` 等价处理。所以「合法零价」在目录侧无法表达。

**源码事实：** `merge_pricing` 在「声明输入输出都为 0」时**完全短路**，不补缓存费率。

**源码事实：** 用户配置文件每次调用都重新读盘，内置定义进程内缓存一次。

**源码事实：** `resolve_usage_cost` 返回的 `CostSource` 区分 `ProviderReported` 与 `Estimated`。

**架构推论：** 四级优先级的排序依据是「谁更可能知道真相」。provider 自报最准（它有账单）；用户声明次之（他知道自己谈的价）；公开目录再次（它是第三方整理的公开信息）；内置声明最后（它是随包发布的快照，必然落后）。这个排序不是按「谁更新」而是按「谁离这笔钱的真相更近」。

**架构推论：** 「只填空档而不覆盖」这一贯穿全程的模式，是这个模块最一致的设计原则。`is_price_gap`、`Option::or`、`is_none()` 检查、以及那条「全零短路」例外，全部围绕「高优先级来源没说时才让低优先级说话」。若改成「高优先级无条件覆盖」，则用户声明里遗漏的缓存价会丢失目录里的准确值。

**架构推论：** 「两侧都要有价」的检查放在两处（查找阶段与转换阶段），说明这个约束被当作不变式而非可选优化。它的代价是「只想声明输入价」的场景无法表达——但在成本估算里，只有输入价确实无法算出任何有意义的总价，所以这个限制是合理的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/providers/canonical_cost.rs` | `estimate_model_cost` | 解析单价并按用量算金额 |
| `crates/goose/src/providers/canonical_cost.rs` | `resolve_usage_cost` | 自报优先，否则估算，附来源标记 |
| `crates/goose/src/providers/canonical_cost.rs` | `resolve_pricing` | 四级优先级的两段合并实现 |
| `crates/goose/src/providers/canonical_cost.rs` | `merge_pricing` | 声明值必胜、缓存费率补空、全零短路 |
| `crates/goose/src/providers/canonical_cost.rs` | `pricing_from_model_info` | 每 token 转每百万，两侧必须有价 |
| `crates/goose/src/providers/canonical_cost.rs` | `is_price_gap` | 把 `None` 与 `Some(0.0)` 都判为空档 |
| `crates/goose/src/providers/canonical_cost.rs` | `full_pricing_model` | 按名字找完整定价的模型条目 |
| `crates/goose/src/providers/canonical_cost.rs` | `bundled_model_info` | 内置定义进程内缓存一次 |
| `crates/goose/src/providers/canonical_cost.rs` | `custom_file_model_info` | 用户配置文件每次读盘 |
| `crates/goose/src/providers/canonical_cost.rs` | `display_currency` | ISO 代码映射为符号后再渲染 |

## 参考

- [goose 源码：providers/canonical_cost.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/providers/canonical_cost.rs)
- [goose 文档：成本追踪](https://block.github.io/goose/docs/guides/usage-tracking/)
- [models.dev 价格目录](https://models.dev/)

## 与主核 / AI 的关系

主核里最接近的是「代价估算器的统计来源优先级」：优化器估算一个算子要花多少 I/O，依据可以是真实统计（精确）、采样的近似统计、或者从表定义推断的默认值。当高层来源缺失时，低层来源填补；当高层来源给了值，低层不能覆盖它。`resolve_pricing` 的两段合并与这个模式几乎逐条对应。

与 smolagents 的用量统计对比，差别在于「有没有成本这一维」。smolagents 的 `TokenUsage` 只有三个整数计数，`Monitor` 累加它们并打印——它不关心每个 token 多少钱，因为框架自己不计费。goose 作为要展示「这次会话花了多少」的产品，必须把 token 数翻译成金额，于是就出现了这一整套优先级与空档填补逻辑。

对 AI 工程最实用的一条是「只填空档不覆盖」加「全零短路」这个组合。前者避免了「高层来源缺失导致低层数据丢失」，后者避免了一个具体的语义错误：用户说「这里免费」，而系统从目录里补上缓存费率，于是「免费」变成了「部分收费」。这两条规则合在一起，才能让「多个来源描述同一件事」的结果既完整又符合最高优先级来源的意图。任何多来源合并的场景都该有这两条。

另一条是「两侧必须有价」的严格性。看似不灵活，实际是在防一类静默错误：只声明输入价时，若把输出价按 0 算，总价会严重偏低，而用户完全看不出来。宁可返回 `None`（表示「无法估算」），也不要返回一个错的数字——因为下游通常无法区分「估算为 0」与「估算失败」，而「估算失败」至少会显示为空而不是错误的金额。
