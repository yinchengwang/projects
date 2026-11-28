---
project: haystack
feature: super-component
title: 超级组件与映射包装
page_type: feature
schema_version: 1
refs:
  - path: haystack/core/super_component/super_component.py
    symbols: [SuperComponent, super_component, _SuperComponent]
  - path: haystack/core/super_component/utils.py
    symbols: [_types_are_compatible, _is_compatible, _unwrap_variadics]
---

# 超级组件与映射包装

## 一句话职责

`SuperComponent` 把「一整张子图」包装成「一个普通组件」：用 `input_mapping` / `output_mapping` 把子图内部的 `组件名.socket名` 路径翻译成新的组件级 socket 名，并且在**构造期**就用 `_is_compatible` 做双向类型兼容检查——于是「多条子图输入被折叠成一个输入」这件事在类型上是安全的，而不是靠调用方自觉。

## 机制是什么

它要解决的问题是**层次化封装**：一张 RAG 子图有十几个组件、几十个 socket，但对外只应该暴露 `query` 和 `replies` 两个口。朴素做法是写一个包装组件手转输入输出，但那意味着重复一遍接线信息、并且与子图脱节。

`SuperComponent` 的做法是**声明式映射**：

```
input_mapping  = {"query": ["retriever.query", "prompt_builder.query"]}
output_mapping = {"llm.replies": "replies"}
```

两种映射的形状是**不对称**的，这个不对称很有信息量：

| | 键 | 值 | 语义 |
|---|---|---|---|
| `input_mapping` | 新的输入名 | **路径列表** | 一个外部输入要喂给多个内部 socket（**扇出**） |
| `output_mapping` | **路径** | 新的输出名 | 一个内部 socket 变成一个外部输出（**一对一**） |

**输入是「一喂多」，输出是「一对一」。** 为什么不支持「多个内部输出合成一个外部输出」？因为合成需要有合并语义（谁优先？列表还是字典？），而那是业务决定，不该由包装层猜。输入侧的「一喂多」没有这个问题——同一个值分发给多个消费者是明确的。

这个不对称决定了类型检查的形态：**输入侧需要「聚合多个类型」，输出侧只需要「逐个搬运」**。于是 `_resolve_input_types_from_mapping` 要算「共同类型」，而 `_resolve_output_types_from_mapping` 只做同名检查（防止两个路径映射到同一个输出名）。

## 怎么实现的

**`_SuperComponent` 与 `SuperComponent` 分两层，前者不带 `@component`，后者带。**

```
@component
class _SuperComponent:
    def __init__(self, pipeline, input_mapping=None, output_mapping=None): ...

@component
class SuperComponent(_SuperComponent):
    def to_dict(self): ...
    @classmethod
    def from_dict(cls, data): ...
```

**两个类都挂了 `@component`** ——这是必须的：`_SuperComponent` 需要 `component.set_input_type` / `set_output_types` 这两个只能作用于「被 `@component` 处理过」的实例上的方法。而分成两层的原因是**序列化入口只在 `SuperComponent` 上提供**：`from_dict` 必须能构造出正确的类，而 `_SuperComponent` 是内部实现。

`__init__` 的执行顺序值得逐段看：

```
pipeline_inputs = self.pipeline.inputs()
resolved_input_mapping = input_mapping if input_mapping is not None else self._create_input_mapping(pipeline_inputs)
self._validate_input_mapping(pipeline_inputs, resolved_input_mapping)
input_types = self._resolve_input_types_from_mapping(pipeline_inputs, resolved_input_mapping)
for input_name, info in input_types.items():
    component.set_input_type(self, name=input_name, **info)

self.input_mapping = resolved_input_mapping
self._original_input_mapping = input_mapping
```

四步：**取子图输入 → 决定映射（默认或用户给）→ 校验映射 → 解析类型并注册 socket**。最后存两份映射：`input_mapping` 是**解析后**的（一定有值），`_original_input_mapping` 是**用户原始给的**（可能是 `None`）。

这个双份存储是序列化正确性的关键：`to_dict` 用原始值，于是「用户没给映射」这个事实被保留下来——重载时会重新走一遍默认推导，而不是把推导结果固化进配置。

**输入映射校验与类型解析是两次独立遍历，职责不同。**

校验只关心「路径指向的东西存在不存在」：

