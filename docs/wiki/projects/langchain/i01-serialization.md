---
project: langchain
feature: serialization
title: 可序列化基类与反序列化
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/load/serializable.py
    symbols: [Serializable, BaseSerialized, SerializedConstructor, SerializedSecret]
  - path: libs/core/langchain_core/load/load.py
    symbols: [Reviver]
  - path: libs/core/langchain_core/load/mapping.py
    symbols: [SERIALIZABLE_MAPPING]
---

# 可序列化基类与反序列化

## 一句话职责

把「对象」变成一份可读的 JSON 构造配方，并把密钥替换成**占位符**而不是明文；反序列化时则反过来——只用一张允许清单内的类、只从受信任命名空间导入、且密钥默认不从环境读。

## 机制是什么

三个文件构成一条闭环：

**写侧**（`serializable.py`）——`Serializable(BaseModel, ABC)` 有一个关键默认值：`is_lc_serializable()` 返回 `False`。docstring 说得直接：「按设计，**即使一个类继承了 `Serializable`，它默认也不是可序列化的**。这是为了防止那些本不该被序列化的对象被意外序列化」。

`BaseSerialized` 是四种序列化形状的公共底座：`lc: int`（格式版本）、`id: list[str]`（对象标识）、可选的 `name` 与 `graph`。三个子类靠 `type` 字面量区分：

- `SerializedConstructor` —— `type: "constructor"`，带 `kwargs`。
- `SerializedSecret` —— `type: "secret"`，**没有额外字段**，只有 `lc` / `id`。所以它的全部信息就是 `id: [密钥名]`。
- `SerializedNotImplemented` —— `type: "not_implemented"`，带 `repr`。

**多版本映射**（`mapping.py`）——`SERIALIZABLE_MAPPING: dict[tuple[str, ...], tuple[str, ...]]`，注释：「第一个值是它被序列化成什么，第二个值是加载它时的路径」。模块 doc 举例：`AIMessage` 代码在 `langchain_core.messages.ai`，但序列化时用 `["langchain", "schema", "messages", "AIMessage"]`——「因为这段代码原本在 `langchain.schema.messages` 里。这个映射让我们能反序列化用旧版 LangChain 创建的 `AIMessage`」。

**读侧**（`load.py`）——`Reviver` 是那个 `object_hook`。它的构造参数里，`allowed_objects` 是允许清单、`valid_namespaces` 是信任命名空间、`secrets_from_env` 默认 `False`、`init_validator` 默认是阻止 jinja2 模板的那个。

## 怎么实现的

**`to_json` 的第一步就是「这个类准不准序列化」。** 实现开头：

```python
if not self.is_lc_serializable():
    return self.to_json_not_implemented()
```

所以不返回错误，而是返回一个 `type: "not_implemented"` 的记录。这个选择让序列化**总能产出某种结果**——一个不可序列化的对象出现在待序列化的结构里时，不会让整次序列化失败，而是变成一条「我在这里但没法序列化」的标记，同时带上 `repr` 供人辨认。

`to_json_not_implemented` 的 `id` 构造也做了防御：优先用 `obj.__name__`（说明传进来的是个类），否则用 `obj.__class__.__name__`，两者都包在 `try/except Exception` 里，失败则 `id_` 保持空列表并记 debug 日志。而 `repr` 更是 `with contextlib.suppress(Exception)` ——**连 `repr()` 可能抛异常这件事都防了**。这是很务实的一步：某些对象的 `__repr__` 会访问失效的底层资源。

**`lc_kwargs` 的收集有三道过滤，且第三道来自 Pydantic。** 循环：

```python
for k, v in self:
    if not _is_field_useful(self, k, v): continue
    if k in model_fields and model_fields[k].exclude: continue
    lc_kwargs[k] = getattr(self, k, v)
```

第一道 `_is_field_useful` 决定「这个字段值要不要进序列化」。它的逻辑（从文件末尾的函数可见）：先判真值，再判是否等于默认值，组合是「值真 **或** 值不等于默认」——注释说「若值为假且不等于默认，返回 `value_is_truthy or value_neq_default`」。所以**显式设置的假值（如 `False`、`0`）只要不等于默认就会被保留**，而「与默认相同的值」被省掉。这是「最小化输出」与「不丢显式意图」的平衡。

第二道是 Pydantic 字段的 `exclude` 标记——被标 `exclude=True` 的字段不进序列化（`BaseChatModel` 的 `rate_limiter` 与 `profile` 都是这样标的）。

第三道 `getattr(self, k, v)` 而不是直接用 `v`：因为迭代出来的可能是一个缓存的旧值，取一次最新值更可靠。

