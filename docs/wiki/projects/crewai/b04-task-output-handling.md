---
project: crewai
feature: task-output-handling
title: 任务输出与结构化导出
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/task.py
    symbols: [check_output, _validate_output_file_path, artifact_kind_validation]
  - path: lib/crewai/src/crewai/utilities/task_output_storage_handler.py
    symbols: [TaskOutputStorageHandler, update, load]
---

# 任务输出与结构化导出

## 一句话职责

任务输出有两条落点：一是「格式」——把模型的自由文本转成 `pydantic` / `json` 两件套（`check_output` 在构造期保证三者互斥，导出在运行期做，`_validate_output_file_path` 与 `artifact_kind_validation` 保证写盘路径安全）；二是「持久化」——`TaskOutputStorageHandler` 把每次输出落进 SQLite，供重放与审计读取。

## 机制是什么

`Task` 上有四个互斥的输出声明位：`output_json`（要 JSON 字典）、`output_pydantic`（要模型实例）、`response_model`（想用模型的原生结构化输出能力）、`output_file` / `artifact`（要写到什么地方）。前两者是**后处理转换**，`response_model` 是**请求侧约束**，后两者是**持久化目标**。

`TaskOutput` 是最终载体，字段里既有 `raw`（原始文本）、也有 `pydantic` 与 `json_dict`（转换结果），还有 `output_format`（RAW / JSON / PYDANTIC 三选一）、`messages`（智能体最近对话）、`tool_failures`（工具失败记录）、`artifact`（保存后的产物引用）。这三件套是并存的而不是互斥的：`raw` 永远有值，结构化字段按配置填充。

「结构化失败不算失败」是一条重要约定：`_unpack_model_output` 在转换结果是一段字符串时，会尝试 `validate_model` 一次，若抛 `ValidationError` 就**返回 `(None, None)`**——也就是放弃结构化字段，但 `raw` 保留。任务本身不会因为结构化转换失败而失败。

## 怎么实现的

**构造期的三条互斥/安全校验。**

`check_output` 是最简单的一条：`output_types = [self.output_json, self.output_pydantic]`，去真值后数量大于 1 就抛 `PydanticCustomError("output_type", "Only one output type can be set, either output_pydantic or output_json.")`。它只查这两个，不管 `response_model`——`response_model` 可以在有 `output_pydantic` 时共存，因为前者是请求约束、后者是后处理。

`output_file_validation` 是 `@field_validator("output_file")`，值非空时转给 `_validate_output_file_path(value, allow_templates=True)`。

`_validate_output_file_path` 是这套机制里唯一的安全闸，判定顺序是四段：

1. 含 `..` 即抛「Path traversal attempts are not allowed」。
2. 以 `~` 或 `$` 开头即抛「Shell expansion characters are not allowed」。
3. 含 `|`、`>`、`<`、`&`、`;` 任一即抛「Shell special characters are not allowed」。
4. 含 `{` 或 `}`：若 `allow_templates=False` 直接抛「Unresolved template variables are not allowed in interpolated paths」；否则把每个 `{...}` 里的变量名取出来，逐个要求 `isidentifier()`，不合法抛「Invalid template variable name」。

最后若 `allow_templates` 为真且路径以 `/` 开头，**去掉前导斜杠**后返回；否则原样返回。这几个检查都是子串级别的（`".."` 出现在文件名中间也会被拒），是**保守的字符串过滤**而不是路径解析，所以它对「合法但刁钻」的路径会误伤，对「故意绕过」的路径能挡住大部分常见形态。

`artifact_kind_validation` 是 `@field_validator("artifact")`：值是 `Artifact` 实例时，先若有 `file_path` 就用 `_validate_output_file_path(..., allow_templates=True, label="artifact.file_path")` 校验，然后交给 `check_artifact_source`；其他值走 `return value or None`（把 `False` 归一成 `None`）。`check_artifact_source` 拒绝的是「file 类 artifact 没有 `file_path`」：`FILE_KINDS = frozenset({"image", "file"})`，这两类靠文件字节而不是文本产出，没有路径就不可能被填充，因此直接抛「artifact kind 'image' needs a file: set file_path to where the agent writes it.」。

**运行期的导出链。** `_export_output(result)` 的逻辑只有三行：若 `model := self.output_pydantic or self.output_json`（注意这是**取类对象**，不是取实例），就 `convert_to_model(result, self.output_pydantic, self.output_json, self.agent, self.converter_cls)`，再把结果交给 `_unpack_model_output(model_output, model, is_json_output=self.output_json is not None)`。没配模型则返回 `(None, None)`。

`_unpack_model_output` 的三种分支决定了「谁是 pydantic、谁是 json」：

- 字符串输入：先试 `validate_model(model_output, model, is_json_output)`，抛 `ValidationError` 就返回 `(None, None)`；成功则落到下面的模型分支。
- 是 `BaseModel`：返回 `(该模型, None)`。
- 其他（即 dict）：返回 `(None, dict)`。