```
for wrapper_input_name, pipeline_input_paths in input_mapping.items():
    if not isinstance(pipeline_input_paths, list):
        raise InvalidMappingTypeError(f"Input paths for '{wrapper_input_name}' must be a list of strings.")
    for path in pipeline_input_paths:
        comp_name, socket_name = self._split_component_path(path)
        if comp_name not in pipeline_inputs:
            raise InvalidMappingValueError(
                f"Component '{comp_name}' not found in pipeline inputs.\n"
                f"Available components: {list(pipeline_inputs.keys())}")
        if socket_name not in pipeline_inputs[comp_name]:
            raise InvalidMappingValueError(
                f"Input socket '{socket_name}' not found in component '{comp_name}'.\n"
                f"Available inputs for '{comp_name}': {list(pipeline_inputs[comp_name].keys())}")
```

**两条错误消息都把「可选项」列了出来**（`Available components` / `Available inputs`）。在一个「路径字符串」被大量使用的 API 里，这是最有效的错误消息形态——用户拼错一个字母时，看到的不是「找不到」而是「这些才是能用的」。

**类型解析把「扇出」变成「求共同类型」，规则有三条。**

```
existing_socket_info = aggregated_inputs.get(wrapper_input_name)
if existing_socket_info is None:
    aggregated_inputs[wrapper_input_name] = {"type": socket_info["type"]}
    if not socket_info["is_mandatory"]:
        aggregated_inputs[wrapper_input_name]["default"] = _delegate_default
    continue

is_compatible, common_type = _is_compatible(existing_socket_info["type"], socket_info["type"])
if not is_compatible:
    raise InvalidMappingTypeError(
        f"Type conflict for input '{socket_name}' from component '{comp_name}'. "
        f"Existing type: {existing_socket_info['type']}, new type: {socket_info['type']}.")

aggregated_inputs[wrapper_input_name]["type"] = common_type

if socket_info["is_mandatory"]:
    aggregated_inputs[wrapper_input_name].pop("default", None)
```

三条规则：

1. **第一个路径决定初始类型**；若它非必填，就挂上 `_delegate_default` 作为默认值。
2. **后续路径逐一与当前聚合类型做兼容检查**，兼容则把聚合类型**收紧成共同类型**（`common_type`）。不兼容立即报错，消息里带两侧类型。
3. **必填性取「或」**：任何一个被映射的 socket 是必填的，聚合输入就是必填的（把 `default` 弹出）。注释写明理由。

第 3 条的方向值得想一下：为什么是「或」而不是「与」？因为一个外部输入要同时喂给多个内部 socket，**任何一个内部 socket 缺不起，这个外部输入就不能省**。若取「与」（只有全都必填才算必填），那么「A 必填 + B 可选」的映射会被判成可选——用户省略它时 A 就收不到值了。

**`_delegate_default` 是一个空类，用途是「把默认值填充交给内层组件」。**

```
class _delegate_default:
    """Custom object for delegating filling of default values to the underlying components."""
```

**为什么需要这个哨兵？** 因为超级组件的输入 socket 上挂的默认值不能是内层某个组件的具体默认值——多个内层组件的默认值可能不同，用哪一个都不对。于是挂一个哨兵，运行时把它过滤掉：

```
# `is not`, not `!=`: numpy/pandas/torch override `__ne__` element-wise and would crash here.
filtered_inputs = {param: value for param, value in kwargs.items() if value is not _delegate_default}
```

**这一行注释是全篇最有价值的一处**：用 `is not` 而不是 `!=`，理由是「numpy/pandas/torch 重写了 `__ne__` 做逐元素比较，会在这里崩」。而且注释明确指出会「crash here」——这是实际踩过的坑。

于是语义变成：**超级组件层面声明「这个输入可选」，但真实默认值由内层组件自己在 `run` 里填**。这与 a02 里 `_add_missing_input_defaults` 是同一件事的两侧——那边负责填入，这边负责不填。

**`_create_input_mapping` 是默认映射，它按 socket 名归并。**

```
input_mapping = {}
for comp_name, inputs_dict in pipeline_inputs.items():
    for socket_name in inputs_dict.keys():
        existing_socket_info = input_mapping.get(socket_name)
        if existing_socket_info is None:
            input_mapping[socket_name] = [f"{comp_name}.{socket_name}"]
            continue
        input_mapping[socket_name].append(f"{comp_name}.{socket_name}")
```

