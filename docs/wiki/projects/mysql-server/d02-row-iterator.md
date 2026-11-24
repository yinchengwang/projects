---
project: mysql-server
feature: row-iterator
page_type: feature
schema_version: 1
refs:
  - path: sql/iterators/row_iterator.h
    symbols: [RowIterator]
  - path: sql/iterators/composite_iterators.h
    symbols: [AppendIterator]
---

# 行迭代器框架

## 一句话职责

行迭代器框架用统一的 `RowIterator` 接口把「怎么取下一行」这件事抽象成 `Init()` + `Read()` 两步协议，让全表扫描、索引扫描、物化回读、过滤、连接等取行方式可以用同一种形状组合成一棵执行树；它只管接口与组合规则，不管单个迭代器内部的取行算法，也不负责生成这份执行树。

## 机制是什么

**这一层提供的是「协议 + 组合规则」，不是某一种取行算法。**

`sql/iterators/row_iterator.h` 里的 `RowIterator` 是所有取行方式的抽象基类。它把「一次取一行」这件事拆成两个虚函数：

- `virtual bool Init() = 0` —— 初始化或重新初始化迭代器。源码注释明确要求「必须在 `Read()` 之前调用 `Init()`」，但 `Init()` 本身不产生行。注释同时写明 `Init()` **可以反复调用**：后续调用会 rewind 迭代器（或按迭代器类型重新定位，例如取的是索引查找），从而允许重新读一遍记录。
- `virtual int Read() = 0` —— 读一行。返回值是三态协议，源码注释写得很明确：`0` 表示 OK、`-1` 表示记录结束（EOF）、`1` 表示错误。**行数据不由 `Read()` 返回**，而是放进表的记录缓冲区 `table->records[0]`；连接场景则是「表（们）的记录缓冲区」。

头文件顶部的注释给出了标准使用范式：

```
unique_ptr<RowIterator> iterator(new ...);
if (iterator->Init())
  return true;
while (iterator->Read() == 0) {
  ...
}
```

**接口本身刻意留了「不完整」的缝。** `RowIterator` 的类注释承认：「这个抽象并不完全严密。特别是它把一些细节留给了 `TABLE`，比如读哪些列（read_set）。这意味着照现在的样子直接排序两张表的 join 会很困难。」源码里对应的 TODO 注释记着同样的抱怨：外连接合成 NULL 行、以及 `EQRefIterator` 里的行缓冲问题，都指向同一个根因——**行是「待在表的缓冲区里」而不是「被显式传递的对象」**。

**基类还定义了三个横切所有迭代器的控制通道：**

- `SetNullRowFlag(bool is_null_row)` —— 把当前行缓冲标记成「全 NULL 行」或不是。注释说明它的用途是外连接：当一个迭代器没产出任何行时，需要产出一条 NULL 补齐行；标志置真后，从这个缓冲读出来的就全是 NULL，不管缓冲里实际残留着什么旧数据。注释特别提醒「`Init()` 或 `Read()` 不一定会重置这个标志」，而且**可以在没有调过 `Init()` 的情况下调用**——注释给的例子是 `NestedLoopIterator` 在外侧迭代器立刻 EOF 时，内侧迭代器拿不到 `Init()` 调用，但 NULL 行标志仍会转发给内外两侧。
- `UnlockRow()` —— 释放行锁。注释给出的背景是：`SELECT FOR UPDATE`、`UPDATE`、`DELETE` 在读行时会自动加锁，但在 `READ COMMITTED` 或更宽松的隔离级别下，如果一行没通过 `WHERE` 谓词就可以释放锁——因为只有被返回的行受保护，不是「哪些行被返回」受保护。注释明确要求：「如果你读了一行但实际没用它，之后应调 `UnlockRow()`」。注释还说 `TableRowIterator` 有默认实现，其他迭代器「通常要么把调用转发给源迭代器，要么直接忽略」。
- `StartPSIBatchMode()` / `EndPSIBatchModeIfStarted()` —— Performance Schema 批量模式。注释说明这个机制是为了降低 performance schema 的开销，「典型地应用在整个 join 最内层的表上」：先开始再结束，handler 调用会作为一组只计时一次，代价均摊，从而减少计时器开销。

  头文件把批量模式的规则写成三条明文约定：**(1)** 只有一个子迭代器的迭代器（如 `FilterIterator`）应把 `StartPSIBatchMode()` 转发给子迭代器；**(2)** 驱动某个迭代器的迭代器（用 for 循环读它）按 `PFSBatchMode` 类的描述使用批量模式；**(3)** 有多个子迭代器的迭代器忽略这个调用，自己按需处理。注释还给出了这套规则的整体效果：单表扫描时批量模式通常会被激活（因为在根迭代器上调 `StartPSIBatchMode()` 会一路下传到表迭代器），但在 join 场景下这个调用会被忽略，由 join 迭代器自己按需激活。

**组合规则由「组合迭代器」（composite iterator）承担。** `sql/iterators/composite_iterators.h` 的文件注释把这类迭代器定义为「接收一个或多个已有迭代器并以某种有意思的方式处理它们的行」：它们通常不绑定到单张表，而是执行树的**内部（非叶）节点**。注释给出一条所有权规则：**组合迭代器一致地拥有自己的源迭代器，但不拥有它的内存**（因为行迭代器从不在堆上分配——通常在 `MEM_ROOT` 上）；结果是最终会得到一个根迭代器，它递归地拥有其他所有迭代器。