所以配 `output_pydantic` 时拿到的是模型实例、`json_dict` 为空；配 `output_json` 时拿到的是字典、`pydantic` 为空。`_get_output_format` 与之严格对应：有 `output_json` → `OutputFormat.JSON`，有 `output_pydantic` → `PYDANTIC`，都没有 → `RAW`。

**落文件。** 内容选择由 `_output_content` 决定：`json_dict` 优先，其次 `pydantic.model_dump_json()`，最后 `raw`。`_save_file` 的流程：`Path(self.output_file).expanduser().resolve()` 得到绝对路径；`create_directory` 为真则 `mkdir(parents=True, exist_ok=True)`，为假且目录不存在则抛 `RuntimeError`；然后按类型写——dict 走 `json.dump(..., ensure_ascii=False, indent=2)`，其他 `file.write(str(result))`。`OSError` / `IOError` 被包成 `RuntimeError`，消息里带一段「建议用 crewai_tools 的 FileWriterTool」的提示（针对 Windows 换行等跨平台问题）。

**存 artifact。** `_save_artifact` 把 dict 内容先 `json.dumps(ensure_ascii=False, indent=2)` 拍平，再 `save_output_artifact(artifact, content=..., name=self.name or self.description, from_task=self)`。`save_output_artifact` 先 `unwrap_fence` 去掉最外层代码围栏（`"```" in match.group(2)` 时不剥，避免把「正文里含代码块」的文档误当成一整个围栏），再 `resolve_artifact` 把 `True` 展开成完整 `Artifact`：`identifier` 由名字小写、非 `[a-z0-9.]` 字符换成 `-`、截到 64 字符；`title` 把 `_` 与 `-` 换成空格；`kind` 优先取围栏语言映射（`_FENCE_KINDS` 里有 html/svg/json/csv/markdown/md/text/txt），否则用 `guess_kind` 按内容开头猜（`<!doctype html`/`<html` → html，`<svg` 或 `<?xml ... <svg` → svg，`{`/`[` 且能 `json.loads` → json，否则 markdown）。`file_path` 为空就用 `content` 存，非空就按文件存。失败只 `logger.warning` 并返回 `None`。

**持久化：两级门面。** `TaskOutputStorageHandler` 极薄——`__init__` 里建一个 `KickoffTaskOutputsSQLiteStorage`，`load()` 直接转交，`reset()` 转 `delete_all()`，`update(task_index, log)` 是唯一有逻辑的方法：

- 先 `load()`，为 `None` 抛「Logs cannot be None」。
- `log.get("was_replayed", False)` 为真时，从 `log` 里挑六个字段组成 `replayed` 字典（`task_id` 取 `str(log["task"].id)`、`task_key` 取 `log["task"].key`、`expected_output`、`output`、`was_replayed`、`inputs`），调 `storage.update(task_index, **replayed)`。
- 否则 `storage.add(**log)`。

这个分支解释了正常执行与重放在存储上的差别：**正常执行插入、重放原地更新**，且更新按 `task_index` 定位而不是 `task_id`。`add` 的方法签名是 `(task, output, task_index, inputs=None, was_replayed=False)`，与 `update` 接受的 `log` 字典形状不同——`_store_execution_log` 构造的 `log` 里 `task` 是任务对象、`output` 是字段字典（`description`/`summary`/`raw`/`pydantic`/`json_dict`/`output_format`/`agent`/`messages`）、另有 `task_index`、`inputs`、`was_replayed`。

SQLite 层的表结构（`latest_kickoff_task_outputs`）是 `task_id TEXT PRIMARY KEY`、`task_key TEXT`、`expected_output TEXT`、`output JSON`、`task_index INTEGER`、`inputs JSON`、`was_replayed BOOLEAN`、`timestamp DATETIME DEFAULT CURRENT_TIMESTAMP`。`_initialize_db` 在 `CREATE TABLE IF NOT EXISTS` 之后会 `PRAGMA table_info` 查一遍，若缺 `task_key` 就 `ALTER TABLE ADD COLUMN`——一次原地迁移。写操作全在 `store_lock(f"sqlite:{realpath}")` 里，库级 `PRAGMA journal_mode=WAL`；`load()` 是唯一不加锁的操作，用 `ORDER BY task_index` 保证顺序。JSON 列用 `CrewJSONEncoder` 序列化，读时 `json.loads`。

**`TaskOutput` 自身的两处小机制。** `set_summary` 是一个 `after` 校验器，把 `description` 前十个词拼成 `"<截断>..."` 赋给 `summary`——纯截断，没有任何模型参与，所以 `summary` 只是「短一点的描述」而不是摘要。`json` 属性在 `output_format` 不是 `JSON` 时抛 `ValueError`（带一段解释性提示），并在 `json_dict is None` 时返回 `None`——这是对「配了 `output_json` 但转换失败」的显式表达，调用方可以据此区分「没有 JSON」与「有 JSON 但是空」。

## 关键取舍

**源码事实：** `pydantic` 与 `json_dict` 互斥（同一个 `BaseModel` 结果按配置落到其中一个），而 `raw` 永远并存。所以「原始文本」是唯一不会被格式化弄丢的字段。

