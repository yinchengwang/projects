---
project: openai-agents
feature: runner-entry
title: Runner 唯一入口与默认运行器
page_type: feature
schema_version: 1
refs:
  - path: src/agents/run.py
    symbols: [Runner, AgentRunner, set_default_agent_runner]
  - path: src/agents/run.py
    symbols: [get_default_agent_runner]
---

# Runner 唯一入口与默认运行器

## 一句话职责

`Runner` 是所有 agent 执行的唯一公开入口，三个类方法（异步、同步、流式）签名一致，全部转发给一个可替换的默认运行器实例。

## 机制是什么

这个 SDK 没有「智能体自己在跑」的概念：`Agent` 是纯数据定义，它自己不执行任何东西。真正跑起来必须调用 `Runner`，而 `Runner` 本身也只是个静态转发壳——它的类方法不实现任何逻辑，只把参数原样交给模块级单例 `DEFAULT_AGENT_RUNNER`。

三个类方法对应三种执行形态，它们的参数列表逐字相同（13 个参数：起始智能体、输入、上下文、最大轮次、钩子、运行配置、错误处理器、上一响应 id、自动链式开关、会话 id、会话对象等），差别只在返回值类型和是否可迭代：

| 类方法 | 返回 | 用途 |
|---|---|---|
| `run` | `RunResult` | 异步等待直到结束 |
| `run_sync` | `RunResult` | 同步阻塞版，内部驱动事件循环 |
| `run_streamed` | `RunResultStreaming` | 立即返回，事件与结果边跑边出 |

## 怎么实现的

**转发。** `Runner.run` 取出 `DEFAULT_AGENT_RUNNER`，把 13 个参数逐个显式转发给它的 `run` 方法。这里没有用 `*args, **kwargs` 透传，而是把每个参数都写出来——好处是签名不匹配会在调用点立刻报错，坏处是新增参数要改三处（三个类方法 + `AgentRunner` 的抽象方法）。

**敏感数据处理。** 转发外面包了一层 `try/except BaseException`。逻辑是：如果抛出的异常不是「数据已脱敏」类型的，原样重抛；如果是，就把异常对象用 `_detach_data_redacted_error_traceback` 摘掉 traceback 里的帧，然后**把作用域内所有引用变量的值都置为 None**（起始智能体、输入、上下文、钩子、运行配置、会话……逐个赋 None），最后 `_raise_data_redacted_error` 重抛。

这段代码的意图很明确：脱敏异常意味着「异常对象本身携带着不该泄漏的数据」，而默认的 traceback 会把每个栈帧的局部变量都挂上去，于是敏感数据会通过 traceback 泄漏。摘掉 traceback 帧只能挡住一部分，还要把当前帧的局部变量清空，才能确保异常对象被外传时不再有可达路径指向原始数据。

**默认运行器的可替换性。** `set_default_agent_runner` 改的是模块级变量，`get_default_agent_runner` 读它。这使「换一整套执行引擎」不需要改调用方代码。代价是全局可变状态：进程内所有 `Runner.*` 调用共用同一个运行器，测试之间会互相污染，除非显式恢复。

**输入形态的三态。** `input` 参数接受三种类型：字符串（当一条用户消息）、响应输入项列表、或 `RunState`。第三种是关键——它意味着「从上次中断的状态继续」和「从零开始」走的是同一个入口，不是两套 API。运行器内部按类型分派，而不是要求调用方换成 `Runner.resume()` 之类的独立方法。

## 关键取舍

**源码事实：** `Runner` 的三个类方法都是 `@classmethod`，第一个参数是 `cls` 但从不使用——它们不访问 `Runner` 的任何类属性，实际用的是模块级单例。写成类方法只是为了让 `Runner.run(...)` 这种调用形态成立。

**源码事实：** 脱敏异常路径在重抛前把 12 个局部变量逐个赋 `None`。这不是防御性编程的随手一笔，而是有针对性的：异常对象的 traceback 帧持有该帧的 `f_locals`，只要原始对象还被引用着，数据就仍可达。

**源码事实：** 参数逐字转发而非 `**kwargs`。新增一个运行参数至少要同步改 4 处（三个类方法 + 抽象基类），但换来的是签名级别的类型检查。

**源码事实：** `max_turns` 默认取 `DEFAULT_MAX_TURNS`，且文档明确「传 `None` 可禁用轮次上限」。这是一个显式的不设防开关，而不是靠传一个大数字变通。

**架构推论：** 「一个入口 + 可替换运行器」的结构让 SDK 能把执行策略的整体替换变成一次赋值。这与 langgraph 把编译产物交给不同的执行后端、autogen 把运行时抽象成 `AgentRuntime` 协议是同一种设计取向：公开 API 稳定，执行引擎可换。

**架构推论：** 三态输入使「续跑」不再是特殊路径。这直接影响持久化设计：`RunState` 必须是可完整序列化的，而它确实由 `RunState.to_string()/from_string()` 承担（见 a04）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/run.py` | `Runner` | 三个类方法：异步、同步、流式 |
| `src/agents/run.py` | `AgentRunner` | 真正实现执行逻辑的运行器类 |
| `src/agents/run.py` | `set_default_agent_runner` | 替换模块级默认运行器 |
| `src/agents/run.py` | `get_default_agent_runner` | 读取当前默认运行器 |
| `src/agents/run.py` | `Runner.run` | 异步入口，含脱敏异常处理与变量清空 |

## 参考

- [openai-agents-python 源码：run.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run.py)
- [Agents SDK 文档：Running agents](https://openai.github.io/openai-agents-python/running_agents/)
- [Agents SDK 文档：Agent 是纯定义](https://openai.github.io/openai-agents-python/agents/)

## 与主核 / AI 的关系

「入口固定、引擎可换」与主核里「SQL 入口固定、存储引擎可替换」是同一个模式。差别在于主核的替换发生在编译期（换实现），这里的替换发生在运行期（换单例），所以必须处理「换掉之后旧对象还活着」的问题——SDK 的做法是让运行器无状态，状态全在 `RunState` 与结果对象里。

脱敏异常那段代码值得单独对照：它解决的问题是「错误报告本身成为数据泄漏通道」。主核里对应的问题是「错误日志里带出页面内容」或「panic 信息带出密钥」。做法上的共性是：不要指望调用方记得脱敏，要在异常离开边界前把可达路径切断。

对 agent 工程的实际建议：选 SDK 时先看它的执行入口是不是单一且可替换。入口分散（例如每类工具各有自己的执行函数）会导致「统一加超时、统一加追踪、统一加重试」这类需求无处落地。