文件注释同时指出，排序迭代器与两个窗口迭代器也是组合迭代器，但定义在自己的文件里。

**`AppendIterator` 是「多路顺序拼接」这一形态的组合迭代器。** 它的类注释写明：「接收两个或更多迭代器，顺序地输出它们的行（先输出第一个的全部行，然后是第二个的，以此类推）。用于实现 `UNION ALL`，通常与 `StreamingIterator` 一起使用。」

## 怎么实现的

### `RowIterator` 的接口形态与两条构造约束

`RowIterator` 的构造函数是 `explicit RowIterator(THD *thd) : m_thd(thd) {}`，持有 `THD *const m_thd`（私有 const 成员）。基类**禁拷贝**（`RowIterator(const RowIterator &) = delete;`），但**保留默认移动构造**（`RowIterator(RowIterator &&) = default;`）——头文件注释写明这条移动构造是给 `IndexRangeScanIterator` 用的。

头文件顶部还有一条使用约定：**迭代器通常应通过 `sql/iterators/timing_iterator.h` 里的 `NewIterator` 实例化**。

### `NewIterator`：统一的实例化入口

`sql/iterators/timing_iterator.h` 的 `NewIterator<RealIterator>(THD *thd, MEM_ROOT *mem_root, Args &&...args)` 是模板函数，逻辑只有两条分支：

- 若 `thd->lex->is_explain_analyze` 为真，返回 `new (mem_root) TimingIterator<RealIterator>(thd, ...)`；
- 否则返回 `new (mem_root) RealIterator(thd, ...)`。

注释说明了这个分支的意义：`EXPLAIN ANALYZE` 下迭代器被 `TimingIterator<T>` 包一层以收集计时信息，「因此几乎所有迭代器的实例化都应该走这个函数」。注释还写明内存约束：**在给定 `MEM_ROOT` 上分配新迭代器，`MEM_ROOT` 的寿命必须至少和迭代器一样长**。

`TimingIterator<RealIterator>` 是一个模板包装器：它内含一个 `RealIterator m_iterator` 成员，`Init()` 记录开始时间戳、调用 `m_iterator.Init()`、再调 `m_profiler.StopInit(start_time)`；`Read()` 同理，调用后调 `m_profiler.StopRead(start_time, err == 0)`——`err == 0` 即读成功，用来决定要不要累加行数。其余方法（`SetNullRowFlag`、`UnlockRow`、`StartPSIBatchMode`、`EndPSIBatchModeIfStarted`）直接转发。

`TimingIterator` 的注释里有一段重要的例外说明：**`MaterializeIterator` 不使用这个类**，因为它有内部成员迭代器 `m_table_iterator` 负责读物化结果，对它的 `Init()`/`Read()` 会再次经过 `MaterializeIterator` 自身的 `Init()`/`Read()`；如果套上 `TimingIterator`，`EXPLAIN ANALYZE` 输出里 `MaterializeIterator` 的耗时会变成「物化耗时 + 读结果耗时」的和，与其内部迭代器那一行重复。注释点名了对应的缺陷编号 Bug #33834146，并说明 `TemptableAggregateIterator` 同理。

### 组合迭代器的三种转发范式

`sql/iterators/composite_iterators.h` 里的组合迭代器在实现 `SetNullRowFlag` / `UnlockRow` / 批量模式这三个控制通道时，呈现出三种不同的处理方式：

1. **直接转发给源迭代器**——`FilterIterator` 的实现是 `m_source->SetNullRowFlag(is_null_row);`、`m_source->UnlockRow();`、`m_source->StartPSIBatchMode();`、`m_source->EndPSIBatchModeIfStarted();`。`LimitOffsetIterator`、`CacheInvalidatorIterator`、`WeedoutIterator`、`RemoveDuplicatesIterator`、`RemoveDuplicatesOnIndexIterator`、`MaterializeInformationSchemaTableIterator` 都是这一形态。
2. **转发给所有子迭代器**——`NestedLoopIterator` 的 `SetNullRowFlag` 同时转给 `m_source_outer` 与 `m_source_inner`；`EndPSIBatchModeIfStarted` 也同时转给两侧。`NestedLoopSemiJoinWithDuplicateRemovalIterator` 同理。
3. **故意不做任何事**——`AggregateIterator::UnlockRow()` 的函数体是空的，只有一段注释解释原因：多半是 HAVING 失败了；理想情况下想回退并解锁进入这个聚合的所有行，但做不到；也不能解锁**当前**行，因为当前行属于另一个组。所以什么都不做。
4. **按状态条件转发**——`NestedLoopIterator::UnlockRow()` 只在状态为 `READING_FIRST_INNER_ROW` 或 `READING_INNER_ROWS` 时才转给 `m_source_inner`。注释解释了原因：不知道是哪个条件否决了这行，因此也无法判断能不能一并解锁外侧行（外侧行可能还被其他 join 结果复用）。

### `FilterIterator`：过滤的组合形态

