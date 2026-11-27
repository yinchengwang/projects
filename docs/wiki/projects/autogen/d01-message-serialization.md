---
project: autogen
feature: message-serialization
title: 消息序列化注册表
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_serialization.py
    symbols: [SerializationRegistry, MessageSerializer, UnknownPayload]
---

# 消息序列化注册表

## 一句话职责

把「消息对象 ↔ 字节 + 类型名 + 内容类型」的双向转换收敛到一张以二元组 `(type_name, data_content_type)` 为键的注册表，让运行时在写日志、发事件、跨进程序列化时不必认识任何具体消息类。

## 机制是什么

运行时内部传递的是 Python 对象，但一旦要进事件流、追踪 span 或跨进程信封，就必须变成字节。AutoGen 的做法是把这件事做成可插拔：每个消息类型登记若干 `MessageSerializer`，序列化器只自报两个身份字段——`type_name`（类型名）与 `data_content_type`（内容类型，JSON 还是 protobuf）——注册表按这两个字段的二元组查找。

查找的两条路径刻意不对称：**反序列化找不到就返回 `UnknownPayload`，序列化找不到则抛 `ValueError`**。接收端必须能「收到但暂时不认识」的载荷，发送端则不允许「发不出去还装作成功」。

内置序列化器与 Python 类型一一对应：pydantic `BaseModel` 走 `PydanticJsonMessageSerializer`，dataclass 走 `DataclassJsonMessageSerializer`，protobuf `Message` 走 `ProtobufMessageSerializer`。选择由 `try_get_known_serializers_for_type` 按 `issubclass` 顺序决定：先判 `BaseModel`，再判 dataclass，最后判 protobuf。

## 怎么实现的

**类型名。** `_type_name` 对 protobuf 用 `DESCRIPTOR.full_name`（即 proto 包路径下的全名，与 Python 类名无关），其余用 `cls.__name__`；传入实例时取 `cls.__class__.__name__`。所以类型名是「Python 类名」级别的标识，不是命名空间安全的稳定 ID。

**dataclass 的构造期拒绝。** `DataclassJsonMessageSerializer` 在 `__init__` 里做两项检查：字段含 `Union` 就抛 `ValueError`（提示改用 pydantic）；字段含嵌套 dataclass 或嵌套 `BaseModel`（含 `List[BaseModel]`、`List[List[BaseModel]]` 这类泛型嵌套）也抛。反序列化只有一行 `json.loads` 后 `cls(**data)`，序列化是 `asdict` 后 `json.dumps`——没有递归转换能力，所以把「用不了的结构」提前到构造期报错，而不是运行期悄悄丢字段。

**pydantic 与 protobuf。** pydantic 序列化器直接 `model_dump_json` / `model_validate_json`，不加任何约束。protobuf 序列化器先用 `any_pb2.Any()` 的 `Pack` 包一层再 `SerializeToString`，反序列化反向 `Unpack`，`Unpack` 失败抛 `ValueError`——因为 `Any` 载荷里带着完整类型 URL，这层包装是必要成本。

**注册与查找。** `SerializationRegistry._serializers` 就是一个平字典，`add_serializer` 对 `Sequence` 递归展开，因此「一个类型多个编码」天然被二元组键支持。`is_registered` 只查键；`type_name(message)` 是给调用方凑出键前半段的辅助。

**注册时机。** 序列化器不是消息类自己登记，而是**智能体注册时顺带登记**：`BaseAgent.register` / `register_instance` 遍历 `_handles_types()`，把每项的序列化器列表交给 `runtime.add_message_serializer`，后者直接转发给注册表。`_handles_types` 的产出有两个来源：`@handles(Type, serializer=None)` 装饰器（`serializer` 为空时调 `try_get_known_serializers_for_type`，返回空列表直接抛 `ValueError`），以及 `RoutedAgent` 扫描所有 `@message_handler` 的 `target_types`——后者对每个目标类型同样要求至少有一个已知序列化器，否则抛 `ValueError`。两处都留着「TODO: 支持不同序列化器 / 去重」的注释。

**运行时侧的容忍。** `SingleThreadedAgentRuntime._try_serialize` 把 `ValueError` 捕获掉，返回常量字符串 `"Message could not be serialized"`。它的用途只有事件日志与追踪属性，所以这里选择了「日志尽力而为」而不是让一条调试信息打断消息投递。

