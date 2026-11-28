---
project: ragas
feature: multimodal-metrics
title: 多模态指标
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/metrics/collections/multi_modal_faithfulness/metric.py
    symbols: [MultiModalFaithfulness]
  - path: src/ragas/metrics/collections/multi_modal_relevance/metric.py
    symbols: [MultiModalRelevance]
---

# 多模态指标

## 一句话职责

多模态忠实度与多模态相关性：上下文列表里允许混入图像（本地路径、URL、base64），指标把每一条按类型拆成「文本块」或「图像块」后交给视觉模型，判定仍是一句二值结论。

## 机制是什么

两个指标在语义上与纯文本版对应：忠实度问「答案里的话能不能从上下文推出来」，相关性问「答案有没有答到问题且与上下文对得上」。多模态版换了三处：

1. **输入形态变了**。不再是「提示字符串」，而是 OpenRouter 风格的消息内容块列表——一个 `type: "text"` 的块加一串 `type: "image_url"` 的块。
2. **调用路径绕开了提示层**。纯文本指标走 `llm.agenerate(prompt_str, OutputModel)`；多模态版**直接调底层 client**，把消息内容块原样传下去，因为结构化输出的 `agenerate` 不接受内联图像。
3. **上下文条目要做类型探测**。同一条 `retrieved_contexts` 里，字符串既可能是纯文本也可能是图片路径，必须先判类型再决定怎么塞进消息。

判定本身没有变细：仍然是「faithful / not faithful」或「relevant / not relevant」一句布尔结论，不拆陈述。这是刻意的——拆陈述需要模型逐句引用图像内容，可靠性远低于整体判断。

## 怎么实现的

**类型探测的三条路。** `process_image_to_base64` 按顺序尝试三种来源，任一成功即返回 `{"mime_type", "encoded_data"}`：

- **base64 data URI**：用一条正则匹配 `data:image/(png|jpeg|gif|webp);base64,<数据>`，匹配到还要真解一次 base64 验证合法性。这条最便宜，不触网。
- **URL**：scheme 必须是 http/https；下载走 `fetch_url_bytes`，带超时、大小上限（10 MB）与内网地址拦截；拿到字节后**用 PIL 校验**：先 `Image.open(...).verify()` 确认是完整图像，再重开一次读 `.format` 得到真实格式，然后用**真实格式**而不是 URL 后缀拼 mime 类型。这一步很关键——URL 后缀可以撒谎，`img.png` 可能是 JPEG。
- **本地文件**：默认**关闭**。要启用必须同时设置 `ALLOW_LOCAL_FILE_ACCESS` 与 `ALLOWED_IMAGE_BASE_DIR`，且路径要经 `realpath` 解析后与基准目录做 `commonpath` 比较，越界即拒绝。这是显式的路径穿越防护，不是靠后缀白名单。

三条都不成立就返回 `None`，该条目被当作文本处理。

**消息内容的拼装顺序。** `build_multimodal_message_content` 产出的是一个线性块列表，顺序固定：先一个文本块（指令 + 待评答案 + 「Retrieved contexts:」字样），然后逐条上下文——是图就追加一个 `image_url` 块**紧跟**一个标注块「[Image context N]」，是文本就追加一个「Context N: ...」块，最后再加一个收尾文本块补全判定标准。

「图像块 + 标注块」成对出现是必要的：模型看到两段图像时无法在文本里区分「第一张」与「第二张」，靠一个紧邻的文本标注把序号绑上去。代价是标注本身也在上下文里，模型可能把「[Image context 1]」当成内容的一部分。

**相关性版多了一个问题。** 它的拼装函数多收一个 `user_input` 并写进首个文本块，其余结构与忠实度版逐字同构。两个 `build_*` 函数是两份独立实现，而不是一个带开关的共用函数——忠实度版不需要问题，相关性版需要，参数差异让复用不如复制直接。

**直连 client 的两条分支。** `_evaluate_faithfulness` / `_evaluate_relevance` 都做了 provider 判断：`provider` 小写为 `google` 时调 `client.create(...)`，否则调 `client.chat.completions.create(...)`。两个调用都带 `response_model=<输出模型>`，也就是把结构化输出交给 instructor 的客户端包装。`provider_kwargs` 由 `llm._map_provider_params()` 提供，用于抹平不同供应商的参数名差异。

这也是本组指标与其他 collections 指标最大的实现差异：**它们依赖 `InstructorLLM` 的具体内部结构**（`.client`、`.model`、`.provider`、`_map_provider_params`），而不是抽象的 `agenerate` 接口。换句话说，换成另一个只实现了 `agenerate` 的模型包装，这两个指标就跑不了。

**输入校验。** 忠实度版要求 `response` 与 `retrieved_contexts` 都非空；相关性版额外要求 `user_input` 非空。缺任一项直接 `ValueError`，不做降级。这与 b01 里「空陈述返回 NaN」的取向不同：这里把缺参当成配置错误（硬失败），而不是当成无数据（软返回）。