`FilterIterator` 是「接收行流并只放行满足条件的行」的组合迭代器，类注释说它「典型地用于 WHERE/HAVING」。

它的 `Init()` 是纯粹的一行转发：`return m_source->Init();`。真正的逻辑在 `Read()`（`sql/iterators/composite_iterators.cc`）：

```
int FilterIterator::Read() {
  for (;;) {
    int err = m_source->Read();
    if (err != 0) return err;

    bool matched = m_condition->val_int();

    if (thd()->killed) {
      thd()->send_kill_message();
      return 1;
    }

    /* check for errors evaluating the condition */
    if (thd()->is_error()) return 1;

    if (!matched) {
      m_source->UnlockRow();
      continue;
    }

    // Successful row.
    return 0;
  }
}
```

三个要点能从函数体直接读出：**(1)** 条件求值用 `m_condition->val_int()`，即条件是一个 `Item`；**}(2)** 求值后先查 `thd()->killed`，命中则调 `thd()->send_kill_message()` 并返回 `1`（错误）；**}(3)** 求值后查 `thd()->is_error()`，命中同样返回 `1`；**}(4)** 未匹配时调 `m_source->UnlockRow()` 再继续循环——**这正是 `RowIterator::UnlockRow()` 那段注释描述的场景**：读到了但没用的行要释放行锁。

### `LimitOffsetIterator`：LIMIT/OFFSET 的组合形态

`LimitOffsetIterator` 的类注释描述了它的协议：「处理 LIMIT 和/或 OFFSET；`Init()` 吃掉前 `offset` 行，`Read()` 在看到 `limit` 行后停止（包括被 offset 跳过的那些行）」。构造参数里 `limit` 可以是 `HA_POS_ERROR` 表示无限制，`offset` 可以是 `0` 表示无偏移。

`Init()`（`sql/iterators/composite_iterators.cc`）的逻辑是：若 `m_source->Init()` 返回真就返回真；否则若 `m_offset > 0`，置 `m_seen_rows = m_limit` 且 `m_needs_offset = true`，否则置 `m_seen_rows = 0`、`m_needs_offset = false`。

`Read()` 里有一段带具体理由的注释：offset 行的跳过**放在 `Read()` 而不是 `Init()` 里做**，因为「performance schema 批量模式可能还没被 executor 在第一次 `Read()` 前设置好」。这样做保证两件事：**(a)** 即使读的是 offset 行也能拿到批量模式的性能收益；**(b)** 不会在 `Init()` 期间意外开启批量模式（例如通过 `NestedLoopIterator` 开启），因为 executor 若在第一次 `Read()` 前出错就还没准备好**关闭**它。跳过 offset 行的循环里，每读一行都调 `m_source->UnlockRow()`，并把 `*m_skipped_rows` 递增。

`Read()` 还有一条标量子查询的基数检查分支：`m_reject_multiple_rows` 为真时，读到行之后检查 `m_seen_rows - m_offset > 0`，成立则调 `my_error(ER_SUBQUERY_NO_1_ROW, MYF(0))` 并返回 `1`。

### `NestedLoopIterator`：连接的状态机

`NestedLoopIterator` 的类注释写明：「一个简单的嵌套循环连接，接收两个迭代器（左/外侧与右/内侧）并把它们连接起来。它当然会多次扫描内侧迭代器。它是目前我们唯一一种 join 形式。」

它的注释还说明了性能模式的责任划分：「该迭代器负责在需要时对右侧迭代器激活 performance schema 批量模式；这典型地只在它是整个 join 的最内层表时才用（此时开启批量模式的收益最大，关掉它的精度损失也最小）。」

构造函数里有一条断言：`assert(m_source_outer != nullptr); assert(m_source_inner != nullptr);`，以及一条针对半连接的断言——`JoinType::ANTI` 或 `JoinType::SEMI` 时 `assert(!pfs_batch_mode);`，注释解释「批量模式对反连接或半连接没有意义，因为它们应该只读一行」。

`Init()` 的顺序是：先 `m_source_outer->Init()`，失败就返回真；置 `m_state = NEEDS_OUTER_ROW`；若 `m_pfs_batch_mode` 为真则调 `m_source_inner->EndPSIBatchModeIfStarted()`。

`Read()`（`sql/iterators/composite_iterators.cc`）是一个显式状态机，状态枚举为 `NEEDS_OUTER_ROW`、`READING_FIRST_INNER_ROW`、`READING_INNER_ROWS`、`END_OF_ROWS`。函数体的顺序是：

1. 若状态已是 `END_OF_ROWS`，直接返回 `-1`。
2. 状态为 `NEEDS_OUTER_ROW` 时读外侧行：`err == 1` 返回 `1`（错误）；`err == -1` 则置 `m_state = END_OF_ROWS` 并返回 `-1`；拿到行后，若 `m_pfs_batch_mode` 为真则对内侧调 `StartPSIBatchMode()`，然后**先** `m_source_inner->SetNullRowFlag(false)`，**再**调 `m_source_inner->Init()`，置状态为 `READING_FIRST_INNER_ROW`。

   这里有一处顺序上的细节带着注释：「`Init()` 可能会读取 NULL 行标志（例如在构建哈希表时），所以要在之前而不是之后把它取消掉。」
