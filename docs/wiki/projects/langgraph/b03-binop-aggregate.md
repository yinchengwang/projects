---
project: langgraph
feature: binop-aggregate
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/channels/binop.py
    symbols: [BinaryOperatorAggregate, _get_overwrite, _operators_equal]
---

# 二元聚合通道与覆盖判定

## 一句话职责

BinaryOperatorAggregate 用二元算子把当前值与每个新值累积起来，并识别「整值覆盖」这种特殊写入。

## 机制是什么

给状态键加一个 reducer（例如列表相加、整数相加），这个键就会变成聚合通道。它保存的是「累积结果」，每轮把新值折进去。

但聚合通道必须处理一个例外：如果某个任务要**整体替换**这个键的值（不是追加），继续按算子累加就是错的。源码为此引入 `Overwrite` 这种写入形态，并规定一轮里最多只能有一次覆盖。于是聚合通道的 `update` 实际是一条带状态机的循环：先看到覆盖就整值替换，再看到普通值才做算子累积。

## 怎么实现的

**初始值的可用性探测。** 构造时先尝试 `typ()` 造一个空值；对 `typing` / `collections.abc` 里的抽象容器（`Sequence`、`Set`、`Mapping` 及其可变版本）会先替换成具体类型（`list`、`set`、`dict`）再尝试实例化。造不出来就把值置成缺失标记——这样「类型无法零值构造」不会在构造期崩，而是在 `get()` 时才表现为空通道。

**覆盖形态的三种识别。** `_get_overwrite` 认得三种：`Overwrite` 实例；带哨兵键的单元素字典；以及 JSON 序列化后丢掉 dataclass 类型信息的 `{"value": ..., "type": "__overwrite__"}` 形态。第三种注释写明是为跨 JSON 边界准备的——状态更新可能经由 API server 用 `orjson` 编码后再传回，dataclass 类型已经被抹掉，只能靠字段形状识别。这条设计让「覆盖」语义在序列化往返后仍然成立。

**更新循环。** `update` 先处理「通道还是空的」这一情形：直接把第一个值当初始值，剩下的值继续走循环。循环中一旦遇到覆盖，就检查是否已经有覆盖（第二次就抛 `InvalidUpdateError`，因为一轮里同时有多个任务要求整体替换，语义冲突），否则直接替换并把标记置上。标记置上之后的普通值会被**跳过**——注释和实现都在表达同一件事：覆盖发生在后面，后面的普通值不该再被追加。

**相等性与算子比较。** `__eq__` 只比较算子是否等价，不比较当前值。`_operators_equal` 处理了 lambda 带来的麻烦：所有 lambda 的 `__name__` 都是 `<lambda>`，对象身份比较不可靠，因此只要任一侧是 lambda 就判等价。这是构建期「同键通道是否冲突」判定的实际需要。

**检查点兼容。** `from_checkpoint` 额外处理一种历史形态：若检查点里存的是增量快照对象（字段曾经是增量通道），要取出其内部值再恢复。这让「字段从增量通道改成聚合通道」不会因为旧检查点而读错。

## 关键取舍

**源码事实：** 覆盖用三种形态识别，其中一种专门为 JSON 往返准备。这说明「覆盖」是需要跨进程稳定传递的语义，不能只靠 Python 对象身份。

**源码事实：** 一轮里第二次覆盖直接报错。这与其他通道「多写就报错」的精神一致：并发覆盖必然有一个是被丢弃的，静默选择其一会掩盖逻辑错误。

**源码事实：** 覆盖标记置上之后的普通值被跳过，而不是继续累积。这让「整值替换」与「追加」在同一次更新序列里不会互相污染。

**源码事实：** lambda 被视为彼此等价。这是为了不让「用 lambda 写 reducer」在建图期因为身份不等而误报冲突。

**架构推论：** 聚合通道把「空值可用性」推迟到首次 `get()`，使类型构造失败不影响建图。代价是错误从建图期挪到运行期，这是有意的取舍——建图期无法知道用户类型是否能零值构造。

**架构推论：** 算子等价性只做身份/名字比较，意味着「两个不同函数实现同一语义」会被判为不同通道。用命名函数而不是同名 lambda 时，同一 reducer 必须复用同一函数对象，否则同键会被判成异类通道。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/channels/binop.py` | `BinaryOperatorAggregate` | 聚合通道：用二元算子累积更新 |
| `libs/langgraph/langgraph/channels/binop.py` | `BinaryOperatorAggregate.update` | 覆盖与累积混合的更新状态机 |
| `libs/langgraph/langgraph/channels/binop.py` | `_get_overwrite` | 识别三种覆盖写入形态 |
| `libs/langgraph/langgraph/channels/binop.py` | `_operators_equal` | 算子等价判定，lambda 一律等价 |
| `libs/langgraph/langgraph/channels/binop.py` | `BinaryOperatorAggregate.from_checkpoint` | 兼容历史增量快照形态 |
| `libs/langgraph/langgraph/channels/binop.py` | `_strip_extras` | 剥掉 Annotated / Required / NotRequired 包装 |

## 参考

- [LangGraph 源码：channels/binop.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/channels/binop.py)
- [LangGraph 文档：自定义 reducer](https://langchain-ai.github.io/langgraph/concepts/low_level/#reducers)
- [LangGraph 文档：Overwrite 覆盖写入](https://langchain-ai.github.io/langgraph/how-tos/state-reducers/)

## 与主核 / AI 的关系

聚合通道要求算子满足结合律，这与主核里「增量维护的聚集结果」面临同样的约束：只要合并顺序可能变化，算子就不能依赖顺序。区别在于主核能用事务隔离把并发收敛到串行，这里只能靠「拒绝一轮多写」来避免顺序问题。

对 AI 最直接的是消息历史：`Annotated[list, add_messages]` 就是这条通道的典型用法。理解「覆盖写作」很关键——需要清空历史时不能传空列表再期望累加，而要走覆盖语义，否则空列表会被当成一次追加而被忽略。
