---
project: haystack
feature: breakpoints
title: 断点与管道快照
page_type: feature
schema_version: 1
refs:
  - path: haystack/dataclasses/breakpoints.py
    symbols: [Breakpoint, PipelineState, PipelineSnapshot]
  - path: haystack/core/pipeline/breakpoint.py
    symbols: [_create_pipeline_snapshot, load_pipeline_snapshot]
---

# 断点与管道快照

## 一句话职责

`Breakpoint` 声明「在某个组件的第 N 次执行之前停下来」，停下来时把整条管道的运行状态封成一个 `PipelineSnapshot`（输入、visit 计数、已产出的输出、原始入参、组件顺序），落成 JSON；`load_pipeline_snapshot` 读回来之后，`Pipeline.run` 能从这个状态**继续往下跑**——断点续跑的全部前提是「所有中间值都可序列化」。

## 机制是什么

断点续跑看似只是「存个状态再读回来」，但它逼出一个设计上的硬约束：**管道的运行状态必须能被完整表达成可序列化的数据**。

回头看 a02 的运行循环，状态其实只有五样东西：

| 状态 | 形态 | 序列化难点 |
|---|---|---|
| `inputs` | `{组件: {socket: [{sender, value}]}}` | `value` 是任意 Python 对象 |
| `component_visits` | `{组件名: 次数}` | 纯整数，无难点 |
| `pipeline_outputs` | `{组件名: 输出}` | 同 `value` |
| `original_input_data` | 用户最初传的 `data` | 同 `value` |
| `ordered_component_names` | 字符串列表 | 无难点 |

**四个有难点，全都落在「`value` 是任意对象」这一件事上。** 于是这套机制的核心不是断点本身，而是那套「带 schema 的序列化」。`_serialize_with_field_fallback` 的注释坦白了这个现实：

> Serialize a payload and, on failure, retry field-by-field to preserve serializable fields.

**序列化失败不是异常终止，而是降级**——逐字段重试，把能存的存下来、存不下的丢掉，并给每个丢掉打一条 warning。这是一个非常重要的设计决定：**宁可存一份残缺的快照，也不要因为某一个字段存不下就完全不存**。因为快照的价值在于「尽最大可能让你还能继续」，而不是「保证无损」。

而 `_create_pipeline_snapshot` 里那三处 `_serialize_with_field_fallback` 调用就是这个降级策略的三个落点：`inputs`、`original_input_data`、`pipeline_outputs` 各一次。

还有一层与 a03 的直接关联：**`_NoOutputProduced` 哨兵必须实现 `to_dict` / `from_dict`**。因为「上游明确无产出」这个标记本身会躺在 `inputs` 里被序列化；若它不可序列化，那么任何含「无产出」输入的快照都会在那一处降级丢掉字段——而丢掉的恰好是「这个 socket 已经被上游处理过但没给值」这个关键信息，续跑时就会把该组件重新触发。

## 怎么实现的

**三个 dataclass 分工明确，且都提供了对称的 `to_dict` / `from_dict`。**

```
@dataclass(frozen=True)
class Breakpoint:
    component_name: str
    visit_count: int = 0
    snapshot_file_path: str | None = None
```

`Breakpoint` 是 **`frozen=True`** 的——它是不可变的声明（「在这里断」），没有任何运行期状态需要修改。三个字段：

- **`component_name`**：断在哪。
- **`visit_count`**：断在第几次执行。默认 0 表示首次。
- **`snapshot_file_path`**：快照存哪。这个字段虽然是 `Breakpoint` 的一部分，实际用途是「告诉保存逻辑往哪里写」——所以它与「断在哪里」是两个概念被塞进同一个类里。原因是保存发生在 `run` 内部，而 `run` 只接收一个 `break_point` 参数，没有第二个地方能传存储位置。

`PipelineState` 与 `PipelineSnapshot` 都带 `_warn_on_inplace_mutation` 装饰器：

```
@_warn_on_inplace_mutation
@dataclass
class PipelineState:
    inputs: dict[str, Any]
    component_visits: dict[str, int]
    pipeline_outputs: dict[str, Any]
    inputs_format: str | None = None
```