**键是 socket 名，不是组件名。** 于是「两个组件都有叫 `query` 的输入」会自动合并成超级组件的一个 `query` 输入扇出给他们。这是一个很巧的默认策略：**同名即同义**。

副作用是：若两个组件的 `query` 语义不同（一个是要检索的文本、一个是 SQL 查询串），默认映射会把它们错误合并。所以默认映射只适合「同名同义」的管道，而一旦有歧义就必须显式给 `input_mapping`。

**输出映射的默认策略是「同名冲突就报错」。**

```
used_output_names: set[str] = set()
for comp_name, outputs_dict in pipeline_outputs.items():
    for socket_name in outputs_dict.keys():
        if socket_name in used_output_names:
            raise InvalidMappingValueError(
                f"Output name conflict: '{socket_name}' is produced by multiple components. "
                "Please provide an output_mapping to resolve this conflict.")
        used_output_names.add(socket_name)
        output_mapping[f"{comp_name}.{socket_name}"] = socket_name
```

**注意它比输入侧更严格**：输入侧同名时合并（扇出），输出侧同名时报错。

这个不对称完全合理：输入合并是「一个值给多人用」（安全），输出合并是「两个值抢一个名字」（必须有人决定留哪个）。而且注意报错消息直接给出解法——「请提供 `output_mapping` 来解决冲突」。

**输出的数据来源是 `include_components_with_connected_outputs=True`，这是一个必要参数。**

```
leaf_pipeline_outputs = self.pipeline.outputs()
all_possible_pipeline_outputs = self.pipeline.outputs(include_components_with_connected_outputs=True)

resolved_output_mapping = output_mapping if output_mapping is not None else self._create_output_mapping(leaf_pipeline_outputs)
self._validate_output_mapping(all_possible_pipeline_outputs, resolved_output_mapping)
```

三处不同的取法：

- **默认映射**基于 `leaf_pipeline_outputs`（只有叶子组件的输出）——因为默认情况下超级组件对外暴露的就是子图的最终产物。
- **校验**基于 `all_possible_pipeline_outputs`（含中间组件的输出）——因为用户可能显式映射一个中间输出（例如「顺便把检索到的文档也暴露出来」）。
- **类型解析**同样基于 `all_possible_pipeline_outputs`。

如果校验也用叶子输出，那么「映射一个中间输出」这个合法用法会被误判为「socket 不存在」。**这个用两种口径的细节是这一页最容易漏的设计点。**

运行时配合的是 `_get_include_outputs_from`：

```
return {self._split_component_path(path)[0] for path in self.output_mapping.keys()}
```

**把所有被映射组件名塞进 `include_outputs_from`** ——于是管道运行时会把那些组件的输出一并返回（哪怕它们是中间组件）。这与 a02 的 `run(include_outputs_from=...)` 参数直接配套：**超级组件的「暴露中间输出」能力，实际是靠管道那个参数实现的**。

**`run` 的三步是「放进去 → 跑 → 取出来」。**

```
def run(self, **kwargs):
    filtered_inputs = {p: v for p, v in kwargs.items() if v is not _delegate_default}
    pipeline_inputs = self._map_explicit_inputs(input_mapping=self.input_mapping, inputs=filtered_inputs)
    include_outputs_from = self._get_include_outputs_from()
    pipeline_outputs = self.pipeline.run(data=pipeline_inputs, include_outputs_from=include_outputs_from)
    return self._map_explicit_outputs(pipeline_outputs, self.output_mapping)
```

`_map_explicit_inputs` 是扇出：

```
for wrapper_input_name, pipeline_input_paths in input_mapping.items():
    if wrapper_input_name not in inputs:
        continue
    for socket_path in pipeline_input_paths:
        comp_name, input_name = self._split_component_path(socket_path)
        if comp_name not in pipeline_inputs:
            pipeline_inputs[comp_name] = {}
        pipeline_inputs[comp_name][input_name] = inputs[wrapper_input_name]
```

**`if wrapper_input_name not in inputs: continue`** 实现「可选输入」：用户没给就整个跳过，内层组件自己填默认值。这里没有「用聚合默认值填入」的逻辑——那正是 `_delegate_default` 的设计意图。

`_map_explicit_outputs` 是取出：

