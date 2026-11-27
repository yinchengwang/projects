---
project: metagpt
feature: message-exchange
title: 消息对象与收发路径
page_type: feature
schema_version: 1
refs:
  - path: metagpt/schema.py
    symbols: [Message, UserMessage, SystemMessage]
  - path: metagpt/environment/base_env.py
    symbols: [publish_message, get_role]
---

# 消息对象与收发路径

## 一句话职责

`Message` 是角色之间唯一的通信单位，携带内容、结构化载荷与三个路由字段；`Environment.publish_message` 按地址逐角色投递，`get_role` 按名取人。

## 机制是什么

一条消息有七个字段，可以分成三组：

| 组 | 字段 | 语义 |
|---|---|---|
| 载荷 | `content`、`instruct_content`、`metadata` | 自然语言、结构化输出、附加元信息 |
| 路由 | `cause_by`、`sent_from`、`send_to` | 由哪个动作引发、谁发的、发给谁 |
| 身份 | `id`、`role` | 唯一标识、对话角色（system/user/assistant） |

路由三字段全部是**字符串或字符串集合**，而不是对象引用。`cause_by` 与 `sent_from` 存的是「模块名.类名」形式的全名；`send_to` 是一个集合，默认值是通配地址 `<all>`。这个选择让消息可以直接 JSON 序列化、跨进程传递，代价是路由完全依赖字符串匹配，写错即静默丢弃。

`role` 字段被刻意留给 LLM 对话协议使用（只能取那三个值），MetaGPT 自己的身份信息（谁发的）放在 `sent_from` 与 `metadata` 里。这个分离在 MGX 环境里被显式处理过——它会把非协议值改写成 `assistant`，再把发送者与收件人塞进 `content` 前缀。

## 怎么实现的

**路由字段的归一化有三层。** 第一层是字段校验器（`mode="before"`）：`cause_by` 为空时**默认填成 `UserRequirement` 的类全名**（而不是空串），`sent_from` 为空时归一成空串，`send_to` 为空时归一成 `{<all>}`。第二层是 `__setattr__` 覆写：构造之后再用属性赋值写这三个字段，同样会经过 `any_to_str` / `any_to_str_set` 转换。第三层是序列化器：`send_to` 是集合，写盘时转成列表；`instruct_content` 是模型实例，写盘时展开成「类名 + 映射或模块 + 值」的字典。

其中「`cause_by` 默认给 `UserRequirement`」是一条强默认：任何没标来源的消息都会被当作「用户需求引发的」，因此订阅了 `UserRequirement` 的角色会看到它。这既是便利也是陷阱——一条无来源的内部消息会被误认为用户输入。

**`instruct_content` 的双向编码。** 写盘时按来源分两种：若类型来自动态创建的 ActionNode 模型（判据是类型字符串里含 `metagpt.actions.action_node`），就存 `{"class", "mapping", "value"}`，其中 `mapping` 是字段到类型的映射并额外序列化成字符串；否则存 `{"class", "module", "value"}`，靠模块路径导入还原。读盘时反向：有 `mapping` 就按映射动态建模型类，有 `module` 就 `import_class`，两者都没有则抛 `KeyError`。也就是说结构化载荷的可还原性依赖「动态类留下了映射」或「真实类所在模块可导入」二者之一。

**子类的角色绑定。** `UserMessage`、`SystemMessage`、`AIMessage` 三个子类都做同一件事：扔掉调用方传来的 `role`，强制设成自己的值。这是一种「用类型表达角色」的做法——想要 user 角色就用 `UserMessage`，不要靠传参。`AIMessage` 额外提供 `with_agent` 把「谁生成的」记进 `metadata["agent"]`，并在读取时用属性 `agent` 取回。

**id 的生成时机。** `id` 的校验器在为空时用 uuid4 十六进制填充。注意 `load` 静态方法会先把 `id` 从字典里摘出来，构造完消息再单独赋回去——绕开校验器以保留原有 id。此外记忆层有一个 `ignore_id` 开关，打开后所有消息入库时 id 都被改写成同一个固定值，这样「按 id 判等」退化成「按内容判等」，用于去重。

**环境的投递实现。** `publish_message` 的流程是：记录 debug 日志、遍历地址表 `member_addrs`、对每个角色用 `is_send_to(message, addrs)` 判断是否命中、命中则 `role.put_message(message)` 放进该角色的私有缓冲、最后把消息追加进 `history`。