3. 读内侧行：`err != 0` 且处于批量模式时，对内侧调 `EndPSIBatchModeIfStarted()`；`err == 1` 返回 `1`；接着查 `thd()->killed`，命中则调 `thd()->send_kill_message()` 并返回 `1`。
4. `err == -1`（这个外侧行的内侧行读完了）时：**若连接类型是 `JoinType::OUTER` 且状态仍是 `READING_FIRST_INNER_ROW`（即一条内侧行都没匹配上），或连接类型是 `JoinType::ANTI`**，则对内侧调 `SetNullRowFlag(true)`、置状态回 `NEEDS_OUTER_ROW`、返回 `0`——这就是外连接的 NULL 补齐行；否则置状态回 `NEEDS_OUTER_ROW` 并继续循环。
5. 读到内侧行后：**`JoinType::ANTI`** 的连接一旦看到内侧有行就停止扫描内侧，且**不返回**那一行（置状态 `NEEDS_OUTER_ROW` 后 `continue`）；**`JoinType::SEMI`** 的连接在第一条内侧行后停止（置状态 `NEEDS_OUTER_ROW` 后返回 `0`）；内连接与外连接则置状态 `READING_INNER_ROWS` 继续扫内侧。

### `AppendIterator`：多路拼接的实际执行顺序

`AppendIterator` 的构造函数体只有一句断言：`assert(!m_sub_iterators.empty());`。

`Init()` 做三件事：`m_current_iterator_index = 0`、`m_pfs_batch_mode_enabled = false`、返回 `m_sub_iterators[0]->Init()`——**只初始化第一个子迭代器**。

`Read()` 的执行顺序可以直接从函数体读出：

1. 若 `m_current_iterator_index >= m_sub_iterators.size()`，返回 `-1`（所有子迭代器都已耗尽）。
2. 读当前子迭代器；`err != -1` 就直接返回（拿到一行，或出错）。
3. 遇到 EOF：先对当前子迭代器调 `EndPSIBatchModeIfStarted()`，然后 `++m_current_iterator_index`；若已越过末尾则返回 `-1`。
4. 对新的当前子迭代器调 `Init()`；失败返回 `1`。
5. 若 `m_pfs_batch_mode_enabled` 为真，对新的当前子迭代器调 `StartPSIBatchMode()`。
6. **递归调用 `Read()`** 自身，用新的当前迭代器重试。

批量模式的两个方法体现了「多子节点按规则 (3) 自己处理」：`StartPSIBatchMode()` 置 `m_pfs_batch_mode_enabled = true` 并**只对当前子迭代器**开启；`EndPSIBatchModeIfStarted()` 则**遍历全部子迭代器**逐个关闭，再置 `m_pfs_batch_mode_enabled = false`。`SetNullRowFlag` 与 `UnlockRow` 都只作用在当前子迭代器上，并各自带 `assert(m_current_iterator_index < m_sub_iterators.size());`。

### `CacheInvalidatorIterator`：代际计数器

`CacheInvalidatorIterator` 是一个专门用于「让物化器发现依赖已变化」的包装器。它的类注释写明：「每次有行从它这里经过，或它以任何其他方式改变状态，它就递增自己的『代际』计数器。这让 `MaterializeIterator` 能看出它的任何依赖是否变化过，然后强制重新物化——这典型地用于 LATERAL 表，即 join 里依赖更早位置的派生表。」

它的实现是纯机械的：`Init()` 是 `++m_generation; return m_source_iterator->Init();`，`Read()` 是 `++m_generation; return m_source_iterator->Read();`，`SetNullRowFlag()` 也是先 `++m_generation;` 再转发。计数器成员声明为 `int64_t m_generation = 0;`。

### 计划到迭代器的装配：`CreateIteratorFromAccessPath`

迭代器树不是手工搭的，而是由 `sql/join_optimizer/access_path.cc` 的 `CreateIteratorFromAccessPath()` 从访问路径树装配出来。

`sql/join_optimizer/access_path.h` 的 `AccessPath` 结构体注释解释了这条对应关系：「访问路径是一种查询规划结构，与迭代器 1:1 对应，一个访问路径包含几乎正好是实例化给定迭代器所需的信息，外加一些只在规划期需要的信息（如代价）。（新的 join 优化器未来会扩展这一点。）」`AccessPath` 里有一个 `RowIterator *iterator = nullptr;` 成员，注释说：「如果已经为这个访问路径实例化了迭代器，就指向它。用于构造需要互相通信的迭代器。」

`CreateIteratorFromAccessPath()` 的实现说明了装配策略：它不递归，而是用一个 `MEM_ROOT` 支撑的 `todo` 栈（`Mem_root_array<IteratorToBeCreated>`）。注释写明原因：「访问路径树可能相当深，在某些编译器/配置下栈帧也很大，所以不用显式递归，而是把任务压到一个 `MEM_ROOT` 支撑的栈上。这会多用一点内存（`MEM_ROOT` 一般活到查询结束），但大幅减少栈的使用。」注释给出的通用规则是：需要子节点的迭代器会把它子节点的访问路径任务压栈，然后把自己重新压栈；子节点实例化完成后回到原迭代器时，才真正实例化自己。注释说明区分依据是「`job.children` 是否已分配」。