**源码事实：** 结构化转换失败被吞成 `(None, None)`，任务继续成功。这避免了「模型没按格式输出 → 整个流水线失败」，代价是调用方必须检查 `json_dict` 是否为 `None` 才能知道转换成功没有。

**源码事实：** `_validate_output_file_path` 用子串匹配拒绝 `..`、`~`、`$`、shell 特殊字符与未解析模板。这是「宁可误拒，也不放行可疑路径」的保守策略；含 `..` 的合法文件名会被误伤。

**源码事实：** 模板路径 `allow_templates=True` 时保留 `{var}` 并去掉前导 `/`，插值完成后再用 `allow_templates=False` 校验并通过 `validate_interpolated_path` 额外禁止「输入把路径变成绝对路径」。两级校验的分工是「构造期允许模板、运行期拒绝未解析与越权锚定」。

**源码事实：** `FILE_KINDS` 里的 `image` 与 `file` 必须有 `file_path`，否则构造期就报错。这挡住了「声明要存图片但任务只能产出文本」这种注定失败的配置。

**源码事实：** 重放走 `update` 且按 `task_index` 定位，不是按 `task_id`。这使重放能覆盖历史行，但也要求任务顺序稳定——顺序漂移会更新错行。

**架构推论：** 输出格式做成「构造期声明 + 运行期转换 + 失败可降级」三段，而不是「要求模型必须原样返回 JSON」。这承认了模型不可控的事实，把结构化降级成 optional enhancement。

**架构推论：** `output_file` 与 `artifact` 是两条并行的持久化路径，严格程度不同（前者失败抛 `RuntimeError`、后者失败只 warning）。因为文件是用户显式声明的主产出，artifact 是附加的可分享产物。

**架构推论：** 存储层用固定表名的单库（`latest_kickoff_task_outputs`）而不是每团队一库，意味着同一个团队多次执行会互相覆盖。这是为「重放最近一次」这个主用途服务的设计，不是审计日志。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/task.py` | `check_output` | 构造期校验：`output_json` 与 `output_pydantic` 互斥 |
| `lib/crewai/src/crewai/task.py` | `_validate_output_file_path` | 路径安全闸：拒绝穿越、shell 展开与未解析模板 |
| `lib/crewai/src/crewai/task.py` | `artifact_kind_validation` | artifact 字段校验：路径校验 + 拒绝无文件的 file 类 |
| `lib/crewai/src/crewai/task.py` | `_export_output` | 结构化导出：转换并按 `output_json` 与否拆成两件套 |
| `lib/crewai/src/crewai/task.py` | `_unpack_model_output` | 字符串先试校验、模型给 pydantic、字典给 json，失败降级为双 `None` |
| `lib/crewai/src/crewai/task.py` | `_save_file` | 写 `output_file`：解析路径、建目录、dict 走 JSON |
| `lib/crewai/src/crewai/utilities/task_output_storage_handler.py` | `TaskOutputStorageHandler` | 存储门面，封装 SQLite 实现 |
| `lib/crewai/src/crewai/utilities/task_output_storage_handler.py` | `update` | 重放走原地更新、正常执行走插入 |
| `lib/crewai/src/crewai/utilities/task_output_storage_handler.py` | `load` | 读取全部任务输出记录供重放使用 |
| `lib/crewai/src/crewai/tasks/task_output.py` | `TaskOutput` | 结果载体：`raw` 与结构化字段并存、`summary` 为描述截断 |

## 参考

- [CrewAI 源码：task.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/task.py)
- [CrewAI 源码：task_output_storage_handler.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/utilities/task_output_storage_handler.py)
- [CrewAI 文档：结构化任务输出](https://docs.crewai.com/en/concepts/tasks#structured-output)

## 与主核 / AI 的关系

这一页的两个主题，在主核里都能找到结构对应物。格式导出对应「序列化 + schema 校验」：主核也有「写入前按 schema 校验、失败就拒绝」的路径，区别在于主核拒绝即终止，这里的结构化转换失败会**静默降级**为无结构化字段，任务继续成功。持久化对应「把结果落盘供后续重读」，主核用的是自己的存储引擎与事务，这里用的是一张固定表的 SQLite 加文件锁。

这种差异的根源是输入端不同：主核处理的是确定性数据，格式错误意味着上游有 bug，应当立即暴露；这里处理的是模型输出，格式错误是可预期的常态，把它当成硬失败会让整条流水线可用性极差。因此框架选择了「保留 `raw`、尽力结构化」。代价是**契约从类型系统退化为字段判空**——调用方不能假设 `json_dict` 一定有值。

对 AI 工程的实际意义在于：真正可靠的产出永远要落到文件或数据库，而不是只活在内存里的 `TaskOutput` 对象。`output_file` 与 artifact 提供了两条落点，前者严格、后者宽松；而 `TaskOutputStorageHandler` 落的那张表只服务「最近一次运行的重放」，不是长期审计。分不清这两类持久化的用途，就会把「重放时被覆盖的历史」当成丢失的审计数据——这大概是这套机制最容易被误解的地方。
