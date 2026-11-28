---
project: ragas
feature: experiment
title: 实验与表格容器
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/experiment.py
    symbols: [Experiment, version_experiment, experiment]
  - path: src/ragas/dataset.py
    symbols: [DataTable, Dataset]
---

# 实验与表格容器

## 一句话职责

`DataTable` 是一个「行为像列表、但每次写入都同步到后端」的表格容器；`Experiment` 是它的实验变体；`@experiment` 装饰器把「一个函数 + 一份数据集」跑成一批结果并自动落盘；`version_experiment` 则把「代码版本」记进 Git 分支名与提交号。

## 机制是什么

这一层要解决的是「评测结果的存放与追溯」，围绕三件事：

**容器。** `DataTable` 用 `_data` 一个列表加 `backend` / `name` / `data_model` 三个属性，同时承担「内存列表」与「后端存储视图」两个身份。它不继承 `list`，而是实现 `__len__` / `__getitem__` / `__iter__` / `append` 等方法模仿列表行为。`Dataset` 与 `Experiment` 都只是它的一行子类声明，差别仅在一个类属性 `DATATABLE_TYPE`，而这个属性决定调后端的 `save_dataset` 还是 `save_experiment`。

**执行。** `@experiment` 装饰器把被装饰函数包成 `ExperimentWrapper`。它的 `arun(dataset, name, backend)` 是真正的执行入口：为每条数据建一个任务，用 `asyncio.as_completed` 并发跑，把每个非空返回值塞进实验容器，最后 `save()`。

**版本。** `version_experiment` 是一个独立的 Git 操作函数：若有未提交改动就提交，然后用固定前缀加实验名作为分支名，指向那个提交号。

## 怎么实现的

**双重身份与两个数据表示。** `_data` 里的元素可以是 Pydantic 模型，也可以是裸字典。这个宽容是有理由的：`data_model` 为 `None` 时（未校验模式）后端返回的就是字典，此时容器不能强制转成模型。于是每个方法都要先判类型：

- `save()` 遍历 `_data`，模型转 `model_dump()`、字典原样，其他类型直接 `TypeError`；
- `append()` 有模型时接受模型或字典（字典先校验成模型），无模型时**只接受字典**；
- `validate_with()` 要求当前容器处于未校验模式，把全部字典转成模型后返回一个**新容器**（不就地改）；
- `to_pandas()` 与 `save()` 用同一套转换逻辑。

所以「有模型 / 无模型」是容器的两种运行模式，不是装饰选项。`_resolve_backend` 是唯一把「后端名字符串」变成实例的地方：查注册表，查不到就抛 `ValueError` 并把**可用后端列表**一起打印出来——这是比 `KeyError` 有用的错误信息。

**`load` 的分派靠类属性。** 同一份代码要同时服务 `Dataset` 与 `Experiment`，用 `getattr(cls, "DATATABLE_TYPE", None)` 判断调 `load_dataset` 还是 `load_experiment`。`save` 与 `reload` 也走这套判断。这个手法的代价是：任何新增的表格类型都必须改这三处判断，而不是在基类上留一个抽象方法由子类实现。

**训练集与测试集切分的后端重置。** `train_test_split` 切完之后**不用原来的后端**，而是新建一个 `InMemoryBackend` 给两个子集，并立即 `save()`。理由是切分产物是临时数据，写回原后端会污染原始数据集。这个选择同时也说明「数据集的 backend 决定它的持久化位置」，切分等于换了一个存放地。

**`asyncio.as_completed` 的容错。** `arun` 对每个完成的任务做 `try/except Exception`：失败只打印一条警告，继续处理下一个，`finally` 里更新进度条。所以**单条数据失败不会中断整个实验**，最终结果里也不会有那条数据的记录——这是一个静默的丢弃。

注意任务的创建方式：`tasks.append(self(item, *args, **kwargs))`，即**调用装饰器包装的 `__call__`**，它对每个 `item` 传第一个位置参数。所以被装饰函数的第一个参数就是数据集里的一行，这是 `@experiment` 唯一的形状约定。

