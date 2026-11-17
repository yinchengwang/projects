# B3 切片：保留结果集 SQL NULL 语义

## 目标

嵌入式 `engine/db` 的当前公开结果 API 以字符串返回所有 cell：`val_to_str()` 将 SQL NULL 写为 `"NULL"`，而用户真实 TEXT 值 `"NULL"` 得到同一字符串。调用者因此无法区分数据库 NULL 与文本字面量；这会丢失类型/NULL 信息。B3 首个垂直切片添加一个向后兼容的显式 NULL accessor：`db_result_value()` 保持既有返回行为，新 API 查询 NULL 位图。

## 设计决策

- 公开签名：`int db_result_is_null(const Result *r, int row, int col)`。
- 有效单元格返回 1/0；NULL Result、缺失 QueryResult、空结果、负索引、越界行列均返回 0，遵守当前 accessor 的“无害默认值”惯例。
- 不改变 `db_result_value()`：SQL NULL 仍返回字符串 `"NULL"`，旧调用者不回归；文本 `"NULL"` 也仍是该字符串，但新 accessor 对它返回 0。
- `QueryResult` 新增 row-major cell NULL 位图；在 `b_push()` 根据 `DbValue.is_null` 填标记，grow 必须扩容并初始化新增标记；`qr_free_inner()` 负责释放。
- `exec_write.c` 等非 Builder 手工构造的 QueryResult 路径必须初始化兼容位图或明确用 NULL 指针安全回退；逐一搜 `calloc(sizeof(QueryResult))`。

## 文件边界

- `engine/db/src/query/exec.h`：增加私有 `QueryResult` NULL 位图字段与注释。
- `engine/db/src/query/exec.c`：Builder init/grow/push 和析构维护位图；失败路径不得留下不可释放部分资源。
- `engine/db/include/db.h`、`engine/db/src/db_result.c`：添加公开 API 声明和实现。
- `engine/db/test/unit/test_query.c`：公开 API 回归测试（选该已有测试，不另造 runner）。
- 可能涉及 `engine/db/src/query/exec_write.c`：若其直接创建结果对象，则补充安全初始化。

## 验收测试设计

写入测试在实现前完成并确认 RED：

1. `SELECT name FROM t WHERE id = 3` 返回一格，`db_result_is_null(...,0,0)==1`，旧 `db_result_value` 仍为 `"NULL"`。
2. 同表另插一行文本 `'NULL'`，其 accessor 返回 0，而 `db_result_value` 仍为 `"NULL"`；证明新 API 可区分两种语义。
3. 混合多行/多列，覆盖 NULL 与普通值；跨 NULL bitmap 字节边界（至少 9 个 cell）。
4. 查询结果超过 Builder 初始 capacity（至少 8 行）后，确认 grow 不丢 NULL bits。
5. 无效句柄、空结果、负 row/col 与越界访问返回 0；列范围检查独立进行。
6. 查询结果构造/扩容失败路径按现有错误模型清理 cells 与位图，不泄漏；若没有确定性分配故障框架，不单为此改全项目 allocator。

## 风险与范围

- NULL 位图只解决“结果集 cell 的 NULL 可观察性”，不改变内部三值逻辑、存盘格式或 SQL 语义。
- 位图和 cells 必须共享同一个 row-major 索引：`cell = row * ncols + col`。
- `b_grow()` 扩容若一个数组成功而另一个失败，必须避免 QueryResult 中悬空指针或尺寸不一致；建议先分配临时的新 cell/null buffers、复制旧逻辑数据，全部成功后一次提交新指针和 cap。
- 本切片不迁移 `engine/src/db` 代码，不建立 server，不调整并发/分布式能力。

## 验证与交付

- 聚焦：`mingw32-make clean && mingw32-make all` 后编译/运行 `test_query`；新断言需先 RED、实现后 GREEN。
- 完整：`sh tools/check_deps.sh`、`sh test/run.sh`、`mingw32-make check`。
- 通过后更新 `projects/engine/db/README.md`/`projects/README.md` 的结果 API 主线说明；按项目规约精确提交并 push projects，再更新 code submodule gitlink。