```
for pipeline_output_path, wrapper_output_name in output_mapping.items():
    comp_name, socket_name = self._split_component_path(pipeline_output_path)
    if comp_name in pipeline_outputs and socket_name in pipeline_outputs[comp_name]:
        outputs[wrapper_output_name] = pipeline_outputs[comp_name][socket_name]
```

**两个 `in` 检查让「映射的组件这次没产出」变成「静默跳过」而不是 `KeyError`。** 这是一个有意宽容的选择：管道里条件分支可能让某个组件不执行，此时它对应的输出就该缺席。代价是**用户无法区分「没跑」与「跑了但没这个键」**。

**`SuperComponent.from_dict` 处理了一个遗留键。**

```
# `is_pipeline_async` is a legacy key kept only for backward compatibility.
data["init_parameters"].pop("is_pipeline_async", None)
pipeline = Pipeline.from_dict(data["init_parameters"]["pipeline"])
data["init_parameters"]["pipeline"] = pipeline
return default_from_dict(cls, data)
```

两点：**先 `pop` 掉历史键再反序列化**（旧版本存过 `is_pipeline_async`，现在这个字段没了）；**嵌套的 pipeline 要单独 `Pipeline.from_dict`**——`default_from_dict` 只做一层反序列化，不会递归处理未知结构。

注意它**就地修改了传入的 `data`**（`data["init_parameters"]["pipeline"] = pipeline`）。这意味着同一个 `data` dict 被调用两次 `from_dict` 会出问题（第二次拿到的是已构造的 `Pipeline` 而不是 dict）。这是「为省一次 copy 而引入的隐式约束」。

**`@super_component` 装饰器把类「转成」`_SuperComponent` 的子类，并把原 `__init__` 包起来。**

```
def init_wrapper(self, *args, **kwargs):
    original_init(self, *args, **kwargs)
    if not hasattr(self, "pipeline"):
        raise ValueError(f"Class {cls.__name__} decorated with @super_component must define a 'pipeline' attribute")
    _SuperComponent.__init__(
        self, pipeline=self.pipeline,
        input_mapping=getattr(self, "input_mapping", None),
        output_mapping=getattr(self, "output_mapping", None))

init_wrapper = functools.wraps(original_init)(init_wrapper)
```

三个设计点：

1. **先跑用户的 `__init__`，再跑 `_SuperComponent.__init__`** ——顺序反了不行：`_SuperComponent.__init__` 需要 `self.pipeline`，而它由用户的 `__init__` 创建。
2. **`functools.wraps(original_init)` 保留原签名**，注释写明理由是「为了 IDE / 文档 / 工具」。
3. **`getattr(self, "input_mapping", None)`** ——映射是**可选属性**，用户可以不定义。

然后重建类：

```
new_cls = new_class(cls.__name__, (_SuperComponent,) + cls.__bases__, {}, copy_class_namespace)
new_cls.__module__ = cls.__module__
new_cls.__qualname__ = cls.__qualname__
new_cls.__doc__ = cls.__doc__
return component(new_cls)
```

**`(_SuperComponent,) + cls.__bases__`** ——新类同时继承 `_SuperComponent` 与用户声明的全部基类。注释解释了这个选择。三个 `__module__` / `__qualname__` / `__doc__` 的显式赋值是为了让**序列化路径与文档呈现保持与用户类一致**——`new_class` 造出来的类默认会把 `__module__` 设成 `super_component` 模块，那样 registry 里登记的就是错误的路径。

**最后 `return component(new_cls)`，而不是给 `new_cls` 加装饰器** ——因为 `@super_component` 本身就是装饰器，不可能再叠一个 `@component`。注意顺序：**先 `super_component` 再 `component`**，于是 `component(new_cls)` 会再重建一次类并套上 `ComponentMeta`。

**这里是全篇最绕的一处**：`_SuperComponent` 已经带了 `@component`，`new_cls` 继承它，又被 `component(new_cls)` 再处理一次。`ComponentMeta.__call__` 在实例化时会跑 `_parse_and_set_input_sockets(cls, instance)`——它会从 `run` 签名反推 socket。而 `_SuperComponent.run(self, **kwargs)` 的签名里**只有 `**kwargs`**，于是反推出的输入 socket 集合为空（a01 讲过的「跳过 `VAR_KEYWORD`」）；而 `set_input_type` 已经在 `__init__` 里手工设好真正的 socket，且 `_parse_and_set_input_sockets` 只在 `__init__` **之前**没设过 `__haystack_input__` 时才创建空容器……