**`lc_secrets` 与 `lc_attributes` 沿 MRO 合并，且 secret 别名也被登记。** 那段循环：

```python
for cls in [None, *self.__class__.mro()]:
    if cls is Serializable: break
    if cls:
        for attr in ["lc_namespace", "lc_serializable"]:
            if hasattr(cls, attr):
                raise ValueError(f"Class ... has a deprecated attribute {attr}. Please use the corresponding classmethod instead.")
    this = cast("Serializable", self if cls is None else super(cls, self))
    secrets.update(this.lc_secrets)
    for key in list(secrets):
        value = secrets[key]
        if (key in model_fields) and (alias := model_fields[key].alias) is not None:
            secrets[alias] = value
    lc_kwargs.update(this.lc_attributes)
```

四个要点：

1. **从 `None` 开始**（代表「这个实例自身」），然后遍历 MRO 直到 `Serializable` 为止。所以每个参与继承链的类都能贡献自己的 `lc_secrets` / `lc_attributes`，且**后遍历的父类会覆盖先遍历的子类**（`update` 的方向）。方向值得注意：子类的声明写在前面，父类的会盖掉它——所以父类若声明了同一个 secret 名，父类的映射赢。

2. **`super(cls, self)` 拿的是「绑定到该类的 self」**。这是让 `this.lc_secrets` 走这个类自己的属性解析（如果它有覆盖的话），而不是一律用子类的实现。

3. **废弃属性检查**：类上若还存在 `lc_namespace` 或 `lc_serializable`（老的属性式声明），直接 `ValueError`。所以这是一次**主动的迁移强制**——不兼容地拒绝旧声明方式，逼它改成 `get_lc_namespace()` / `is_lc_serializable()` 这类 classmethod。

4. **secret 别名扩写**：若 secret 的键恰好是一个 Pydantic 字段，且该字段有 `alias`，那把别名也加进 `secrets`。注释解释了目的：「这确保已知的 secret 别名被隐藏。注意：这**不会**隐藏任何不在字段里的其他额外 kwargs」。所以这条防的是「用户用别名字段名传了密钥」——`Field(alias="api-key")` 之类。而「不隐藏额外 kwargs」的坦白也很重要：这套机制只覆盖声明过的（字段名与别名），不覆盖任意 kwarg。

**密钥注入的第三步是「即使没在 kwargs 里也要带上」。**

```python
for key in secrets:
    secret_value = getattr(self, key, None) or lc_kwargs.get(key)
    if secret_value is not None:
        lc_kwargs.update({key: secret_value})
```

注释：「包含所有 secret，即使没在 kwargs 里指定，因为这些 secret 可能通过环境变量传入」。

所以逻辑是：先看对象属性，再看已收集的 kwargs，都没有就不放。而「都没有」恰恰是「从环境变量读」的情形——此时序列化产物里不含这个键，反序列化时由 `secrets_map` 或环境变量补上。这正是这套设计的闭环：**序列化只记「这里需要一个叫 X 的密钥」，从不记密钥值**（除了替换成占位符）。

**`_replace_secrets` 是「把值换成占位符」的那一步，且支持点号路径。** 实现：

```python
result = root.copy()
for path, secret_id in secrets_map.items():
    [*parts, last] = path.split(".")
    current = result
    for part in parts:
        if part not in current: break
        current[part] = current[part].copy()
        current = current[part]
    if last in current:
        current[last] = {"lc": 1, "type": "secret", "id": [secret_id]}
```

三个要点：

1. **`[secret_id]` 是列表**——占位符的 `id` 是一个只含密钥名的一元素列表，恰好符合 `SerializedSecret` 的 `id: list[str]`。
2. **支持嵌套路径**（`path.split(".")`），且每层都 `.copy()` 才改——所以是**写时复制**，不修改调用方传进来的嵌套字典。
3. **`if last in current` 才替换**：键不存在时不插入。所以占位符只替换「原本有值的密钥」，不会凭空造一个新键。

而序列化侧那三行：

```python
return {"lc": 1, "type": "constructor", "id": self.lc_id(), "kwargs": lc_kwargs if not secrets else _replace_secrets(lc_kwargs, secrets)}
```

`if not secrets` 的分支是为了省掉一次深拷贝——没有密钥时直接给 `lc_kwargs`（它本来就是新建的）。

**`Reviver.__call__` 是三级分派加五道校验。** 分派按 `lc == 1` 且 `type` 的字面量：

**第一级 `secret`**：

```python
[key] = value["id"]
if key in self.secrets_map: return self.secrets_map[key]
if self.secrets_from_env and key in os.environ and os.environ[key]: return os.environ[key]
return None
```