**这两个类故意不是 `frozen`，但用装饰器警告「被就地修改」**。这是一个折中：快照在构造后往往还要被上层调整（例如补上时间戳、或测试里改一点），严格冻结会太碍事；但快照本质上是「某一时刻的不可变事实」，就地修改几乎总是 bug。于是用一个只警告不阻止的装饰器。

**`PipelineSnapshot.__post_init__` 做了一条跨字段一致性校验。**

```
components_in_state = set(self.pipeline_state.component_visits.keys())
components_in_order = set(self.ordered_component_names)

if components_in_state != components_in_order:
    raise ValueError(
        f"Inconsistent state: components in PipelineState.component_visits {components_in_state} "
        f"do not match components in PipelineSnapshot.ordered_component_names {components_in_order}")
```

**为什么要这个校验？** 因为续跑时 `component_visits` 决定「哪些组件还能跑」，`ordered_component_names` 决定「调度时要遍历哪些组件」。若两者集合不同，就会出现「某个组件永远不被考虑」或「某个组件没有 visit 计数」——**而这两种情况都不会报错，只会静默地少跑或多跑**。把它变成构造期的 `ValueError`，是把一个难以诊断的行为错误转成一个立刻可见的数据错误。

**`PipelineState.inputs_format` 承载了一次格式演进。**

```
inputs_format: str | None = None
```

docstring 把它讲得很清楚：`"internal"` 表示每个输入记录了发送者与到达顺序；`None` 表示**旧版快照**（Haystack 还没记录 sender 时），那种快照里每个 socket 只存一个扁平值，**只能从组件首次执行处续跑**。

对应的处理在 `_create_pipeline_snapshot` 与 `Pipeline.run` 两侧：

```
# 创建时永远写新格式
inputs_format=INTERNAL_INPUTS_FORMAT,

# 续跑时按格式分派
if pipeline_snapshot.pipeline_state.inputs_format == INTERNAL_INPUTS_FORMAT:
    inputs = _deserialize_value_with_schema(pipeline_snapshot.pipeline_state.inputs)
else:
    # 旧快照丢了 sender，只能当成「来自管道外」
    inputs = self._convert_to_internal_format(
        pipeline_inputs=_deserialize_value_with_schema(pipeline_snapshot.pipeline_state.inputs))
    legacy_resume_component = pipeline_snapshot.break_point.component_name
```

**`legacy_resume_component` 是一个专门的补丁变量**：旧快照续跑时，被暂停的那个组件需要「被放行一次」（因为它的输入被当成了管道外输入，而管道外输入只在首次执行时触发）。而注释说明了它必须是「一次性」的：

> Only the first execution of the paused component needs the legacy handling. Later visits of the same component inside a loop receive regular inputs and must consume them normally.

于是 `is_resume` 只有在 `legacy_resume_component == component_name` 时才为真，且用完立刻置 `None`。**这是一个「兼容性补丁」被精确限制作用域的样本**——没有它，循环里的第二次访问会被错误地当成 resume（a02 的 `_consume_component_inputs` 里 `is_resume` 分支会直接取第一个值不消费，那对非首次访问是错的）。

**`_create_pipeline_snapshot` 的关键在 `component_inputs` 这个额外参数。**

```
serialized_inputs = _serialize_with_field_fallback(
    {**inputs, component_name: component_inputs}, description="the inputs of the current pipeline state")
```

**它把「即将运行的那个组件的输入」合并进全局 `inputs` 再序列化。** 为什么？因为 a02 的循环里，输入在组件运行**之前**就被 `_consume_component_inputs` 从全局 `inputs` 里删掉了。而快照必须存「组件还没消费输入」的状态，否则续跑时那个组件就没有输入了。`Pipeline.run` 为此专门在消费前留了一份：

```
# A snapshot has to store the component's inputs as they were before the component consumed them, so
# that resuming re-triggers the component the same way this run did.
component_inputs_before_consume = inputs.get(component_name, {})
component_inputs = self._consume_component_inputs(...)
```

**这个「先备份再消费」的顺序是快照正确性的核心**，注释也明确写了这一点。若顺序反了，快照里那个组件就没有输入，续跑时它会永远 BLOCKED。