实际上这里存在一个时序细节值得点出：`ComponentMeta.__call__` 的顺序是「先 `super().__call__()` 完成 `__init__`（期间 `set_input_type` 往 `__haystack_input__` 写 socket），再 `_parse_and_set_input_sockets`」。而后者对每个 `run` 签名参数做「若同名 socket 存在且不等则报错」的检查——`run` 只有 `**kwargs`，一个参数都不产生，所以不会冲突，也不会清空已有 socket。**整个机制能成立，依赖的是「`run(self, **kwargs)` 不产生任何具名参数」这个巧合。**

## 关键取舍

**源码事实：** `input_mapping` 的值是**路径列表**（一个外部输入扇出到多个内部 socket），`output_mapping` 的键是**路径**、值是单个名字（一对一）；不支持多个内部输出合成一个外部输出。

**源码事实：** `__init__` 存两份映射：`input_mapping` / `output_mapping` 是解析后的（一定有值），`_original_input_mapping` / `_original_output_mapping` 是用户原始值（可为 `None`），序列化时用后者以便重载时重新推导默认映射。

**源码事实：** 输入类型解析有三条规则：首个路径定初始类型、后续路径用 `_is_compatible` 检查并把聚合类型收紧为共同类型、必填性取「或」（任何一个被映射 socket 必填则聚合输入必填，并弹掉 `default`）。

**源码事实：** 可选输入挂 `_delegate_default` 哨兵作默认值，运行前用 `value is not _delegate_default` 过滤掉（注释明确说不能用 `!=`，因为 numpy/pandas/torch 的逐元素 `__ne__` 会崩）；真实默认值由内层组件自己填。

**源码事实：** 默认输入映射按 **socket 名** 归并（同名即合并成扇出）；默认输出映射按 socket 名建，同名冲突时直接报错并提示用户显式提供 `output_mapping`。

**源码事实：** 输出用三种口径：默认映射基于**叶子**输出，校验与类型解析基于**含连接输出**（`include_components_with_connected_outputs=True`）。否则「映射中间组件的输出」会被误判为 socket 不存在。运行时靠 `_get_include_outputs_from` 把所有被映射的组件名塞进 `include_outputs_from`。

**源码事实：** `_map_explicit_outputs` 对「组件不在输出里」与「socket 不在组件输出里」都用 `in` 检查后静默跳过，不抛 `KeyError`；代价是无法区分「组件没跑」与「跑了但没这个键」。

**源码事实：** 映射校验的两条错误消息都列出可选项（`Available components` / `Available inputs`）。

**源码事实：** `SuperComponent.from_dict` 会 `pop` 掉遗留键 `is_pipeline_async`，单独调 `Pipeline.from_dict` 处理嵌套管道，并**就地修改**传入的 `data`。

**源码事实：** `@super_component` 先跑用户 `__init__` 再跑 `_SuperComponent.__init__`（后者依赖前者建立的 `self.pipeline`），用 `functools.wraps` 保留原签名，用 `new_class(..., (_SuperComponent,) + cls.__bases__, ...)` 让新类同时继承 `_SuperComponent` 与用户基类，显式复制 `__module__` / `__qualname__` / `__doc__`，最后 `return component(new_cls)`。

**架构推论：** 「输入扇出允许、输出合并不允许」这个不对称不是遗漏，而是**合并语义必须由业务决定**的体现。多个内部输出抢一个名字时，谁赢、要不要合成列表——这些都没有普适答案。相比之下「把同一个值给多个消费者」是明确无歧义的。

**架构推论：** 默认映射「按 socket 名归并」把「同名即同义」当成约定。这让最简单的管道零配置可用，但对于「两个组件都有 `query` 但语义不同」的管道必须显式配置。这类「友好的默认 + 静默的错误可能」是所有框架都要面对的取舍：默认值越好用，误用时的症状越隐蔽。

**架构推论：** `_delegate_default` 揭示了一个层级化封装的普遍问题：**外层声明的可选性、内层实际默认值、两者是否需要一致**。这里的答案是「外层只声明可选，值完全交给内层」，于是不需要在两层之间同步任何默认值。若外层也存一份默认值，就必须处理「内层改了默认值而外层没改」的漂移。