`is_send_to` 的判定只有两条：消息的 `send_to` 里有 `<all>` 就直接命中；否则看角色的任一地址是否出现在 `send_to` 里。**不看 `cause_by`**。所以「消息该给谁」完全由 `send_to` 与收件地址决定，`cause_by` 只在角色侧的 `_observe` 里用于过滤「与我关注的动作相关」。

找不到任何收件人时，实现只记一条 warning，然后照常返回 `True`。返回值没有传达失败信息。

**`get_role` 的宽容。** 它用 `dict.get(name, None)`，取不到返回 `None` 而不抛异常。这与 `get_addresses` 一致：整个环境 API 在「查不到」这件事上都返回空值，让调用方自己判空。对比一下 `EnvAPIRegistry.get`，那里查不到会抛 `KeyError`——同一代码库里存在两种相反约定。

## 关键取舍

**源码事实：** 路由用字符串全名而不是对象引用。这让消息可以自由序列化、跨进程序列化，也让「发送者是谁」不依赖对象还活着。代价是所有路由都是字符串相等比较，任何改名（模块改名、类改名）都会让旧消息或旧存档里的路由失效。

**源码事实：** `cause_by` 缺省为 `UserRequirement`。这让「随手发一条消息」也能被主流程消费，但也让「忘记标来源」静默变成「用户提的需求」。

**源码事实：** `send_to` 默认 `<all>` 且 `is_send_to` 优先判通配，意味着默认为广播。定向投递必须显式指定收件人地址（类全名或角色名二者之一），否则每个角色都会收到。

**源码事实：** 环境投递不检查 `cause_by`，也不看角色是否已经处理过这条消息。去重被推给角色侧：`_observe` 会拿「记忆里已有的消息」做差集。也就是说同一条消息若被放进缓冲两次，角色侧也可能处理两次。

**架构推论：** 消息不带「已消费」标记，也没有确认机制。投递是「塞进缓冲就结束」，角色读不读、读几次，环境都不知道。这使整套通信是无确认的单向推送，与 autogen 那种带请求/响应语义的消息模型不同。

**架构推论：** `Message` 同时承担「对话消息」与「事件」两种角色。当它作为事件时，`content` 是载荷、`cause_by` 是事件类型、`send_to` 决定订阅者；当它作为对话消息时，`role` 与 `content` 直接喂给 LLM。同一个类型两种用途，使「事件流」与「对话历史」共用一份存储——这也是记忆既能当事件日志又能当对话上下文的原因。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/schema.py` | `Message` | 消息定义：载荷、路由、身份三组字段与归一化校验器 |
| `metagpt/schema.py` | `UserMessage` | 强制 `role="user"` 的子类 |
| `metagpt/schema.py` | `SystemMessage` | 强制 `role="system"` 的子类 |
| `metagpt/schema.py` | `Message.load` | 从 JSON 还原，保留原 id 绕开校验器 |
| `metagpt/environment/base_env.py` | `Environment.publish_message` | 遍历地址表逐角色投递，无可达者只告警 |
| `metagpt/environment/base_env.py` | `Environment.get_role` | 按名取角色，取不到返回 `None` |
| `metagpt/utils/common.py` | `is_send_to` | 通配优先、否则地址交集的路由判定 |
| `metagpt/const.py` | `MESSAGE_ROUTE_TO_ALL` | 通配地址常量 `<all>` |

## 参考

- [MetaGPT 源码：schema.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/schema.py)
- [MetaGPT 源码：environment/base_env.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/environment/base_env.py)
- [MetaGPT 源码：utils/common.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/utils/common.py)

## 与主核 / AI 的关系

「路由用纯字符串、不做对象引用」与主核里「日志记录逻辑标识而不是物理地址」是同一取向：可持久化优先，运行期解析次之。代价也一致——改名即断链，需要靠外部约定维持一致性。

对 AI 工程而言，这一页能解释三类常见故障。第一类是「消息没人收」：`send_to` 写的是角色名还是类全名、地址表里是否有这个地址，两者必须一致；不匹配时只有一条 warning，不会报错。第二类是「消息被所有人收到」：忘了设 `send_to` 就默认广播。第三类是「角色把内部消息当用户需求」：`cause_by` 为空会被填成 `UserRequirement`，于是任何订阅用户需求的动作都会被触发。与 LangGraph 的通道写入相比，这里没有版本号也没有合并语义，投递就是入队，消费与否由接收方自己负责。
