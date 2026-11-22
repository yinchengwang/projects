# PostgreSQL Wiki P2 A 组试点报告

**状态：** A 组正文、机械验收与抽样深度审查完成；尚未提交。
**源码基线：** `postgres@9b5d4e3331753b3399e84965093109cac8f364bf`（2026-11-21 shallow clone）  
**P2a/P2b 分支：** P2a 已合并 projects/main，合并提交 `058673f6d`；umbrella gitlink 已推到 `0ba1686`。  
**试点范围：** 新机制清单 A 组 20 页；总机制清单为 151 项（12 组），替代 P1 的按文件生成 332 页。

## 1. P2a 清单重建结果

| 领域 | 机制数 | 领域 | 机制数 |
|---|---:|---|---:|
| A 存储与 IPC | 20 | G 目录与缓存 | 7 |
| B 事务与 WAL | 19 | H 内存与排序 | 5 |
| C 堆表与元组 | 15 | I 复制与高可用 | 10 |
| D 索引族 | 17 | J 服务端与命令面 | 12 |
| E 优化器 | 13 | K SQL 处理与安全 | 11 |
| F 执行器 | 13 | L SQL 类型与内建函数 | 9 |
| **总计** | **151** | **领域** | **12** |

清单经人工 review，跨组重复机制有唯一主归属；A–D 71 项和 E–L 80 项全部引用至少一个源码函数定义。实现时用 Universal Ctags 6.1.0 的同 path `kind=function` 作为定义证据，再由 `wiki_refs.py` 对目标源码做词边界硬门禁。151 项共 604 个 `(path, symbol)` ref 对（A–D 262，E–L 342），定义/源码门禁均未命中失败。

## 2. A 组文件清单与分片

| Writer | 文件 | 主题范围 | 结果 |
|---|---|---|---|
| W1 | a01–a05 | buffer pool、本地 buffer、ProcArray snapshot、SSI、常规锁与死锁 | 5 页 |
| W2 | a06–a10 | VFD 文件 I/O、PGPROC、smgr/md、WaitEventSet、LWLock | 5 页 |
| W3 | a11–a15 | read stream、共享内存、DSM、shm_mq、AIO 后端 | 5 页 |
| W4 | a16–a20 | FSM、BufFile、ProcSignal、并行 DSM、Shared Invalidation 传输 | 5 页 |
| **合计** | **a01–a20** | **A-存储与 IPC** | **20 页** |

页面路径、slug、标题、refs 全部与 `_manifest.yml` 一致。Writer 分别报告了读取的源码文件与所用外链；实际总墙钟由各分片记录的起止时间存在近似值，故不伪造精确端到端分钟数。四分片大致在 21:52–22:05（Asia/Shanghai）完成。每页用词和源码落点由 reviewer/主代理再检查。

## 3. 机械门禁与规格测试

A 组全部页面及项目 `_index.md` 写入后，主流程 fresh-run 整目录门禁：

```text
python wiki_lint.py docs/wiki/projects/postgres --quiet
  -- 检查 21 页，失败 0 页

python wiki_refs.py docs/wiki/projects/postgres \
  --repo-root .cache/repos/postgres --quiet
  -- 检查 21 页，失败 0 页

python -m unittest test_postgres_mechanism_pages
  Ran 3 tests ... OK
```

工具回归 fresh-run：

```text
python -m unittest test_wiki_schema test_wiki_frontmatter test_wiki_lint \
  test_wiki_refs test_repo_cache test_wiki_schema_doc test_wiki_catalog \
  test_wiki_manifest test_wiki_manifest_lint test_wiki_tree test_build_catalog \
  test_catalog_additions test_gen_postgres_manifest test_postgres_mechanism_pages
  Ran 315 tests ... OK (skipped=1)
```

额外核对 A 组 `frontmatter.refs` 与 manifest 逐页深度相等，20/20 一致；UTF-8 无 BOM、LF；每页 7 个契约 H2（六必含+条件章节）；无 `path:line` 引用。项目 overview 页面按契约另有总览字段和 refs，两个目录门禁均通过。


抽样方法：`random.Random(20261122)` 从 20 页无放回抽 5 页；固定样本为 a19、a17、a08、a13、a02。由两个独立审阅视角检查，问题修复后重新逐项给分。

评分项（每项 0/1）：

1. **机制边界**：负责/不负责、上下游清楚。
2. **实现映射**：流程能落到指定的真实源码符号。
3. **机制深度**：核心状态/数据结构及生命周期或并发边界有说明。
4. **取舍**：至少两项，并能区分源码事实与架构推论。
5. **来源质量**：至少两个权威且与论点相关的外链，URL 可达。
6. **RAG 切块**：章节可独立理解，边界/事实不依赖未说明上下文。

