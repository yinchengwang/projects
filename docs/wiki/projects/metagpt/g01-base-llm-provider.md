---
project: metagpt
feature: base-llm-provider
title: 模型基类与成本计费
page_type: feature
schema_version: 1
refs:
  - path: metagpt/provider/base_llm.py
    symbols: [BaseLLM]
---

# 模型基类与成本计费

## 一句话职责

`BaseLLM` 是所有模型提供方的公共父类：它把「拼消息、发请求、抽文本、记花费」这四件事拆成可覆写的钩子与固定的公共流程，子类只需实现三处真正与厂商相关的调用。

## 机制是什么

继承 `BaseLLM` 的类（OpenAI 系、Claude、Gemini、通义、Ollama 等二十余个）都不需要自己处理「系统提示词该不该带」「图片怎么拼」「流式与非流式怎么统一」「这次调用花了多少钱」这些事。基类提供：

- **消息构造层**：`_system_msg` / `_user_msg` / `_assistant_msg` 三个私有构造函数，`format_msg` 负责把 `str`、`dict`、`Message`、以及它们的列表归一成 OpenAI 风格的 `list[dict]`。
- **公共请求层**：`aask` 是所有人用的入口，它拼消息、脱敏日志、可选压缩，最后落到一个抽象方法。
- **五个抽象/可覆写钩子**：`__init__`、`_achat_completion`、`acompletion`、`_achat_completion_stream` 是 `@abstractmethod`；`aask_code` 默认抛 `NotImplementedError`；`count_tokens` 有一个粗糙的默认实现，鼓励覆写。
- **成本层**：`_update_costs` 与 `get_costs`，把每次调用的 token 数与单价换算成累计花费。

## 怎么实现的

**`aask` 的消息装配顺序。** 这是整套契约的核心，顺序决定了提示词的形状：

1. 传了 `system_msgs` 就用它们构造系统消息；没传就用默认的 `self.system_prompt`（"You are a helpful assistant."）。
2. 若 `use_system_prompt` 为假，把上一步的结果**整体清空**。这条是为了 o1 这类不接受 system role 的模型——注意它连用户显式传的 `system_msgs` 也一起丢掉。
3. 追加 `format_msgs`（预先构造好的消息列表）。
4. 追加 `msg`：字符串走 `_user_msg`（可带图片），否则当作消息列表 extend。
5. `stream` 为 `None` 时回落到 `self.config.stream`。
6. 打日志前对每条消息做 `mask_base64_data`，把 base64 图片替换成占位串。
7. 可选 `compress_messages` 压缩，然后调 `acompletion_text`。

**`acompletion_text` 是唯一的出口。** 它同时是重试点：装饰器写死了「最多 3 次、随机指数退避 1 到 60 秒、仅对 `ConnectionError` 重试、重试耗尽后走 `log_and_reraise`」。流式与非流式的分叉也在这里：`stream=True` 调 `_achat_completion_stream` 并直接返回它，`False` 调 `_achat_completion` 拿到原始响应再 `get_choice_text` 抽取文本。

**`get_choice_text` 顺带捕获推理内容。** 它取 `choices[0].message.content`，但如果该 message 里带 `reasoning_content` 字段，就先写进 `self.reasoning_content`（一个带 property 与 setter 的私有字段）。因此「推理模型的思考过程」不作为返回值，而是作为实例上的一个可读属性暴露——同一次 `aask` 之后再读 `reasoning_content` 就是这一轮的推理。

**超时的三级回落。** `get_timeout(timeout)` 返回 `timeout or self.config.timeout or LLM_API_TIMEOUT`。因此「调用点显式指定」优先于「配置的默认」，最后才是常量兜底。

**成本计算的三级回落。** `_update_costs` 里计费模型名的优先级是 `model 参数 or self.pricing_plan or self.model`。`pricing_plan` 存在的意义是：某些部署通过代理访问，实际 model 名与计费表里的名字不同，需要单独指定用哪个价格。计费开关是 `self.config.calc_usage and local_calc_usage` 两个条件的合取——后者让特定调用点可以临时关掉本地估算（有些模型自己返回 usage，不需要再算）。

实际累加交给 `CostManager.update_cost`：token 数为 0 或模型名为空直接返回；模型名不在价格表里只记 warning 然后返回（此时 token 数已经累加进去了，但金额没加——这是一个细微的不一致）；单价按「每千 token」计价，`(prompt * 单价 + completion * 单价) / 1000`。

`get_costs()` 在 `cost_manager` 为空时返回全零的 `Costs`，而不是 `None`。这让「取花费」永远可调用。

**`format_msg` 按类型四路分支。** `str` → 用户消息；`dict` → 断言键集合恰为 `{role, content}` 后原样使用；`Message` → 若 metadata 里有 `IMAGES` 就用多模态构造，否则 `to_dict()`；其他类型抛 `ValueError`。断言用 `assert`，所以优化模式下这层校验会消失。

**多模态构造。** `support_image_input()` 判断 `MULTI_MODAL_MODELS` 里是否有子串出现在 `self.model` 中（子串匹配，不是精确匹配）。`_user_msg` 只有在「有图片**且**支持图片输入」时才走多模态分支，否则静默降级成纯文本——图片被丢弃，不报错。`_user_msg_with_imgs` 把非 http 开头的图片当作 base64 补上 `data:image/jpeg;base64,` 前缀。

