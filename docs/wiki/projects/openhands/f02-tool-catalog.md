---
project: openhands
feature: tool-catalog
title: 工具目录校验
page_type: feature
schema_version: 1
refs:
  - path: src/api/tool-catalog-service/tool-catalog-service.api.ts
    symbols: [isToolCatalogEntry, assertValidCatalog, ToolCatalogService]
---

# 工具目录校验

## 一句话职责

从 agent server 取回「这台服务器提供哪些工具」的清单，并在**交出之前逐条校验**：只要有一条形状不对，就整体拒绝，绝不让界面拿一份半可信的目录去决定「哪些工具可以选择」。

## 机制是什么

整个模块只有 54 行，三个部件：

| 部件 | 作用 |
|---|---|
| `ToolCatalogEntry` | 从 SDK 重导出的类型：`name`、`user_selectable`、`usable`、`in_default_set`、`description` |
| `isToolCatalogEntry` | 运行期类型守卫，逐字段检查五个必填项 |
| `assertValidCatalog` | 断言函数，遍历一整批并在失败时抛错 |
| `ToolCatalogService.getCatalog` | 唯一入口：取响应 → 检查 `tools` 是数组 → 逐条断言 → 返回 |

它不缓存、不重试、不归一化——`getCatalog` 的 docstring 只有一句：**这台服务器提供的工具，按它自己的顺序**。

## 怎么实现的

**「拒绝整份目录」而不是「过滤掉坏条目」，这是本页的核心决定。** `assertValidCatalog` 抛错而不是跳过。错误文案给出了理由的一截：`refusing to pick tools from it`——注意是 **pick**（挑选），不是「展示」。也就是说这份目录的用途是**让用户或上层挑选工具**，而在一份部分条目不可信的清单上挑选，会把「这个工具存在」这个前提变成猜测。

**报错里尽量给出定位信息。** 失败时它从坏条目里取 `name`，取不到就用 `"<missing>"`；若坏条目连对象都不是（`null` 或字符串等），用 `"<malformed>"`。这两种兜底文案的区别说明它区分了「有对象但缺 name」与「根本不是对象」两种畸形——在排查「上游版本变了」时这个区别很有用。

**校验只看五个字段，且全部是「是否存在且类型正确」，不检查取值域。** `isToolCatalogEntry` 检查 `name` 是 `string`、三个布尔字段是 `boolean`、`description` 是 `string`。它**不**检查 `name` 非空、不检查 `description` 非空、不检查有没有额外字段。

这是一个刻意的宽严分界：**结构错会让后续逻辑崩，取值错只会显示得难看**。选择一个永远为空的名字是用户界面上难看，但不会让代码走错分支；而一个 `user_selectable` 是字符串 `"true"` 的条目会让「过滤可选工具」这一步悄悄给出错误结果。

**`response?.tools` 用了可选链加 `Array.isArray` 的两次检查。** 先 `const tools: unknown = response?.tools`（把类型主动放宽成 `unknown`），再 `if (!Array.isArray(tools))` 抛一条独立的错误（`missing the tools array`）。这与逐条断言的错误是两条不同消息，因为它们对应两种不同的上游故障：**响应整体形状不对**（可能是路由错了、SDK 版本不匹配）与**某一条条目不对**（可能是服务器返回了部分构造失败的条目）。

**`asserts tools is ToolCatalogEntry[]` 的断言签名让类型收窄发生在调用点。** `assertValidCatalog` 的返回类型标注是 `asserts tools is ToolCatalogEntry[]`（TypeScript 的断言函数签名）。这让 `getCatalog` 里那句 `assertValidCatalog(tools); return tools;` 无需任何强制转换或类型断言——`unknown[]` 在断言之后就是 `ToolCatalogEntry[]`。

这是本页唯一的技术亮点：**校验与类型收窄用同一个动作完成**，所以「校验了但忘了窄化」或「窄化了但没校验」这两类错误在编译期就不可能发生。