**判定输出。** 两个输出模型各有一个布尔字段（`faithful` / `relevant`）与一个默认为空串的 `reason`，指标把布尔转成 1.0/0.0 并把 reason 带进结果。所以分数尺度与纯文本的忠实度**不一样**：那边是「被支持陈述的比例」，这边只有一个 0 或 1。同一份评测里混用两版指标，聚合出来的均值含义会不一致。

## 关键取舍

**源码事实：** 多模态版放弃了「分解 → 逐项判定 → 聚合」的三段式，退化成单次整体判定。分数从比例变成二值。原因是图像无法像文本那样被切成可逐条引用的原子陈述。

**源码事实：** 图像 mime 类型以 PIL 探测的真实格式为准，不信任 URL 或文件后缀。这一条同时挡住了「伪装后缀」与「非图像文件」。

**源码事实：** 本地文件读取默认关闭，需要显式开两个开关（总开关 + 白名单目录），且路径用 `realpath` 后做 `commonpath` 校验。URL 抓取则默认开启但受内网拦截与 10 MB 上限约束。

**源码事实：** 两个指标直接调 `client.create` / `client.chat.completions.create`，而不是走 `agenerate`。这让它们与 `InstructorLLM` 的内部字段强耦合。

**架构推论：** 「图像块 + 紧邻标注块」的顺序约定，本质是用文本序号弥补多图无法自标识的缺陷。这条路在两张图时还清楚，图像一多，标注文本占的上下文就不可忽略，且模型可能把标注误当内容。更彻底的做法是给每张图配一个结构化 id，但那要求模型侧支持引用。

**架构推论：** 输入校验全部硬失败而不降级，是因为多模态评测的成本远高于文本——一次带图的调用可能比纯文本贵一个数量级。宁可早失败，也不要花着昂贵的调用得出一个「因为缺上下文所以判不了」的 0 分。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/metrics/collections/multi_modal_faithfulness/metric.py` | `MultiModalFaithfulness` | 多模态忠实度主体，含输入校验与直连 client |
| `src/ragas/metrics/collections/multi_modal_faithfulness/metric.py` | `_evaluate_faithfulness` | 按 provider 分两条调用路径 |
| `src/ragas/metrics/collections/multi_modal_faithfulness/util.py` | `build_multimodal_message_content` | 文本块与图像块的有序拼装 |
| `src/ragas/metrics/collections/multi_modal_faithfulness/util.py` | `process_image_to_base64` | 三种来源的类型探测与编码 |
| `src/ragas/metrics/collections/multi_modal_faithfulness/util.py` | `_try_process_url` | SSRF 拦截、大小上限与 PIL 格式校验 |
| `src/ragas/metrics/collections/multi_modal_faithfulness/util.py` | `_try_process_local_file` | 目录白名单与路径穿越防护 |
| `src/ragas/metrics/collections/multi_modal_relevance/metric.py` | `MultiModalRelevance` | 多模态相关性主体 |
| `src/ragas/metrics/collections/multi_modal_relevance/util.py` | `build_multimodal_relevance_message_content` | 多带一个问题的拼装版本 |

## 参考

- [ragas 源码：multi_modal_faithfulness/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/multi_modal_faithfulness/metric.py)
- [ragas 源码：multi_modal_relevance/metric.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/metrics/collections/multi_modal_relevance/metric.py)
- [ragas 文档：多模态评测](https://docs.ragas.io/en/stable/concepts/metrics/available_metrics/multi_modal/)
- [OpenAI 文档：图像输入格式](https://platform.openai.com/docs/guides/vision)

## 与主核 / AI 的关系

这一页与主核的交集不在「模态」，而在「**外部输入的类型判定与安全边界**」——这恰好是主核里模态层最头疼的部分。

**后缀不可信，内容才可信。** 这里用 PIL 真正解一次来确认格式，而不是看路径后缀。主核在加载外部文件时同样必须靠魔数校验而非扩展名——**「文件叫什么」和「文件是什么」是两件事**，任何依赖前者做安全判断的地方都会被绕过。

**SSRF 与路径穿越是同一类问题。** URL 抓取要拦内网，本地文件要拦目录外——两者都是「调用方给的名字可以指向不该去的地方」。这里的做法是白名单（scheme 白名单 + 目录白名单）加解析后校验（`realpath` + `commonpath`），而不是黑名单。白名单之所以更可靠，是因为「允许什么」是可枚举的，「不允许什么」永远列举不全。

**「便宜来源先试」的顺序设计。** base64 → URL → 本地文件，越往后越贵、越不可控。这与主核对页缓存的查找顺序一致：先本地、再远程、最后生成。**代价随尝试顺序递增**是一条通用的工程原则。

对 AI 工程最实用的三条：其一，**图像上下文的 mime 类型必须由内容决定**，否则部分供应商会拒收，错误信息还指不到真正原因。其二，**多图时必须给出可区分的标注**，否则模型无法在文本里引用第几张图。其三，**多模态指标的分数尺度与文本版不兼容**（二值 vs 比例），同一份报告里若两者混排，聚合均值必须分开算，不能相加或平均。