## 关键取舍

**源码事实：** 反序列化返回 `UnknownPayload`（一个持有 `type_name`、`data_content_type`、`payload` 的 dataclass），序列化抛 `ValueError`。同一个注册表两条路径的严格度不同，是为了让「新版本发、老版本收」能成立，而「老版本发一个新类型」立即失败。

**源码事实：** `_try_serialize` 把序列化失败降级为一句占位字符串，说明序列化在运行时里**只是可观测性设施**，不承担正确性。真正需要字节的地方（跨进程信封）走的是注册表本身，失败会直接暴露。

**源码事实：** dataclass 支持被主动限缩到「平坦字段」。Union、嵌套 dataclass、嵌套 BaseModel 全部在构造期拒绝，把能力边界写在类型上，避免 `asdict` 深度不足造成的静默数据丢失。

**架构推论：** 内容类型常量里的 protobuf 取值 `application/x-protobuf` 带一句源码注释说明「这个 content type 应该是什么其实有争议」。这意味着 protobuf 是二等公民：能用，但协议层没有共识标准。

**架构推论：** 键用「类名」而非稳定 ID，等价于把类的重命名当成协议变更。跨版本互通时，类型名就是线上契约的一部分。

**架构推论：** 序列化器随智能体注册而登记，因此「某个类型能序列化」是**运行时状态**而非代码属性。同一进程里先注册谁，就决定了注册表里有哪些条目；两处注册点都留着「TODO: deduplication」，说明同一类型被重复登记是预期会发生的常态（多个智能体处理同一消息类型），而不是异常。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_serialization.py` | `SerializationRegistry` | 以二元组为键的序列化器注册表 |
| `python/packages/autogen-core/src/autogen_core/_serialization.py` | `MessageSerializer` | 序列化器协议：类型名、内容类型、双向转换 |
| `python/packages/autogen-core/src/autogen_core/_serialization.py` | `UnknownPayload` | 未注册类型的占位载荷 |
| `python/packages/autogen-core/src/autogen_core/_serialization.py` | `DataclassJsonMessageSerializer` | dataclass 的 JSON 编解码，构造期拒绝复杂字段 |
| `python/packages/autogen-core/src/autogen_core/_serialization.py` | `PydanticJsonMessageSerializer` | pydantic 模型的 JSON 编解码 |
| `python/packages/autogen-core/src/autogen_core/_serialization.py` | `ProtobufMessageSerializer` | 以 `Any` 包装的 protobuf 编解码 |
| `python/packages/autogen-core/src/autogen_core/_serialization.py` | `try_get_known_serializers_for_type` | 按 Python 类型推导内置序列化器 |
| `python/packages/autogen-core/src/autogen_core/_serialization.py` | `_type_name` | 类型名推导，protobuf 走 descriptor 全名 |

## 参考

- [AutoGen 源码：_serialization.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_serialization.py)
- [AutoGen 文档：消息与通信](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/message-and-communication.html)
- [Pydantic 文档：模型序列化](https://docs.pydantic.dev/latest/concepts/serialization/)

## 与主核 / AI 的关系

主核的 WAL 也有同构问题：每条记录必须能被资源管理器（rmgr）认出来并按已知布局重放，遇到不认识的类型必须安全跳过。差别在于主核把「类型标识」写进磁盘格式、由编译期常量表决定，是**静态注册**；这里的注册表是运行期字典，同一条消息能否序列化取决于本次进程注册了哪些智能体。静态注册换来跨版本可重现，运行期注册换来插件式扩展——两者都不可能同时占全。

对 AI 工程最直接的推论是：LLM 工具调用的 `arguments` 本身就是一段 JSON 字符串，而它外层又要被这条注册表再序列化一次。于是同一条消息里存在两层 JSON 编码，排查线上问题时必须分清「哪一层坏了」：是模型给出的参数串不合法，还是外层信封编解码失败。

另外，未注册载荷不抛错这个设计，等价于给消息总线的**前向兼容留了口子**：新版本发出的新消息类型，在老版本里会以 `UnknownPayload` 抵达订阅者，由订阅者自行决定是丢弃还是记录。这比「直接拒绝」温和，但要求每个消费者都显式处理这个分支。