**`compress_messages` 的四种策略。** `NO_COMPRESS` 直接返回。其余四种分成两个语义维度：`POST_*` 保留**最新**的消息、`PRE_*` 保留**最早**的消息；`*_BY_TOKEN` 允许把放不下的那条按 token 截断、`*_BY_MSG` 直接丢弃整条。系统消息无条件全保留（注释假设它们不会超限）。token 上限优先从 `TOKEN_MAX` 表按模型名取，取不到才用默认的 128000；再乘 `threshold`（默认 0.8）留出回复余量。

系统消息与后续消息的分界用一个 `for ... break` 扫描实现：遇到第一条非系统消息就把它之后的所有消息当作 `user_assistant_msgs`。这意味着「中途插入的 system 消息」会被归入后半段，而不是被当作系统消息保留。

**`count_tokens` 的默认实现是粗估。** 它对每条消息取 `len(content) * 0.5` 取整后求和，注释明确说这是「对英文的严重高估，应被子类用精确实现覆写」。压缩策略依赖这个数字，所以不覆写的子类上压缩行为会偏保守。

**`aask_batch` 是串行多轮。** 它把每个问题依次拼进上下文、发请求、把回答作为 assistant 消息追加，最后把所有 assistant 内容用换行拼接返回。也就是说返回值不是「最后一轮的回答」，而是「每一轮回答的拼接」。

## 关键取舍

**源码事实：** `use_system_prompt=False` 会清掉调用方显式传入的 `system_msgs`。这是「模型不支持 system role」的实现方式，代价是调用方无法通过这个参数绕过——想给 o1 加约束只能把内容塞进用户消息。

**源码事实：** 只有 `ConnectionError` 会触发重试。服务端返回 429、超时、5xx 都不在重试范围内，需要子类自己包一层。重试次数上限硬编码为 3。

**源码事实：** 计费模型名有三处回落，`pricing_plan` 优先于 `model`。而「模型不在价格表」时 token 数已累加、金额未累加，导致 `total_prompt_tokens` 与 `total_cost` 的口径可能不一致。

**源码事实：** 图片支持判定用的是「模型名包含某个子串」，而非白名单精确匹配。因此新增多模态模型只需在常量列表里加一项，但也可能因为模型名撞上子串而误判。

**源码事实：** 消息压缩的系统消息识别是靠「从头扫到第一条非系统消息就停」，而不是全量分类。插入在中间的 system 消息会被当作普通消息参与裁剪。

**架构推论：** 基类把「与厂商相关的部分」压缩到三个抽象方法（非流式调用、流式调用、构造），其余全部共享。这让二十多个提供方只写几十行适配代码，代价是任何公共行为的调整（例如新增一种压缩策略）都会同时影响所有提供方——共享是增益也是耦合。

**架构推论：** 成本是「本地估算」而不是「以后端回报为准」。`_update_costs` 依赖调用方传入的 usage 字典，若某个子类拿不到 usage 就完全不记账。因此整套成本数据在异构部署下不可直接横向比较，只适合「同一提供方内部的预算控制」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/provider/base_llm.py` | `BaseLLM` | 消息装配、请求出口、成本与压缩的公共实现 |
| `metagpt/provider/base_llm.py` | `BaseLLM.aask` | 系统/用户/预置消息的装配顺序与开关 |
| `metagpt/provider/base_llm.py` | `BaseLLM.acompletion_text` | 唯一出口，含 3 次仅 ConnectionError 的重试 |
| `metagpt/provider/base_llm.py` | `BaseLLM.get_choice_text` | 抽文本并顺带捕获 `reasoning_content` |
| `metagpt/provider/base_llm.py` | `BaseLLM._update_costs` | 三级模型名回落与计费开关 |
| `metagpt/provider/base_llm.py` | `BaseLLM.get_timeout` | 显式、配置、常量的三级超时回落 |
| `metagpt/provider/base_llm.py` | `BaseLLM.format_msg` | 四种输入类型归一为消息列表 |
| `metagpt/provider/base_llm.py` | `BaseLLM.mask_base64_data` | 日志脱敏：base64 图片替换为占位串 |
| `metagpt/provider/base_llm.py` | `BaseLLM.compress_messages` | 前后保留 × 截断丢弃的四种压缩策略 |
| `metagpt/provider/base_llm.py` | `BaseLLM.count_tokens` | 粗略 token 估算，鼓励覆写 |
| `metagpt/provider/base_llm.py` | `BaseLLM.support_image_input` | 按模型名子串判定多模态能力 |

## 参考

- [MetaGPT 源码：provider/base_llm.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/provider/base_llm.py)
- [MetaGPT 源码：utils/cost_manager.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/utils/cost_manager.py)
- [OpenAI 文档：Chat Completions 消息格式](https://platform.openai.com/docs/api-reference/chat)

## 与主核 / AI 的关系

「把与后端相关的部分收敛成少数几个钩子、其余流程共享」与主核里「存储引擎只实现页读写与刷盘、缓冲池与日志逻辑共享」是同一种分层：主核的引擎接口很窄（读页、写页、刷盘），因此适配新引擎只写少量代码；这里的钩子是「非流式调用、流式调用、构造」，因此适配新厂商也只写少量代码。两者都把「公共复杂度」锁在一处，代价是公共部分的任何 bug 会一次性影响所有后端。

对 agent 工程最实际的一条是成本：`BaseLLM` 的成本是**本地按单价表估算**，且模型名不在表里时只加 token 不加金额。因此拿它做预算控制要保证「实际模型名命中价格表」，否则会出现「token 在涨、花费不动」的假象，多角色团队的成本上限（`max_budget`）也会因此失效。