`_transform_json_structure` 则在序列化**原始输入**时做逆向摊平：

```
# For example: "key": [{"sender": null, "value": "some value"}] -> "key": "some value"
if isinstance(data, dict):
    if "value" in data and "sender" in data:
        return data["value"]
    return {k: _transform_json_structure(v) for k, v in data.items()}

if isinstance(data, list):
    transformed = [_transform_json_structure(item) for item in data]
    if len(data) == 1 and isinstance(data[0], dict) and "value" in data[0] and "sender" in data[0]:
        return transformed[0]
    return transformed
```

**为什么原始输入要摊平回去？** 因为 `original_input_data` 的用途是「让用户看到自己当初传了什么」，以及「重放时保持原样」。若把内部格式（带 sender 的列表）存进去，用户读快照时会看到一堆不该出现的内部结构。

**注意那个单元素列表的特殊处理**：`[{"sender":..., "value":...}]` 摊平成 `value` 而不是 `[value]`。这是为了让「单个输入」与「多个输入」在快照里形态一致——用户传的是 `{"query": "xxx"}`，快照里也应当是 `{"query": "xxx"}`，而不是 `{"query": ["xxx"]}`。

一个副作用值得记：**这个摊平是有损的**。若一个 socket 有两个上游各给一个值，摊平后得到 `[v1, v2]`；而单个上游给一个 list 值 `[v1, v2]`，摊平后也是 `[v1, v2]`。两者在快照里无法区分。因为 `original_input_data` 只用于展示与重放（重放时 `_convert_to_internal_format` 会重新包），这个歧义在实践上不影响正确性——但它是「为可读性牺牲信息」的一处实例。

**`_serialize_with_field_fallback` 的降级逻辑值得单独读。**

```
try:
    return _serialize_value_with_schema(_deepcopy_with_exceptions(payload))
except Exception as error:
    logger.warning("Failed to serialize {description}. Haystack will omit only the non-serializable fields when possible. Error: {e}", ...)

kept: dict[Any, Any] = {}
if isinstance(payload, dict):
    for field_name, value in payload.items():
        try:
            key = _deepcopy_with_exceptions(field_name)
            val = _deepcopy_with_exceptions(value)
            _serialize_value_with_schema(key)
            _serialize_value_with_schema(val)
        except Exception as field_error:
            logger.warning("Failed to serialize the '{field_name}' field of {description}. The field will be omitted from the snapshot. Error: {e}", ...)
```

四处：

1. **先尝试整体序列化**，只有失败才逐字段重试。这避免了「本来能一次成功，却因为逐字段处理而变慢」。
2. **键也要单独序列化**（注释说明理由：非字符串键用自己的 schema 表示，键不可序列化时该字段一并丢弃）。
3. **`_deepcopy_with_exceptions` 包了两层** ——因为「深拷贝」本身也可能失败（某些对象不支持 deepcopy），而它的失败与「序列化失败」是两回事。用同一个 helper 意味着两者共享同一套例外处理。
4. **两条 warning 的措辞不同**：整体失败时说「Haystack will omit only the non-serializable fields when possible」，单字段失败时明确说「The field will be omitted from the snapshot」。**第二条用 `will`，这是对结果的确定断言**——比含糊的「may fail」有用得多。

还有一个兜底细节写在 docstring 里：

> When the payload is not a mapping, or when every field fails to serialize, the helper returns a structurally valid empty-object payload so that the downstream `_deserialize_value_with_schema` can still load it back instead of raising `DeserializationError` on a bare `{}`.

**「返回一个结构合法的空对象」是关键**：因为 `_deserialize_value_with_schema` 要求输入必须含 `serialization_schema` 与 `serialized_data` 两个键，一个裸 `{}` 会让读取方直接抛 `DeserializationError`。于是写入方主动提供一个合法的空壳，让「空快照」至少能被读回来。

**`load_pipeline_snapshot` 把三种失败分别包成三条消息。**