`sql/join_optimizer/access_path.cc` 的 `switch (path->type)` 分派里能看到全部迭代器类型与访问路径类型的对应关系，例如 `AccessPath::TABLE_SCAN` → `NewIterator<TableScanIterator>`、`AccessPath::FILTER` → `NewIterator<FilterIterator>`、`AccessPath::NESTED_LOOP_JOIN` → `NewIterator<NestedLoopIterator>`、`AccessPath::WINDOW` → `NewIterator<BufferingWindowIterator>` 或 `NewIterator<WindowIterator>`、`AccessPath::WEEDOUT` → `NewIterator<WeedoutIterator>`、`AccessPath::REMOVE_DUPLICATES` → `NewIterator<RemoveDuplicatesIterator>`。

`AccessPath::APPEND` 分支的装配顺序值得单列（`sql/join_optimizer/access_path.cc`）：子节点先被压栈并标记 `eligible_for_batch_mode=true`；子节点全部实例化后，代码把 `job.children` 逐个 `std::move` 进一个 `vector<unique_ptr_destroy_only<RowIterator>> children`，再调 `NewIterator<AppendIterator>(thd, mem_root, std::move(children))`。

### 叶节点：`TableRowIterator` 与 `TableScanIterator`

`sql/iterators/row_iterator.h` 里的 `TableRowIterator` 是 `RowIterator` 的第二个基类，负责所有「绑定到一张表」的迭代器的公共实现。它覆写了 `UnlockRow`、`SetNullRowFlag`、`StartPSIBatchMode`、`EndPSIBatchModeIfStarted` 四个方法，并声明了受保护成员 `HandleError(int error)` 与 `PrintError(int error)`。

`sql/iterators/basic_row_iterators.cc` 里这四个方法的实现是：

- `TableRowIterator::UnlockRow()` —— `m_table->file->unlock_row();`，注释写明这是「`RowIterator` 的 unlock-row 方法的默认实现，用于除 `EQRefIterator` 之外的所有访问方法」。
- `TableRowIterator::SetNullRowFlag(bool is_null_row)` —— 为真时调 `m_table->set_null_row()`，否则调 `m_table->reset_null_row()`。
- `TableRowIterator::StartPSIBatchMode()` —— `m_table->file->start_psi_batch_mode();`。
- `TableRowIterator::EndPSIBatchModeIfStarted()` —— `m_table->file->end_psi_batch_mode_if_started();`。

**错误分支集中在 `TableRowIterator::HandleError()` 里**，它把 handler 层的错误码翻译成迭代器协议的三态返回值：

```
int TableRowIterator::HandleError(int error) {
  if (thd()->killed) {
    thd()->send_kill_message();
    return 1;
  }

  if (error == HA_ERR_END_OF_FILE || error == HA_ERR_KEY_NOT_FOUND) {
    m_table->set_no_row();
    return -1;
  } else {
    PrintError(error);
    return 1;
  }
}
```

即：**`HA_ERR_END_OF_FILE` 与 `HA_ERR_KEY_NOT_FOUND` 被当作 EOF**（返回 `-1`，并调 `m_table->set_no_row()`），其余错误调 `PrintError(error)`（内部是 `m_table->file->print_error(error, MYF(0))`）并返回 `1`。**`thd()->killed` 优先于一切**，命中就发 kill 消息并返回 `1`。

`TableScanIterator::Init()` 的顺序是：`empty_record(table())`；算 `first_init = !table()->file->inited`（注释说明「只在 handler 首次初始化时才尝试分配记录缓冲」）；调 `table()->file->ha_rnd_init(true)`，出错则 `PrintError(error)` 并返回真；若是首次初始化则调 `set_record_buffer(table(), m_expected_rows)`；最后置 `m_stored_rows = 0`。

`TableScanIterator::Read()` 读行用 `table()->file->ha_rnd_next(m_record)`，并处理一个 MyISAM 特有的错误码：`HA_ERR_RECORD_DELETED` 在「一个线程在读、另一个线程在无锁删除」时会被返回，此时 `if (tmp == HA_ERR_RECORD_DELETED && !thd()->killed) continue;`——即跳过这一行继续读；否则走 `HandleError(tmp)`。

`TableScanIterator::Read()` 里还有一条行数上限判断：`if (++m_stored_rows > m_limit_rows) { return HandleError(HA_ERR_END_OF_FILE); }`。`m_limit_rows` 的来源在构造函数里：`table->set_counter() != nullptr ? table->m_limit_rows : HA_POS_ERROR`——即只有表带了计数器时才生效。

### 物化：`MaterializeIterator` 的两条路径

`sql/iterators/composite_iterators.cc` 里 `MaterializeIterator<Profiler>::Init()` 的逻辑分成「需要重新物化」和「只需重扫」两条路：