**后端解析的三级回退。** `arun` 里实验后端的决定顺序是：显式传入的 `backend` → 装饰器上的 `default_backend` → `dataset.backend`。第三级是隐式的：如果数据集本身已经绑了后端，实验就写进同一个后端。名字生成也有一层：没给 `name` 就用 `memorable_names.generate_unique_name()` 生成一个随机可读名，若装饰器带 `name_prefix` 还拼在前面。

**`Experiment` 的数据模型可选。** 构造实验容器时把 `self.experiment_model` 作为 `data_model` 传进去——所以装饰器若给了结果模型，每条结果都会被校验；没给就是未校验模式，`append` 只接受字典。这意味着**被装饰函数返回模型对象时必须同时在装饰器上声明该模型**，否则 `append` 会抛 `TypeError`。这是「返回类型」与「容器模式」之间必须一致的一处约束。

**版本函数的四个判断。** `version_experiment` 的流程很短，但每步都有分支：

1. **导入 GitPython**，失败就抛出带两条安装命令的 `ImportError`。可选依赖被显式表达。
2. **定位仓库**：`repo_path` 为空时用 `find_git_root()` 从当前目录向上找。
3. **暂存**：`stage_all=True` 时用 `repo.git.add(".")`（含未跟踪文件），否则 `repo.git.add("-u")`（只有已跟踪文件的改动）。**只有真的脏才有暂存动作**，干净时直接跳过并打印「No changes detected」。
4. **提交或复用**：有改动就 `repo.index.commit(message)` 拿新提交号；无改动就用 `repo.head.commit.hexsha`——**复用当前 HEAD**。

最后无条件创建（或试图创建）名为 `ragas/<实验名>` 的分支指向该提交。返回提交号。

注意「无改动时复用 HEAD」这个行为：它让「同一份代码上重复版本化」变成幂等的（分支名相同、指向同一提交），但如果同名分支已存在，`repo.create_head` 会抛异常——源码没有做「分支已存在则跳过或移动」的处理。

**版本号与结果之间没有自动关联。** `version_experiment` 返回的提交号只是一个字符串，`Experiment` 容器与 `ExperimentWrapper` 都不会把它记进结果里。要建立关联必须自己在实验函数里把提交号写进每行结果，或者从分支名反查。这是一个刻意留白：版本与数据的绑定方式取决于使用者的存储习惯。

## 关键取舍

**源码事实：** `DataTable` 不继承 `list`，而是手工实现列表协议并叠加后端同步。好处是「写入即持久化」；代价是任何绕过 `append` 的直接操作（比如改 `_data`）都不会同步到后端。

**源码事实：** 容器有「有模型 / 无模型」两种模式，`append` 在两种模式下的可接受类型不同。声明了 `experiment_model` 就必须返回该模型（或可校验的字典），否则 `append` 抛 `TypeError`。

**源码事实：** `load` / `save` / `reload` 用 `DATATABLE_TYPE` 字符串分派到后端的不同方法，而不是抽象方法。新增表格类型要改三处。

**源码事实：** `train_test_split` 给子集换用 `InMemoryBackend` 并立即保存，避免污染原后端。

**源码事实：** 实验单条失败只打印警告并跳过，不中断也不记录到结果里。

**源码事实：** `version_experiment` 在无改动时复用 HEAD，且对「同名分支已存在」没有处理——重复调用同名实验会抛异常。

**架构推论：** 「容器即存储视图」这个模型让 dtaset 与 experiment 共用一套读写路径，也让切换后端（本地 CSV / 本地 JSONL / 内存 / 云盘）成为纯配置。代价是容器的语义变重：它不再是一个纯粹的数据结构，而是「数据 + 位置 + 持久化策略」三者的绑定体。