```
try:
    with open(file_path, encoding="utf-8") as f:
        pipeline_snapshot_dict = json.load(f)
except FileNotFoundError as e:
    raise FileNotFoundError(f"File not found: {file_path}") from e
except json.JSONDecodeError as e:
    raise json.JSONDecodeError(f"Invalid JSON file {file_path}: {str(e)}", e.doc, e.pos) from e
except OSError as e:
    raise OSError(f"Error reading {file_path}: {str(e)}") from e

try:
    pipeline_snapshot = PipelineSnapshot.from_dict(pipeline_snapshot_dict)
except ValueError as e:
    raise ValueError(f"Invalid pipeline snapshot from {file_path}: {str(e)}") from e
```

三种 I/O 异常**都保留原类型**（而不是统一包成自定义异常），只是把文件路径拼进消息——因为「文件不存在」「JSON 坏了」「权限问题」对调用方意味着完全不同的处置。而构造失败只捕 `ValueError`，正好覆盖 `__post_init__` 那条一致性校验。

注意 `json.JSONDecodeError` 的那个构造：`f"Invalid JSON file {file_path}: {str(e)}"` 作为 `msg`，另外传 `e.doc` 与 `e.pos` 保持结构——**保留了原异常的结构字段**，于是上层若想拿到出错位置仍然可用。

**`_save_pipeline_snapshot` 的默认行为是「不保存」。**

```
if not _is_snapshot_save_enabled():
    logger.debug("Pipeline snapshot file saving is disabled via HAYSTACK_PIPELINE_SNAPSHOT_SAVE_ENABLED env var.")
    return None
```

**「落盘默认关闭、需要环境变量开启」** 是一个值得注意的默认值选择。理由是快照可能含敏感数据（用户的完整输入与中间结果），静默写盘是一个隐私风险。而环境变量 `HAYSTACK_PIPELINE_SNAPSHOT_SAVE_ENABLED` 只认 `"true"` 与 `"1"`（大小写不敏感），其余一概关闭——**默认安全、显式开启**。

三条保存路径的优先级也很清楚：

```
if snapshot_callback is not None:      # 1. 自定义回调优先，完全不碰文件系统
    ...
    return result

if not _is_snapshot_save_enabled():    # 2. 环境开关
    return None

if snapshot_file_path is None:          # 3. 断点没给路径就不存
    return None
```

**自定义回调的优先级高于环境开关**——因为回调是调用方显式提供的，其意图比一个全局环境变量更强。回调里若抛异常且 `raise_on_failure=False`，则记日志后返回 `None`；而 `raise_on_failure` 的值由调用方按错误类型决定：

```
full_file_path = _save_pipeline_snapshot(
    pipeline_snapshot=pipeline_snapshot,
    raise_on_failure=isinstance(error, BreakpointException),
    snapshot_callback=snapshot_callback,
)
```

**断点触发时必须保存成功（抛错），而通用运行错误时可以容忍保存失败。** 这条区分很实在：断点的全部意义就是「让你能续跑」，存不下就等于这个功能没生效；而 `PipelineRuntimeError` 场景下快照只是辅助诊断，存不下不该掩盖原始错误。

**文件名由「组件名 + visit 次数 + 时间戳」拼成。**

```
dt = pipeline_snapshot.timestamp or datetime.now()
visit_nr = pipeline_snapshot.pipeline_state.component_visits.get(component_name, 0)
timestamp = dt.strftime("%Y_%m_%d_%H_%M_%S")
file_name = f"{component_name}_{visit_nr}_{timestamp}.json"
```

三点：**用快照自己的 `timestamp` 而不是当前时间**（这样重放同一个快照时文件名稳定）；**`.get(..., 0)` 兜底**（万一没有该组件的计数）；**时间戳精确到秒**，于是同一秒内对同一组件同一 visit 的两次保存会覆盖——这在实践中几乎不会发生，但确实是一个（无害的）冲突面。

## 关键取舍

**源码事实：** `Breakpoint` 是 `frozen=True` dataclass（不可变声明）；`PipelineState` 与 `PipelineSnapshot` 不冻结，但都挂了 `_warn_on_inplace_mutation` 装饰器——意图是警告而不阻止。

**源码事实：** `PipelineSnapshot.__post_init__` 校验 `pipeline_state.component_visits` 的键集合与 `ordered_component_names` 完全一致，不一致抛 `ValueError`。