- 若 `table()->materialized` 为真且不需要重扫，直接 `return m_table_iterator->Init();`——源码注释写「只是同一张表的再次扫描」。
- 需要重扫的判据是**代际计数器变了**：遍历 `m_invalidators`，若某个 `invalidator.iterator->generation() != invalidator.generation_at_last_materialize`，则置 `rematerialize = true`。源码注释解释这是为了「看看自上次以来我们依赖的 LATERAL 表有没有变过（这会强制重新物化）」，并附了一条 TODO：更好的做法是检查实际列值而不只是「有没有见到新行」。
- 需要重新物化时：调 `table()->set_not_started()`；若表还没建过则调 `instantiate_tmp_table(thd(), table())`，否则调 `table()->file->ha_index_or_rnd_end()` 后 `table()->file->ha_delete_all_rows()` 清空旧数据。

## 关键取舍

- **源码事实：行数据留在表的记录缓冲区里，而不是由 `Read()` 返回。** `RowIterator::Read()` 的注释写明行数据「被放进表的（或连接时表们的）记录缓冲区，即 `table->records[0]`」。收益是省掉每次投影/拷贝，代价由注释直接承认：抽象「不严密」，「读哪些列（read_set）」的细节留在 `TABLE` 上，「照现在的样子直接排序两张表的 join 会很困难」，并且 TODO 里把「外连接合成 NULL 行」和 `EQRefIterator` 都记为这个设计遗留的债。

- **源码事实：组合迭代器拥有子迭代器，但不拥有其内存。** 注释明确说明「因为行迭代器从不在堆上分配——通常在 `MEM_ROOT` 上」，所以递归所有权最终汇聚到一个根迭代器。收益是销毁只需析构根节点；代价是迭代器的寿命被绑到 `MEM_ROOT` 的寿命上——`NewIterator` 的注释因此要求「`MEM_ROOT` 的寿命必须至少和迭代器一样长」。

- **源码事实：批量模式的转发规则被写成三条明文约定，而不是靠基类自动传播。** 约定 (1) 单子节点转发、(2) 驱动方按 `PFSBatchMode` 类使用、(3) 多子节点自己处理。收益是 join 场景能按连接类型选择是否开启（`NestedLoopIterator` 只在成为最内层表时开启）；代价是每个新迭代器都要自己遵守约定，写错的后果是批量模式泄漏——头文件注释因此要求「在根迭代器上扫完时调 `EndPSIBatchModeIfStarted()`」作为兜底。

- **源码事实：`EXPLAIN ANALYZE` 的计时是插在框架层的，不是每个迭代器各写一份。** `NewIterator` 在 `thd->lex->is_explain_analyze` 为真时统一包一层 `TimingIterator<RealIterator>`。收益是所有迭代器零成本获得计时；代价是 `MaterializeIterator` 与 `TemptableAggregateIterator` 必须**排除**在外，因为它们的内部迭代器会让同一段时间被计入两处（Bug #33834146）。

- **源码事实：`AppendIterator` 逐个初始化、逐个切换子迭代器。** `Init()` 只初始化 `m_sub_iterators[0]`；`Read()` 遇到 EOF 才 `EndPSIBatchModeIfStarted()` + 递增索引 + 对新子迭代器调 `Init()`，末尾递归调用自身。收益是同一时刻只持有活跃子迭代器的一次批量模式状态；代价是每次切换都要走一遍 Init/批量模式的开关配对，且末尾那步用递归实现——子迭代器数量大时递归深度等于子迭代器数量。

- **源码事实：`AggregateIterator` 放弃解锁行。** `UnlockRow()` 的实现是空函数体加一段注释：不能回退解锁进入聚合的行，也不能解锁当前行（当前行属于另一个组）。这是明确的正确性优先于资源释放的取舍。

- **源码事实：`NestedLoopIterator` 的解锁是状态相关的。** 只有在 `READING_FIRST_INNER_ROW` 或 `READING_INNER_ROWS` 状态才转给内侧；注释解释是不知道哪个条件否决了这行，也就无法判断外侧行能否解锁（它可能还被其他 join 结果复用）。

- **源码事实：`FilterIterator` 在条件不匹配时解锁源行。** 函数体里的 `m_source->UnlockRow(); continue;` 与 `RowIterator::UnlockRow()` 上层注释描述的场景一一对应——读到了但没用的行要释放行锁，避免在 `READ COMMITTED` 下持有不需要的锁。

- **架构推论：** `RowIterator` 协议里 `Init()` 可重复调用这一条，是整个框架能支撑「内层迭代器被反复扫描」的前提——嵌套循环连接每换一个外侧行就要对内侧重新 `Init()` 一次（`NestedLoopIterator::Read()` 里每轮都调 `m_source_inner->Init()`）。MySQL 把「重新定位」的责任下放给各具体迭代器（注释说「rewind 迭代器，或按迭代器类型重新定位，例如取的是索引查找」），代价是每个迭代器都得自己实现正确的重入语义。

- **架构推论：** `AccessPath` 与 `RowIterator` 的 1:1 对应，是把「计划」与「执行」解耦的关键设计——同一棵访问路径树可以在不同优化器（传统优化器与超图优化器）下产出，但最终都被折叠成同一种迭代器协议。代价是访问路径结构必须携带足以实例化迭代器的全部信息，`AccessPath` 的注释因此提到它「包含通用信息（类型标识、代价等）加上至多 40 字节的类型特定信息」，并坦承想把它压进一个缓存行（64 字节）却「不容易」。