三支：显式映射里有就用；允许读环境且环境里有就用；否则 `None`。`[key] = value["id"]` 这种解包写法本身就要求 `id` 恰好一个元素——多元素会抛 `ValueError`，这是一种隐式校验。

注意「环境里存在但值为空」被排除（`os.environ[key]` 的真值判定）。空字符串是无效凭据，拿它当值等于把「配了但没填」变成「配了空值」，很可能导致更难查的认证失败。

**第二级 `not_implemented`**：`if self.ignore_unserializable_fields: return None`，否则 `NotImplementedError` 且消息里带完整 `value`。所以「遇到不可序列化字段」是一个可配置的行为：挑选忽略，或明确失败。

**第三级 `constructor`** 是主体，五道校验：

1. **允许清单**：`if self.allowed_class_paths is not None and mapping_key not in self.allowed_class_paths: ValueError(...)`。错误消息给了三条补救路径（用 `'all'`、用显式列表、查 mapping 文件），且说明了默认的 `'core'` 只允许 core 类。

2. **命名空间首段**：`if namespace[0] not in self.valid_namespaces or namespace == ["langchain"]: ValueError(f"Invalid namespace: {value}")`。注释解释了第二个条件：「根命名空间 `["langchain"]` 不是有效的标识符」。

3. **导入路径解析**：优先查 `self.import_mappings`（映射里有就用映射给出的路径）；否则若 `namespace[0] in DISALLOW_LOAD_FROM_PATH` 则报错；否则 `import_dir = namespace`（把命名空间当路径）。

`DISALLOW_LOAD_FROM_PATH = ["langchain_community", "langchain"]` ——注释：「只允许通过 `SERIALIZABLE_MAPPING` 反序列化的命名空间。不允许按路径加载」。

这一条是整个设计里最关键的安全阀：`langchain` 与 `langchain_community` 这两个命名空间**权限极大**（尤其是 community 包含海量第三方实现），若允许「按序列化数据里的 id 直接 import 任意类」，那任何能控制 payload 的人都能实例化这些命名空间下的任意类。所以对这两个，**只允许走白名单映射**——映射表是编译期固定的。

4. **导入目录也要验命名空间**：`if import_dir[0] not in self.valid_namespaces: ValueError(...)`。注意这一道在「映射解析之后」——所以即使映射给出一个路径，那路径的首段仍要在信任列表里。这是一次**纵深防御**：映射表理论上可能被扩展（`additional_import_mappings`），扩展进来的路径也要过信任检查。

5. **`init_validator(mapping_key, kwargs)` 在 import 之前调用**。注释写明理由：「在导入之前运行 init_validator（例如 jinja2 阻止）以在安全违规上快速失败」。所以这是一个**先验检查**：还没 import 就已经能判断这份 kwargs 有问题。默认的 `default_init_validator` 拦的是 jinja2 模板——因为模板字符串可以在后续渲染时执行任意代码。

之后才是 `importlib.import_module`、`getattr(mod, name)`，以及最后一道：`if not issubclass(cls, Serializable): ValueError(...)`。

所以「import 成功」不等于「可以用」——还得是 `Serializable` 的子类。这道检查杜绝了「借一个合法命名空间去实例化任意类」的路径。

返回值是 `cls(**kwargs)` ——**直接调构造函数**。这是模块 doc 反复警告的核心：反序列化等于执行 `__init__` 与验证器，所以「允许清单」只能保证「不实例化清单外的类」，**不能保证清单内的类没有副作用**。

**模块 doc 对这条边界的坦白值得完整引用。** 它写了两段：

第一段：「反序列化会实例化 Python 对象，所以允许类上的任何构造函数（`__init__`）或验证器都可能在 `load()` 期间运行。一份被精心构造、被允许到达某个非预期类的载荷——或到达预期类但带攻击者控制的 kwargs——可能在对象构建时引发网络调用、文件操作或环境变量访问」。

第二段更具体：「例如，一份不可信清单可以反序列化一个聊天模型，其 `base_url`（或 `endpoint_url`）指向攻击者控制的主机。那个模型发出的任何请求于是都被导向那里——这是一个**服务端请求伪造（SSRF）**向量。这是**预期行为**：反序列化忠实重建清单携带的配置，包括自定义端点，而 LangChain 不特判也不剥除这类 kwargs」。

而它的补救被写成小标题式的 `!!! warning "Do not use with untrusted input"`：「若来源不可信，避免调用 `load()` / `loads()`。若必须，把 `allowed_objects` 限制到在初始化期间不执行逻辑的类型——`allowed_objects='messages'`（或消息类的显式列表）是安全选择。保持 `secrets_from_env=False`」。