**源码事实：** `PipelineState.inputs_format` 区分新格式（`"internal"`，每输入带 sender 与到达顺序）与旧格式（`None`，每 socket 一个扁平值，只能从组件首次执行处续跑）；续跑旧快照时用一次性变量 `legacy_resume_component` 精确限制兼容补丁的作用域。

**源码事实：** `_create_pipeline_snapshot` 把「即将运行的组件的输入」合并进全局 `inputs` 一起序列化，因为那些输入已被 `_consume_component_inputs` 从全局状态里删除；`Pipeline.run` 为此在消费前显式备份 `component_inputs_before_consume`。

**源码事实：** `_transform_json_structure` 在序列化 `original_input_data` 前把内部格式摊平回用户形态（`[{"sender":..., "value": v}]` → `v`），单元素列表特判；这个摊平有损（无法区分「两个上游各给一值」与「一个上游给一个 list」）。

**源码事实：** 三处数据（`inputs` / `original_input_data` / `pipeline_outputs`）都用 `_serialize_with_field_fallback` 序列化：整体失败则逐字段重试，只丢弃不可序列化的字段并各打一条 warning；非映射或全部字段失败时返回**结构合法的空对象**，以避免读取方在裸 `{}` 上抛 `DeserializationError`。

**源码事实：** 快照默认**不落盘**，需 `HAYSTACK_PIPELINE_SNAPSHOT_SAVE_ENABLED` 为 `true`/`1`（大小写不敏感）才启用；自定义 `snapshot_callback` 的优先级高于该环境开关。

**源码事实：** 保存失败时的 `raise_on_failure` 由错误类型决定——`BreakpointException` 触发时必须成功（抛错），`PipelineRuntimeError` 触发时可容忍失败，避免掩盖原始错误。

**源码事实：** `load_pipeline_snapshot` 把 `FileNotFoundError` / `JSONDecodeError` / `OSError` 分别保留原类型并拼上路径，`PipelineSnapshot.from_dict` 的 `ValueError` 单独包装；`JSONDecodeError` 保留 `doc` 与 `pos` 结构字段。

**源码事实：** 快照文件名 = 组件名 + visit 次数 + 秒级时间戳（用快照自身 `timestamp`，无则取当前时间）；`visit` 计数取不到时兜底 0。

**架构推论：** 「断点续跑」真正的成本不在断点，而在**把运行状态变成一等公民**。一旦要求状态可序列化，任何藏在数据结构里的哨兵、句柄、闭包都会变成障碍——a03 的 `_NoOutputProduced` 必须实现 `to_dict`、`inputs` 必须用「带 sender 的列表」而不是进程内对象，都是这条要求的直接后果。反过来说，**这套机制的存在反过来约束了运行循环只能用「可序列化状态」表达自己**，这是一个很强的设计纪律。

**架构推论：** 「序列化失败降级为逐字段丢弃」的选择，把「快照完整性」从布尔量变成了程度量。这符合快照的用途——它是给人排查与续跑用的，缺一两个字段通常还能续，而完全失败则什么也留不下。代价是**用户必须读 warning 才知道快照缺了什么**，而「缺字段的快照续跑会不会出现诡异行为」这件事没有保证。