**架构推论：** `@super_component` 里「两次 `component` 处理 + 依赖 `run(self, **kwargs)` 不产生具名参数」这条链路非常脆弱，但它换来的是用户写法极简（定义 `pipeline` 属性 + 可选映射即可）。这类「机制复杂但用法简单」的设计，代价全部落在维护者身上——任何对 a01 元类逻辑的改动都要重新验证这条链路。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/core/super_component/super_component.py` | `SuperComponent` | 对外入口：继承 `_SuperComponent` 并提供 `to_dict` / `from_dict` / `show` / `draw` |
| `haystack/core/super_component/super_component.py` | `_SuperComponent` | 实际实现：映射解析、类型聚合、`run` 的三步翻译 |
| `haystack/core/super_component/super_component.py` | `_resolve_input_types_from_mapping` | 扇出类型聚合：求共同类型 + 必填性取或 |
| `haystack/core/super_component/super_component.py` | `_create_input_mapping` / `_create_output_mapping` | 默认映射：输入按名归并，输出同名冲突报错 |
| `haystack/core/super_component/super_component.py` | `_map_explicit_inputs` / `_map_explicit_outputs` | 运行期翻译：扇出写值、按键取回 |
| `haystack/core/super_component/super_component.py` | `_get_include_outputs_from` | 把被映射组件名塞进 `include_outputs_from` 以便取中间输出 |
| `haystack/core/super_component/super_component.py` | `super_component` | 装饰器：包 `__init__`、重建类、最后套 `component` |
| `haystack/core/super_component/utils.py` | `_is_compatible` | 先递归解包（Optional / Variadic）再做内容比较 |
| `haystack/core/super_component/utils.py` | `_types_are_compatible` | 对称匹配核心：`Any` 短路、Union 分支、泛型参数逐项比较 |
| `haystack/core/super_component/utils.py` | `_unwrap_variadics` / `_is_variadic_type` | 从 `Annotated[Iterable[T], 标记]` 里取出 `T` |
| `haystack/core/super_component/utils.py` | `_delegate_default` | 「默认值交给内层组件」的哨兵 |

## 参考

- [haystack 源码：core/super_component/super_component.py](https://github.com/deepset-ai/haystack/blob/main/haystack/core/super_component/super_component.py)
- [haystack 源码：core/super_component/utils.py](https://github.com/deepset-ai/haystack/blob/main/haystack/core/super_component/utils.py)
- [haystack 文档：超级组件](https://docs.haystack.deepset.ai/docs/supercomponents)
- [haystack 文档：组件](https://docs.haystack.deepset.ai/docs/components)

## 与主核 / AI 的关系

「把子图包装成单一组件」在主核里对应的是**视图与子查询的封装**：一张复杂查询计划被包装成一个可复用的关系，对外只暴露少量列。四条经验。

**第一，封装的映射方向决定了类型检查的形态。** 这里是「输入扇出（一对多）、输出一对一」。主核的视图定义同样是一个映射（视图列 → 基表列），且通常也是「一列对应一列」。若允许一个视图列由多个基表列合成，就必须定义合成语义（CONCAT？PICK？），而这不是框架该决定的。**判断准则：映射里凡是需要「合并语义」的方向，都不该做成默认支持。**

**第二，层级封装必须明确「默认值归属哪一层」。** `_delegate_default` 的做法是「外层只声明可选性，值由内层填」。主核里做「视图上的默认值」时同样要选边：外层存一份默认值会与内层的漂移，不存则外层无法单独使用。这里选了后者，代价是「超级组件的输入能不能省」与「内层组件的默认值」被强绑定。

**第三，同一个概念取两种口径的地方要显式说明。** 这里输出默认映射用「叶子输出」、校验用「含连接输出」，差一点就会把「映射中间输出」这个合法用法判成错误。主核里做「视图可见列」时，同样存在「默认可见」与「允许引用」两套范围——它们的差异必须写清楚，否则用户会遇到「明明存在却说找不到」。

**第四，`is not` 而非 `!=` 的哨兵比较，是数据生态里的硬性要求。** 注释点名 numpy/pandas/torch 覆写 `__ne__` 会崩。主核里若与 Python 数据生态交互（pandas DataFrame、numpy 数组进入元数据），任何「拿哨兵做相等比较」的代码都必须改成身份比较——而症状通常是「某个用户的 DataFrame 一进管道就炸」，本地复现不了。