所以这套机制的自我定位非常清楚：它是**可信输入的便利工具**，不是不可信输入的沙箱。允许清单管的是「哪些类」，不管「这些类的参数是否危险」。

**`_MESSAGES_ALLOWED_CLASS_NAMES` 是那个「安全选项」的具体清单。** 注释：「`allowed_objects='messages'` 允许的现代消息类。**遗留类型（`BaseMessage` / `BaseMessageChunk`、`ChatMessage` / `ChatMessageChunk`、`FunctionMessage` / `FunctionMessageChunk`）被有意排除**——`BaseMessage` 是抽象的，而 chat/function 变体已被 `ToolMessage` 与工具调用取代」。

所以「安全选项」之所以安全，一半靠「消息类构造时不做 I/O」这个事实，另一半靠**只收现代类**——排除掉那些可能带特殊行为的遗留实现。

**`_MAX_DEPTH = 200`**，docstring「`load()` 会走的字典与列表的最大嵌套深度」。这是防递归耗尽：一个深度 10000 层的嵌套 JSON 会让递归解析爆栈。

## 关键取舍

**源码事实：** `Serializable` 默认不可序列化（`is_lc_serializable()` 返回 `False`），注释明确这是「防止意外序列化」。

**源码事实：** 不可序列化的对象产出 `type: "not_implemented"` 记录而不是报错，且 `repr()` 的调用都被异常保护。

**源码事实：** `lc_secrets` / `lc_attributes` 沿 MRO 合并，父类覆盖子类；`lc_namespace` / `lc_serializable` 这两个旧属性式声明会触发 `ValueError`。

**源码事实：** secret 的字段别名也被登记进 `secrets`，但「不隐藏不在字段里的额外 kwargs」。

**源码事实：** 密钥值被替换成 `{"lc": 1, "type": "secret", "id": [密钥名]}` 的占位符，支持点号路径且写时复制。

**源码事实：** `Reviver` 处理 `secret` 时优先 `secrets_map`、其次（若允许）环境变量、否则 `None`；空环境变量被排除。

**源码事实：** `DISALLOW_LOAD_FROM_PATH` 包含 `langchain` 与 `langchain_community`，这两个命名空间只能经映射加载，不能按路径 import。

**源码事实：** 映射解析之后还要再验一次 `import_dir[0]` 在信任命名空间内。

**源码事实：** `init_validator` 在 `importlib.import_module` **之前**运行。

**源码事实：** 最后一道检查是 `issubclass(cls, Serializable)`，import 成功不等于可用。

**架构推论：** 「密钥替换成占位符」是这一层的核心设计，而它的价值不在于「隐藏」而在于**双向可恢复**。若序列化时直接省略密钥，反序列化时就不知道这里需要密钥；若写成明文，产物就不可共享。占位符同时满足两者：它声明了「这里需要名为 X 的密钥」，且不含值。这是一个「形状完整、内容缺失」的中间态——任何「配置要跨环境传递」的系统都会需要它。

**架构推论：** 「默认不可序列化」这个反向默认值，比「默认可序列化 + 逐个禁用」安全得多。理由在注释里：继承一个基类不该自动获得「我能被写进磁盘并能在别处被重建」这个能力。后者意味着「这个类的构造函数是安全的、可重复执行的、不带副作用」——而那是一个需要显式承诺的性质。