**架构推论：** `inputs_format` 这个演进式字段，展示了「格式升级 + 向后兼容」在数据格式类里的标准做法：**新写永远用新格式，读时按字段分派，旧格式走受限路径并显式标注其限制**（只能从首次执行处续跑）。比它更糟的两种做法是：读旧格式时猜（不可靠）、直接不兼容旧格式（破坏用户既有文件）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/dataclasses/breakpoints.py` | `Breakpoint` | 冻结的断点声明：组件名、visit 次数、快照存储路径 |
| `haystack/dataclasses/breakpoints.py` | `PipelineState` | 运行状态：输入、visit 计数、已产出输出、输入格式版本 |
| `haystack/dataclasses/breakpoints.py` | `PipelineSnapshot` | 完整快照：状态 + 断点 + 时间戳 + 原始入参 + 组件顺序 + `include_outputs_from` |
| `haystack/dataclasses/breakpoints.py` | `PipelineSnapshot.__post_init__` | 跨字段一致性校验（visit 键集合 == 组件顺序集合） |
| `haystack/dataclasses/breakpoints.py` | `INTERNAL_INPUTS_FORMAT` | 当前快照输入格式的标识常量 |
| `haystack/core/pipeline/breakpoint.py` | `_create_pipeline_snapshot` | 组装快照：把待运行组件的输入合并进全局输入后序列化 |
| `haystack/core/pipeline/breakpoint.py` | `load_pipeline_snapshot` | 读文件并按异常类型分别包装错误 |
| `haystack/core/pipeline/breakpoint.py` | `_save_pipeline_snapshot` | 回调优先、环境开关次之、路径最后；按错误类型决定是否容忍失败 |
| `haystack/core/pipeline/breakpoint.py` | `_transform_json_structure` | 原始入参从内部格式摊平回用户形态（有损） |
| `haystack/core/pipeline/breakpoint.py` | `_is_snapshot_save_enabled` | 默认关闭的落盘开关 |
| `haystack/core/pipeline/breakpoint.py` | `_validate_break_point_against_pipeline` / `_validate_pipeline_snapshot_against_pipeline` | 断点与快照对当前图的双向校验 |
| `haystack/utils/base_serialization.py` | `_serialize_with_field_fallback` | 整体失败则逐字段降级，只丢不可序列化字段 |
| `haystack/utils/base_serialization.py` | `_deserialize_value_with_schema` | 带 schema 的反序列化，要求两个必备键 |

## 参考

- [haystack 源码：dataclasses/breakpoints.py](https://github.com/deepset-ai/haystack/blob/main/haystack/dataclasses/breakpoints.py)
- [haystack 源码：core/pipeline/breakpoint.py](https://github.com/deepset-ai/haystack/blob/main/haystack/core/pipeline/breakpoint.py)
- [haystack 文档：管道](https://docs.haystack.deepset.ai/docs/pipelines)
- [haystack 文档：序列化管道](https://docs.haystack.deepset.ai/docs/serialization)

## 与主核 / AI 的关系

「把运行状态存下来再恢复」在主核里对应的是**检查点与崩溃恢复**：WAL、savepoint、查询执行的中间结果落盘。haystack 的断点在这张谱系里属于「用户主动触发的检查点」，四条经验。

**第一，状态可序列化是一条设计纪律，不是事后补的。** 这里为了让 `inputs` 可序列化，采用了「带 sender 的字典列表」而不是进程内对象；为了让哨兵可序列化，`_NoOutputProduced` 必须实现 `to_dict`。主核里若做「计划执行可中断可恢复」，同样必须在设计之初就规定「中间结果必须能被序列化到某种格式」——否则一旦某个算子把游标、句柄、闭包放进状态，恢复就无从谈起。

**第二，恢复点的语义必须精确到「消费前还是消费后」。** `component_inputs_before_consume` 那次备份，以及旧快照「只能从首次执行处续跑」的限制，都说明**恢复点落在一个操作的哪一侧是有语义的**。主核的 WAL 也一样：一条记录的「已写入但未提交」与「已提交」是两种恢复行为。设计检查点时要明确每个状态字段对应的是操作前还是操作后的快照。

**第三，序列化失败时降级而不是中止。** 逐字段重试、只丢不可序列化的字段、返回结构合法的空壳——这套做法把「快照完整性」变成了程度量。主核的崩溃转储同理：一份缺了几个不可读字段的转储，远比「什么都没写下来」有用。关键是**丢弃必须留痕**（这里的 warning），否则事后无法判断「这个字段是没有」还是「当时没存下来」。

**第四，默认不落盘是正确的默认值。** 快照含用户的完整输入与中间结果，静默写盘是隐私风险。主核里做「查询计划转储 / 慢查询采样」时同理：默认关闭、显式开启、并且提供「完全不走文件系统的回调入口」（这里的 `snapshot_callback`）让用户可以自己决定存到哪。这三种路径的优先级顺序（回调 > 开关 > 路径）也值得照搬——**调用方显式提供的手段，其优先级应当高于全局配置**。
