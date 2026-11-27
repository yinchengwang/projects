---
project: crewai
feature: security-fingerprint
title: 安全配置与指纹
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/security/security_config.py
    symbols: [SecurityConfig]
  - path: lib/crewai/src/crewai/security/fingerprint.py
    symbols: [Fingerprint]
---

# 安全配置与指纹

## 一句话职责

给实体（agent、task）一个稳定的运行时标识：默认是随机 UUID，也可以由调用方给的种子确定性生成；连同创建时间与一层受限的元数据一起，作为「这是谁」的依据贯穿事件、工具调用与检查点。

## 机制是什么

`Fingerprint` 是 pydantic 模型，但它**只有 `metadata` 是真正字段**：`_uuid_str` 与 `_created_at` 都是 `PrivateAttr`，通过 `uuid_str`、`created_at`、`uuid` 三个只读 property 暴露。这个设计选择决定了它的序列化全靠手写——`to_dict` 和 `from_dict` 显式搬运三个值，而不是靠 pydantic 的 `model_dump`。

标识有两种生成方式：默认 `default_factory=lambda: str(uuid4())`，即每次构造都是新的随机 UUID；`generate(seed=...)` 则用 `uuid5(CREW_AI_NAMESPACE, seed)` 生成**确定性** UUID。命名空间是硬编码常量 `f47ac10b-58cc-4372-a567-0e02b2c3d479`，注释说明用它「用 v5（SHA-1）创建确定性 UUID，作为 CrewAI 的自定义命名空间以增强安全性」。

`SecurityConfig` 是个只有 `fingerprint` 一个字段的容器，但它的校验器把四种输入都收进来：`None` 造新指纹、字符串当种子生成、字典走 `from_dict`、已是指纹实例原样返回。

## 怎么实现的

**种子的确定性。** `_generate_uuid(seed)` 先检查 `seed.strip()` 非空（否则 `ValueError`），再 `str(uuid5(CREW_AI_NAMESPACE, seed))`。所以同一个种子在任何进程、任何时候都得到同一个 UUID，这是「可复现标识」的基础。`generate` 里改私有属性的写法是 `fingerprint.__dict__["_uuid_str"] = ...`——绕过 property 与 pydantic 的赋值路径，直接写实例字典。

**元数据的四重限制。** `_validate_metadata` 是 `BeforeValidator`，检查四件事：必须是字典；键必须是字符串；嵌套只允许一层（嵌套字典里的值不能再是字典）；`len(str(v)) > 10_000` 时报「超过 10KB」。

注意最后一条用的是 `len(str(v))`——那是字典 repr 的字符数，不是序列化字节数。所以限制是粗略的字符量级，中文键值会因 repr 转义而比预期更早触顶。

**等值与哈希只认 UUID。** `__eq__` 要求 `type(other) is Fingerprint`（严格同类型，子类不算）且 `uuid_str` 相等；`__hash__` 是 `hash(self.uuid_str)`。所以两个元数据完全不同但 UUID 相同的指纹被视为相等。这是把「身份」与「附加信息」分开的正确做法，也意味着**元数据不参与去重**。

**`__str__` 直接返回 UUID。** 所以日志里打印指纹得到的就是那串 UUID，不带类名或元数据。

**序列化的手工往返。** `to_dict` 输出 `uuid_str`、`created_at`（ISO 格式字符串）、`metadata` 三个键。`from_dict` 反向：空字典造新指纹；否则先按 metadata 构造，再用 `__dict__` 直写覆盖 `_uuid_str` 与 `_created_at`（后者仅在原值是字符串时用 `datetime.fromisoformat` 解析）。

**配置侧的四种输入。** `SecurityConfig.validate_fingerprint` 是 `mode="before"` 校验器：`None` → `Fingerprint()`；字符串 → 空串报「种子不能为空」否则 `Fingerprint.generate(seed=v)`；字典 → `Fingerprint.from_dict(v)`；实例 → 原样；其他 → `ValueError(f"Invalid fingerprint type: {type(v)}")`。

所以「构造 `SecurityConfig(fingerprint="my-agent")`」得到的是一个由种子确定的指纹，而「构造 `SecurityConfig()`」得到的是一个随机指纹。

**模型配置的取舍。** `model_config` 是 `ConfigDict(arbitrary_types_allowed=True)`，注释明确写了「不能用 `frozen=True`，因为既有测试会修改 fingerprint 属性」。这是一条坦白：可变性不是设计意图，而是兼容负担。

**消费侧。** 工具调用链里，`_build_fingerprint_config` 从 agent 与 task 各自的 `security_config.fingerprint` 取 `to_dict()`，装进 `{"security_context": {...}}` 作为 `config` 参数传给工具的 `invoke`——**不走工具参数**，注释说明理由：避免与严格的工具 schema 冲突。事件侧则把指纹字段并进事件数据（`_prepare_event_data` 与 `_prepare_event` 都有 `if self.fingerprint_context: event_data.update(...)`）。

## 关键取舍

**源码事实：** `_uuid_str` 与 `_created_at` 是私有属性，所以 `Fingerprint()` 的 `model_dump()` 只会有 `metadata`——完整往返必须用 `to_dict`/`from_dict`。任何走 pydantic 序列化的路径都会丢掉 UUID 与创建时间，从而在反序列化后得到一个**新的随机 UUID**。这是最容易踩的坑。

**源码事实：** `__eq__` 用 `type(other) is Fingerprint` 而不是 `isinstance`。所以子类实例与父类实例即使 UUID 相同也不相等。

**源码事实：** 元数据大小限制用 `len(str(v))`，不是 JSON 字节数，也不是序列化后的长度。这条判据对非 ASCII 内容是偏保守还是偏宽松，取决于 Python 的 `repr` 对非 ASCII 的处理方式（默认保留字符而不转义），所以中文的实际容量与英文接近。