| 文件 | 初审 | 发现 | 修订 |
|---|---:|---|---:|
| a19-parallel-query-dsm.md | 6/6 | DSM 退化路径措辞可能被理解为仍以本地状态并行；明确为 worker=0、不启动 worker、调用方走非并行路径。 | 6/6 |
| a17-temporary-buffiles.md | 5/6 | 两条参考外链均与 BufFile 不直接相关；更换为 temp_file_limit 配置文档与 BufFile 源码；明确普通临时文件 vs FileSet 受控共享生命周期。 | 6/6 |
| a08-smgr-md-storage.md | 6/6 | 无阻塞问题；实现与边界复核通过。 | 6/6 |
| a13-dynamic-shared-memory.md | 6/6 | 引用计数初值/活动区间原文过度确定；改成源码内部控制基准限定。 | 6/6 |
| a02-local-buffer-pool.md | 4/6 | 设计取舍扩成 3 项，引用改为 temp_buffers + localbuf 官方源码。 | 6/6 |
| **合计** | **27/30** | 5 处 Major/Minor 反馈全部处理 | **30/30** |

另有全组审核发现的非抽样问题并已修：

- a04-ssi-predicate-locks.md：原将 `CheckForSerializableConflictOut/In` 的读写侧解释写反；对照 `predicate.c` 注释后已纠正。
- a07-backend-proc-slots.md：将等待队列加入职责误落到 `ProcSleep`；改为睡眠/处理等待结果，注明 `JoinWaitQueue` 才加入队列。
- a18-process-signals-barriers.md：明确 ProcSignalBarrier 执行的是源码预定义类型动作，并不运行调用者任意回调，也不自动保证任意状态已同步。
- a20-shared-invalidation.md：将“所有接收者”限缩成参与回收计算的有效接收者，并说明 `sendOnly`/reset 槽处理。
- a01-buffer-pool.md：区分固定帧数与可替换页身份；把读 I/O 描述扩展为同步/批量路径；限定 WAL-before-data 只适用于永久关系；将 `StrategySyncStart` 限定为 bgwriter 的扫描起点/分配计数。
- a12-shared-memory-allocation.md：将旧式 `ShmemInitStruct` 容量预留表述与新 request/init API 区分开。
- a07/a01 的无效或错配 URL 已修正；`a07` 锁相关外链须指向 `explicit-locking.html` 而不是不存在的 `locking.html`。

## 5. 试点成本与扩容建议

实际 provider token usage 未暴露，**token/费用不可观测**，不做估算冒充实测。四 writer 分片记录的工作时长约为 W1 14 分钟、W2 7 分钟、W3 7 分钟（结束时间近似）、W4 10 分钟；四路并行的试点墙钟约 13 分钟（采样区间 21:52–22:05，个别报告开始时间存在近似值）。20 页/约 13 分钟仅是本轮粗估吞吐，不作为恒定产能承诺。

对剩余 131 页作线性工程估算：以 20 页/13 分钟推得约 85 分钟串行有效写作时间；4 并发约 22 分钟，8 并发理想约 11 分钟，16 并发理想约 6 分钟。估算未计跨主题学习、均衡分片、审阅和返工，实际 wall-clock 会更长。下一轮先保持 4 个不重叠分片，根据一次通过率、返工页数和 wall-clock 再决定是否加至 8；不建议直接开 16。

| 并行度 | 剩余 131 页 wall-clock 工程估算 | 假设 |
|---:|---:|---|
| 4 | 约 22 分钟 | 20 页/13 分钟，理想线性并行；未计审阅/返工 |
| 8 | 约 11 分钟 | 同上，不计并发退化 |
| 16 | 约 6 分钟 | 理想上界情景，不建议首轮采用 |

## 6. 未决与质量说明

- B–L 的 131 个正文页未写；但机制 manifest 已重建为 151 项。
- `_index.md` 的 `stars` / `stars_as_of` 沿用现有 catalog 占位值（0 / 1970-01-01）；不得当作真实仓库热度，后续可独立回填。
- 源码引用门禁按词边界验证，不证明语义；A 组人工抽检与 reviewer 审核用于补这一层，但不是形式化证明。
- 当前 `projects` 工作树存在与本任务无关的 `engine/db/src/query/{exec_write.c,lexer.c,lexer.h,parser.c,parser.h}` 和 `engine/db/test/unit/test_json_column.c` 修改；本试点不得暂存/覆盖这些文件。`scripts/wiki/test_postgres_mechanism_pages.py` 是本任务新增契约测试。
- `_index.md` license/language/stars 元数据来自现有 `catalog-projects.yml`（PostgreSQL License、主语言当前目录标 unknown、stars 占位），后续 catalog 回填时同步校正。