**`getCatalog` 是 `static async`，且每次都新建 `ToolClient`。** 没有单例、没有复用——它拿 `getAgentServerClientOptions()` 的当前值新建一个客户端。这个选择与 `b03`（bash 服务）一致：客户端选项依赖**当前活跃后端**，而活跃后端可以在会话中途切换，所以缓存客户端会把旧后端带进新请求。

**整个模块没有把 `ToolCatalogEntry` 的字段映射成内部模型。** `export type { ToolCatalogEntry }` 直接把 SDK 的类型再导出，服务返回的也是 SDK 类型本身。于是「服务层」在这里几乎只是一层校验——它不试图与 SDK 的类型解耦。这是一个务实的选择：目录条目是纯数据、没有行为、也不参与内部状态，为它建一套并行类型只会多一处需要同步的地方。

## 关键取舍

**源码事实：** 一条坏条目导致整份目录被拒，错误文案说的是「拒绝从中挑选工具」。因为这份目录的用途是挑选。

**源码事实：** 校验只覆盖五个字段的类型存在性，不检查取值域（空名字、空描述都能通过）。结构错会崩逻辑，取值错只会难看。

**源码事实：** 「响应缺 tools 数组」与「某条条目畸形」是两条不同的错误消息，对应两类不同的上游故障。

**源码事实：** 用 TypeScript 的 `asserts` 签名，让校验与类型收窄同一个动作完成，消除「校验了没窄化」这类错误。

**源码事实：** 每次调用都新建客户端，因为它依赖当前活跃后端，而活跃后端可在会话中途切换。

**架构推论：** 这一层的价值不在「取数据」而在**把「上游不可信」变成一个显式的、有定位信息的失败**。前端消费的是另一个进程（甚至另一个版本）产出的数据，而这类边界上最常见的错误是「尽力而为地部分接受」——过滤掉坏条目，把剩下的当全部。这在展示型数据上没问题，在**选择型数据**上会变成静默的错误前提。这里的判断是：目录是选择型数据，所以要么全可信，要么报错。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/tool-catalog-service/tool-catalog-service.api.ts` | `ToolCatalogService` | 唯一入口 `getCatalog`，取响应并校验后返回 |
| `src/api/tool-catalog-service/tool-catalog-service.api.ts` | `isToolCatalogEntry` | 运行期类型守卫，检查五个必填字段的类型 |
| `src/api/tool-catalog-service/tool-catalog-service.api.ts` | `assertValidCatalog` | 整批断言，一条不合格即抛错，含定位文案 |
| `src/api/tool-catalog-service/tool-catalog-service.api.ts` | `ToolCatalogEntry`（重导出） | SDK 类型直接复用，不建并行模型 |

## 参考

- [OpenHands 源码：tool-catalog-service.api.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/tool-catalog-service/tool-catalog-service.api.ts)
- [TypeScript 文档：断言函数与 `asserts` 类型谓词](https://www.typescriptlang.org/docs/handbook/2/narrowing.html#assertion-functions)
- [OpenHands 文档：工具](https://docs.openhands.dev/openhands/usage/tools)

## 与主核 / AI 的关系

这类「边界校验」与主核里「解析外部格式」的逻辑完全一致：读别人产出的数据时，宽容度要按「这份数据能不能被拒」来定。主核解析外部表定义、COPY 文本、pg_dump 输出时遵循同一条——**能恢复的就恢复，但参与决策的字段必须严格**。

对 agent 工程的直接价值有三条。其一，**选择型数据不能部分接受**。目录、能力清单、模型列表这类数据一旦被用来「挑选」，任何一条不可信都让整个挑选失去意义。其二，**结构校验与取值校验要分开**。前者影响控制流，后者只影响展示，把它们混在一起的校验器往往会拒绝本可正确处理的输入。其三，**用类型系统的断言签名让校验与窄化合一**。这是一个成本极低但收益很实的做法：它让「校验存在」成为类型收窄的**唯一途径**。