- **架构推论：** 框架把「锁的释放」做成迭代器协议的一等方法（`UnlockRow`）而不是执行器的一次性后处理，是因为行锁是否需要释放取决于「这行有没有被真正使用」，这个信息只有沿着迭代器链向下传递才能拿到。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/iterators/row_iterator.h` | `RowIterator` | 迭代器抽象基类：`Init()`/`Read()` 两步协议、`Read()` 的 0/-1/1 三态返回约定、行留在记录缓冲区的约定、`SetNullRowFlag`/`UnlockRow`/PSI 批量模式三个控制通道 |
| `sql/iterators/row_iterator.h` | `TableRowIterator` | 绑定到单表的迭代器公共基类，覆写解锁、NULL 行标志与 PSI 批量模式四个方法，并声明 `HandleError`/`PrintError` |
| `sql/iterators/composite_iterators.h` | `AppendIterator` | 组合迭代器之一：顺序拼接两个或多个子迭代器的行，用于实现 `UNION ALL`；持有子迭代器索引与批量模式开关状态 |
| `sql/iterators/composite_iterators.h` | `FilterIterator` | 组合迭代器：按 `Item` 条件过滤行流，`Init()` 转发、`Read()` 内循环求值条件并在不匹配时解锁源行 |
| `sql/iterators/composite_iterators.h` | `LimitOffsetIterator` | 组合迭代器：处理 LIMIT/OFFSET，`Init()` 吃掉 offset 行、`Read()` 在达到 limit 后停止 |
| `sql/iterators/composite_iterators.h` | `NestedLoopIterator` | 组合迭代器：嵌套循环连接状态机，四态枚举驱动内外侧推进，按 `JoinType` 区分 OUTER/SEMI/ANTI 的停止与 NULL 补齐行为 |
| `sql/iterators/composite_iterators.h` | `AggregateIterator` | 组合迭代器：分组聚合，用状态枚举处理组切换与 rollup 输出；`UnlockRow()` 是空实现并带原因注释 |
| `sql/iterators/composite_iterators.h` | `CacheInvalidatorIterator` | 组合迭代器：包装源迭代器并递增代际计数器，供 `MaterializeIterator` 判断依赖是否变化以决定重新物化 |
| `sql/iterators/composite_iterators.h` | `WeedoutIterator` | 组合迭代器：半连接 weedout，流式去重，用 `SJ_TMP_TABLE` 记录已见行 ID |
| `sql/iterators/composite_iterators.h` | `StreamingIterator` | 组合迭代器：不真正物化，每次 `Read()` 直接转发给子查询迭代器并做字段拷贝 |
| `sql/iterators/composite_iterators.cc` | `FilterIterator::Read` | 过滤实际执行：读源行、`m_condition->val_int()` 求值、查 `killed` 与 `is_error()`、不匹配时 `UnlockRow()` |
| `sql/iterators/composite_iterators.cc` | `LimitOffsetIterator::Read` | LIMIT/OFFSET 实际执行：offset 跳过放在 Read 而非 Init（批量模式原因）、跳过行逐行解锁、`m_reject_multiple_rows` 时的 `ER_SUBQUERY_NO_1_ROW` 基数检查 |
| `sql/iterators/composite_iterators.cc` | `NestedLoopIterator::Read` | 连接实际执行：状态机推进、外侧 EOF 时返回 `-1`、OUTER/ANTI 的 NULL 补齐、ANTI 见到内侧行即停、SEMI 首行即停 |
| `sql/iterators/composite_iterators.cc` | `AppendIterator::Read` | 拼接实际执行：当前子迭代器 EOF 后关批量模式、递增索引、对新子迭代器 `Init()`、末尾递归重试 |
| `sql/iterators/composite_iterators.cc` | `AppendIterator::StartPSIBatchMode` / `EndPSIBatchModeIfStarted` | 批量模式的多子节点处理：开启只作用当前子迭代器，关闭遍历全部子迭代器 |
| `sql/iterators/composite_iterators.cc` | `MaterializeIterator<Profiler>::Init` | 物化实际执行：首次建表或清空旧表、按 `generation()` 变化决定重新物化还是仅重扫 |
| `sql/iterators/composite_iterators.cc` | `WeedoutIterator::Read` | weedout 实际执行：取行 ID、调用 `do_sj_dups_weedout` 判重、重复则读下一行 |
| `sql/iterators/timing_iterator.h` | `NewIterator` | 统一实例化入口：`EXPLAIN ANALYZE` 下包 `TimingIterator<RealIterator>`，在 `MEM_ROOT` 上分配 |
| `sql/iterators/timing_iterator.h` | `TimingIterator` | 计时包装器：转发全部方法，`Init`/`Read` 记录耗时与行数；注释说明 `MaterializeIterator` 为何要排除在外（Bug #33834146） |
| `sql/iterators/basic_row_iterators.cc` | `TableRowIterator::HandleError` | 错误翻译：`HA_ERR_END_OF_FILE` 与 `HA_ERR_KEY_NOT_FOUND` 视为 EOF 返回 `-1`，其余打印错误返回 `1`，`killed` 优先 |
| `sql/iterators/basic_row_iterators.cc` | `TableScanIterator::Init` | 全表扫描初始化：`empty_record`、`ha_rnd_init(true)`、首次初始化才 `set_record_buffer` |
| `sql/iterators/basic_row_iterators.cc` | `TableScanIterator::Read` | 全表扫描读行：`ha_rnd_next` 循环、`HA_ERR_RECORD_DELETED` 跳过、`m_stored_rows > m_limit_rows` 时按 EOF 处理 |
| `sql/join_optimizer/access_path.cc` | `CreateIteratorFromAccessPath` | 迭代器树装配：用 `MEM_ROOT` 支撑的 `todo` 栈代替递归，按 `AccessPath::type` 分派到各 `NewIterator<>` |
| `sql/join_optimizer/access_path.h` | `AccessPath` | 规划结构，与迭代器 1:1 对应；含 `RowIterator *iterator` 成员用于需要互相通信的迭代器构造 |
| `sql/sql_executor.cc` | `init_table_iterator` | 执行器侧入口：按 unique 结果缓冲状态选 `SortFileIndirectIterator`/`SortBufferIndirectIterator`，否则建表访问路径再 `CreateIteratorFromAccessPath`，最后调 `Init()` |

## 与主核 / AI 的关系

主核（`engine/db`）目前没有「行迭代器框架」这一层。`engine/db` 的查询执行路径是「扫表 → 过滤 → 输出」的固定形态，没有可替换的取行实现，也没有把过滤、连接、聚合、物化抽象成可组合节点的通用协议。

**架构推论：** MySQL 这套框架里有三点结构值得主核借鉴。其一是**「行留在表的缓冲区」这个决定带来的两难**：它省掉了投影拷贝，但把「外连接合成 NULL 行」「EQRefIterator 的缓存」「排序多表 join」三件事都变成了难缠的特例（源码注释自己把这些列为待重构的债）。主核若在设计迭代器协议时就把「行是一个被显式传递的值对象」，能直接免掉这三笔债——代价是每次读行都要处理行的所有权与生命周期。其二是**「错误分支集中在一个 `HandleError` 里」**：MySQL 把 handler 层的错误码（`HA_ERR_END_OF_FILE`、`HA_ERR_KEY_NOT_FOUND`）翻译成迭代器协议的 `-1`，让上层迭代器只需要处理 `-1/0/1` 三态。主核若有多个存储引擎访问路径，这个翻译层是天然的收敛点。其三是**「控制通道走基类接口、子类按规则转发」**：解锁行、NULL 行标志、批量模式这三个横切能力被做成了协议的一部分而不是执行器的一次性后处理，代价是每个新迭代器都要遵守转发约定。

对 AI 场景（模型生成 SQL 并由数据库执行）而言，这一层决定的是「同一个语义有多少种落地形态」以及「这些形态的执行代价差多少」。**架构推论：** MySQL 的 `EXPLAIN ANALYZE` 通过 `TimingIterator` 把每个迭代器的首行耗时、其余行耗时、`Init()` 次数、读出行数都暴露出来，这正是 AI 判断「我写的这条 SQL 慢在哪一层」所需的信息——但它要求 AI 能把执行计划树和 SQL 语义对应起来。主核若要让 AI 参与执行策略选择（而不只是生成 SQL 文本），应当把「每个算子的实际行数与耗时」做成可查询的结构化对象，并且像 MySQL 那样把「为什么走了这条算子路径」也记录下来。

## 参考

- [MySQL 源码：`sql/iterators/row_iterator.h`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/row_iterator.h) —— `RowIterator` 抽象基类与 `TableRowIterator` 的完整定义。
- [MySQL 源码：`sql/iterators/composite_iterators.h`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/composite_iterators.h) —— 组合迭代器族（`AppendIterator`、`FilterIterator`、`NestedLoopIterator`、`MaterializeIterator` 等）的声明与所有权约定。
- [MySQL 源码：`sql/iterators/composite_iterators.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/composite_iterators.cc) —— 组合迭代器的实际执行顺序与错误分支。
- [MySQL 源码：`sql/iterators/timing_iterator.h`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/timing_iterator.h) —— `NewIterator` 统一实例化入口与 `EXPLAIN ANALYZE` 的计时机制。
- [MySQL 源码：`sql/join_optimizer/access_path.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/join_optimizer/access_path.cc) —— 访问路径到迭代器树的装配过程。
- [MySQL 参考手册：EXPLAIN ANALYZE](https://dev.mysql.com/doc/refman/8.0/en/explain-analyze.html) —— `EXPLAIN ANALYZE` 的输出与实际执行计时语义。
- [MySQL 参考手册：EXPLAIN 输出格式](https://dev.mysql.com/doc/refman/8.0/en/explain-output.html) —— 访问路径类型与执行计划输出的对应关系。
- [MySQL 参考手册：JOIN 与连接优化](https://dev.mysql.com/doc/refman/8.0/en/join.html) —— 嵌套循环连接等连接形态的语义。
- [MySQL 参考手册：Performance Schema 事件语句表](https://dev.mysql.com/doc/refman/8.0/en/performance-schema-events-statements.html) —— 迭代器计时数据所依托的 performance schema 事件机制。