**源码事实：** 命名空间常量注释里还留着「TODO: 确定 `CREW_AI_NAMESPACE` 是否应该做成动态或可配置」。一旦改变，所有基于种子的确定性 UUID 都会变——所有历史记录里的指纹与新的对不上。

**架构推论：** 「随机为主、种子可选」这个默认值选择是有含义的：默认情况下指纹标识**这一次运行里的这个对象**，重启后不复存在；显式给种子才得到「跨运行稳定」的身份。前者适合纯追踪（每次运行互不相干），后者适合审计（同一逻辑实体要对上历史记录）。

**架构推论：** 元数据只允许一层嵌套加 10KB 上限，是把指纹定位成「标识 + 少量标签」而不是「完整对象快照」。若允许深嵌套与大容量，指纹会被当成数据传输通道，最终变成不可控的隐式状态。

**架构推论：** `SecurityConfig` 的名字里带 Security，但实现里只有指纹。类 docstring 的字段清单上，认证凭据、作用域规则、模拟/委派令牌三项都标着 *TODO*。所以这个类是**预留的扩展点**：先把指纹落地（它已经被工具链与事件链真实消费），其余等有需求再填。

**架构推论：** 指纹走 `config` 而不是工具参数，是一个值得记住的模式。任何「框架想往工具里传元信息但工具参数有严格 schema」的场合，都会撞上同一个问题：塞进参数会被 schema 校验拒绝，塞进 config 则不影响参数校验但要求工具侧愿意读。CrewAI 选了后者，代价是工具必须自己去看 `config["security_context"]`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/security/fingerprint.py` | `Fingerprint` | 指纹模型：私有 UUID 与创建时间加元数据 |
| `lib/crewai/src/crewai/security/fingerprint.py` | `Fingerprint.generate` | 随机或按种子确定性构造 |
| `lib/crewai/src/crewai/security/fingerprint.py` | `Fingerprint._generate_uuid` | uuid5 加固定命名空间的确定性 UUID |
| `lib/crewai/src/crewai/security/fingerprint.py` | `Fingerprint.uuid_str` | 只读暴露 UUID 字符串 |
| `lib/crewai/src/crewai/security/fingerprint.py` | `Fingerprint.__eq__` | 严格同类型且 UUID 相等 |
| `lib/crewai/src/crewai/security/fingerprint.py` | `Fingerprint.to_dict` | 手工往返：UUID、创建时间、元数据 |
| `lib/crewai/src/crewai/security/fingerprint.py` | `Fingerprint.from_dict` | 直写私有属性恢复 UUID 与创建时间 |
| `lib/crewai/src/crewai/security/fingerprint.py` | `_validate_metadata` | 键为字符串、只一层嵌套、上限 10KB |
| `lib/crewai/src/crewai/security/security_config.py` | `SecurityConfig` | 安全配置容器，当前只有指纹一个字段 |
| `lib/crewai/src/crewai/security/security_config.py` | `SecurityConfig.validate_fingerprint` | 四种输入的归一化 |
| `lib/crewai/src/crewai/security/security_config.py` | `SecurityConfig.to_dict` | 输出指纹字典 |
| `lib/crewai/src/crewai/security/constants.py` | `CREW_AI_NAMESPACE` | uuid5 用的固定命名空间常量 |

## 参考

- [CrewAI 源码：security/fingerprint.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/security/fingerprint.py)
- [CrewAI 源码：security/security_config.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/security/security_config.py)
- [Python 文档：uuid5 与命名空间 UUID](https://docs.python.org/3/library/uuid.html#uuid.uuid5)

## 与主核 / AI 的关系

主核里有同类需求但用别的解法：后台进程的身份靠 PID 加启动时间戳，事务的身份靠 xid，两者都是**分配**出来的、不可自定义。这里的指纹允许用种子确定性生成，等于允许「同一个逻辑实体在任何进程里都算出同一个 id」——这是无中心协调下的身份方案，与主核那种「由系统分配并记录」的思路相反。

与 AutoGen 相比，AutoGen 的 agent 身份是 `AgentId(type, key)` 二元组，`type` 来自注册名、`key` 默认 `"default"`。那是**结构化复合键**，好处是可读、可按前缀匹配（`b03` 那页的 `TypePrefixSubscription` 就靠前缀做直接消息路由），坏处是不能表达「同一个 agent 跨运行是同一个实体」之外的任何东西。CrewAI 的指纹是**不透明 UUID 加元数据**：不可按前缀查，但可以带任意标签、可以做确定性生成、可以等值比较去重。

对 AI 工程最实用的一条是那个「私有属性导致 pydantic 序列化丢身份」的坑。任何把「身份必须是稳定值」的模型做成 pydantic 类时，若把身份放进 `PrivateAttr`，就必须显式实现序列化；否则中间任何一次 `model_dump`/`model_validate` 都会把它换成新值，症状是「同一个对象在落盘再读回后变成了另一个实体」，而这在检查点场景里是致命的。

另一条是元数据的限制。给标识加一个自由的 `metadata` 字典非常有吸引力，但没有上限就会有人塞进整份上下文。一层嵌套 + 10KB 上限是很务实的界线：够放版本号、来源、标签，不够当数据通道。

最后是 `SecurityConfig` 那个形态——把安全能力的容器先定出来、只填实现就位的那一部分、其余留 TODO。这种「骨架先行」在安全领域尤其常见，因为安全字段一旦上线就难以撤回；先只让指纹生效（它有真实消费者），其余的等到有明确语义与消费者再加，比先加一堆空字段更稳。