**架构推论：** `@experiment` 把「数据集 → 结果集」这个映射做成一等公民，是相对 `evaluate` 的结构性改进。`evaluate` 的问题是「样本 × 指标」的结果全部摊在一个结果对象里，没有持久化、没有名字、没有版本；`@experiment` 恰好补齐这三样。这也解释了为什么 `evaluate` 被标记废弃而指向 `@experiment`。

**架构推论：** 版本函数独立于容器，而不是容器的字段，说明「代码版本」与「结果存储」被有意解耦。两者都属于「溯源信息」，但代码版本属于运行环境、结果存储属于产出，强行绑在一起会限制使用方式（比如同一份结果需要标注多个代码版本、或一次实验跨越多个提交）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/dataset.py` | `DataTable` | 列表式容器与后端同步的基础实现 |
| `src/ragas/dataset.py` | `DataTable._resolve_backend` | 后端名字符串到实例的解析与错误提示 |
| `src/ragas/dataset.py` | `DataTable.save` | 统一元素为字典后调后端保存 |
| `src/ragas/dataset.py` | `DataTable.load` | 按类属性分派到数据集或实验的加载 |
| `src/ragas/dataset.py` | `DataTable.append` | 按有无模型决定可接受类型 |
| `src/ragas/dataset.py` | `DataTable.train_test_split` | 切分子集并换用内存后端 |
| `src/ragas/dataset.py` | `Dataset` | 仅声明 `DATATABLE_TYPE` 的数据集子类 |
| `src/ragas/experiment.py` | `Experiment` | 仅声明 `DATATABLE_TYPE` 的实验子类 |
| `src/ragas/experiment.py` | `experiment` | 装饰器入口，构造执行包装器 |
| `src/ragas/experiment.py` | `ExperimentWrapper.arun` | 逐条并发执行、容错、汇总、落盘 |
| `src/ragas/experiment.py` | `version_experiment` | 暂存、提交、建分支并返回提交号 |
| `src/ragas/backends/base.py` | `BaseBackend` | 后端契约：数据集与实验分开的读写方法 |

## 参考

- [ragas 源码：dataset.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/dataset.py)
- [ragas 源码：experiment.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/experiment.py)
- [ragas 文档：实验结果与版本管理](https://docs.ragas.io/en/latest/concepts/experiment/)
- [GitPython 文档：仓库与引用](https://gitpython.readthedocs.io/en/stable/)

## 与主核 / AI 的关系

**「容器即存储视图」与主核里「表句柄不是数据本身」是同一个思路的两种写法。** 数据库的游标/表对象也不持有数据，读写都落到存储层；这里的容器看起来持有数据（有 `_data` 列表），但每次写入都同步到后端，本质仍是「内存缓存 + 存储视图」。差别在于数据库有事务保证原子性，这里每次 `append` 独立生效，中途失败会留下半个实验。

**「版本信息独立于数据」与主核的 MVCC 快照标识同构。** 数据库把「哪个事务读了哪个快照」记在事务上下文里，而不是塞进每一行；`version_experiment` 把提交号交给调用方，让调用方决定怎么关联。强行把版本号嵌进每条结果会造成冗余，且「一次实验对应多个提交」这种情形无法表达。

**静默丢弃是最危险的一类失败。** `arun` 对单条失败只警告并跳过。主核在批量导入时会记录每行的错误以便事后补跑，而这里丢弃之后没有任何痕迹——最终报告里少了几行，但没有任何字段说明「本来应该有几行」。**评测结果的完整性必须能被校验**，否则「跑了 100 条只得 95 条」这种问题永远不会被发现。

对 AI 工程最实用的五条：其一，**被装饰函数必须返回声明的模型类型**，返回字典也只有在未声明模型时才被接受。其二，**单条失败会被静默跳过**，务必自己统计「应有条数 vs 实际条数」。其三，**实验后端可能沿用它所属数据集的后端**，想写别处必须显式传 `backend`。其四，**`version_experiment` 不能重复调用同名实验**，同名分支存在时会抛异常，需要自己先删或改名。其五，**切分后的子集换了内存后端**，不会写回原始位置。