**架构推论：** `DISALLOW_LOAD_FROM_PATH` 那一条是这套安全模型里最实在的防线。允许清单管的是「哪个 id 能用」，但若「id → import 路径」的解析可以由 payload 自由决定，那么 id 空间的约束就形同虚设（任何类都能被一个伪造的 id 指到）。对两个权限最大的命名空间强制走编译期固定的映射表，把「payload 能决定 import 什么」这条通道彻底关掉。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/load/serializable.py` | `Serializable` | 可序列化基类，默认不可序列化 |
| `libs/core/langchain_core/load/serializable.py` | `Serializable.is_lc_serializable` | 反向默认值，防意外序列化 |
| `libs/core/langchain_core/load/serializable.py` | `Serializable.get_lc_namespace` | 默认按模块路径切分的命名空间 |
| `libs/core/langchain_core/load/serializable.py` | `Serializable.lc_secrets` | 构造参数名到密钥 id 的映射 |
| `libs/core/langchain_core/load/serializable.py` | `Serializable.lc_attributes` | 额外要进序列化的属性 |
| `libs/core/langchain_core/load/serializable.py` | `Serializable.lc_id` | 序列化标识列表 |
| `libs/core/langchain_core/load/serializable.py` | `Serializable.to_json` | 收集 kwargs、合并 MRO 声明、替换密钥 |
| `libs/core/langchain_core/load/serializable.py` | `BaseSerialized` | 四种序列化形状的公共底座 |
| `libs/core/langchain_core/load/serializable.py` | `SerializedConstructor` | `type: constructor` 加 kwargs |
| `libs/core/langchain_core/load/serializable.py` | `SerializedSecret` | `type: secret`，只带密钥名 |
| `libs/core/langchain_core/load/serializable.py` | `_replace_secrets` | 点号路径替换为占位符，写时复制 |
| `libs/core/langchain_core/load/load.py` | `Reviver` | 反序列化的 object_hook 与全部校验 |
| `libs/core/langchain_core/load/load.py` | `Reviver.__call__` | 三级分派与五道校验 |
| `libs/core/langchain_core/load/load.py` | `DEFAULT_NAMESPACES` | 十四个受信任命名空间 |
| `libs/core/langchain_core/load/load.py` | `DISALLOW_LOAD_FROM_PATH` | 两个命名空间禁按路径加载 |
| `libs/core/langchain_core/load/load.py` | `_MAX_DEPTH` | 嵌套深度上限 200 |
| `libs/core/langchain_core/load/load.py` | `_MESSAGES_ALLOWED_CLASS_NAMES` | 安全选项的具体清单，排除遗留类 |
| `libs/core/langchain_core/load/load.py` | `default_init_validator` | 导入前拦截 jinja2 模板 |
| `libs/core/langchain_core/load/mapping.py` | `SERIALIZABLE_MAPPING` | 序列化 id 到实际加载路径的映射 |

## 参考

- [langchain 源码：load/serializable.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/load/serializable.py)
- [langchain 源码：load/load.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/load/load.py)
- [OWASP：反序列化不可信数据](https://cheatsheetseries.owasp.org/cheatsheets/Deserialization_Cheat_Sheet.html)

## 与主核 / AI 的关系

主核里最接近的是「系统目录与配置的持久化」：表的定义、索引的定义要能从磁盘重建，而重建时必须校验「这个定义能不能被信任」。这一页的 `Serializable` + `Reviver` 就是那个重建通道，连「默认不可序列化、要显式声明」这个方向都一致——主核里也不能随便把一个内部对象写成可持久化的。

差别在于**威胁模型更陡**。主核的配置来自运维（半可信），这里的序列化产物来自「任何能给你一份 manifest 的人」。所以这里多了两层主核没有的机制：命名空间信任列表，以及「两个大命名空间禁按路径加载」。这是「配置即代码」这个形态的必然后果——一旦反序列化会执行构造函数，配置文件就等价于可执行文件。

与 g04（内存存储）的 `load(..., allowed_objects=[Document])` 直接呼应：那处的白名单参数就是这里的 `allowed_objects`，而 `dump`/`load` 走的 `dumpd` / `load` 正是这一页的函数。所以 g04 的「持久化」能力的安全边界完全由这一页定义。

与 g06（内存与加载）的 `load(run.inputs, allowed_objects="messages")` 同理：追踪里保存的 run 输入也要反序列化，所以取了最严的那一档。

与 h01（聊天模型契约）的联系：`BaseChatModel` 的 `rate_limiter` 与 `profile` 字段都标了 `exclude=True`，所以在 `to_json` 的第二道过滤里被跳过——这解释了「为什么有些字段不出现在序列化产物里」。

对 AI 工程最实用的一条是「密钥用占位符而不是省略」。任何「配置要序列化后共享」的场景（导出一个 agent 定义、把 pipeline 存成文件、把 run 记录写进追踪），都需要这个中间态：保留「这里需要什么密钥」的结构信息，去掉值。省略会让重建者不知道缺什么；明文会让产物不能进版本库或日志。

另一条是 `DISALLOW_LOAD_FROM_PATH` 那条规矩。任何「从数据里读一个标识，然后按标识去 import / 实例化」的机制，都必须回答一个问题：**这个标识能不能指向任意位置？** 如果能，那条「允许清单」就只是装饰。把权限最大的几个命名空间钉死为「只走固定映射」，是让清单真正生效的前提。

最后一条是 `init_validator` 放在 import **之前**。安全检查的顺序很关键：越快失败越好，且最好在产生副作用之前。这里在 import 之前就检查了 kwargs，意味着一次恶意的 jinja2 模板根本不会触及模块导入。判据是：**每个安全检查都应该排在被检查动作的最前面**，而不是「做完再验」。
