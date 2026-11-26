# Task 3 完成报告：`units.runs_local`

## 实现内容

- `units` 新表结构新增 `runs_local TEXT NOT NULL DEFAULT ''`，并为旧库迁移追加同名列。
- `parseUnitRow` 返回 `runs_local` 字符串；`replaceUnits` 写入该值，缺省值为空字符串。
- `listUnits({ runs_local })` 可按值精确过滤；支持 `''`（未标注）、`yes`、`partial`、`no` 四种状态。
- 修复原测试文件末尾重复的悬空块。测试遵循真实公开 API：`openDb()` 返回 `DatabaseSync`，再由 `createStore(db)` 构造仓储；未修改 `openDb`。
- 旧库迁移测试直接创建缺少 `runs_local` 的 `units` 表，并验证 `openDb` 迁移补列且已有记录默认为 `''`。

## TDD 记录

由于此前部分补丁已存在，不能声称从空白开始的 RED。先修复重复悬空块使测试文件可解析，再用其余现存测试作为 RED 证据：

- RED：`node tests/runs-local-filter.test.js` 当时 4 项中 3 项按预期失败：新数据返回字段为 `undefined`、过滤没有生效、旧表未迁移加列；迁移幂等用例通过。
- GREEN：实现 schema、迁移、写入、解析及过滤后，定向测试 4/4 通过。
- 用 canonical `unit-db.test.js` 检查迁移时，发现通用迁移器也对某些旧数据库中的 `units` 缺表情形发出告警。保持其既有迁移规则并令此附加列在表缺失时静默跳过，避免旧库被此任务产生伪失败。

## 验证结果

- `node tests/runs-local-filter.test.js`：4/4 通过。
- `node tests/unit-db.test.js`：11/11 通过。
- `node tests/run-all.js`：122/123 套件通过。唯一失败为已知、无关的 `remediation-python-reload.test.js`：环境返回 NumPy `2.1.3`，测试期望 `2.5.5`。其余新增 runs-local 测试均通过。

## 变更范围

功能代码与新测试仅涉及 `apps/study-hub/server/db.js`、`apps/study-hub/tests/runs-local-filter.test.js`。本报告按任务要求放在 `.superpowers/sdd/ai-infra-p0/task-3-report.md`。没有触碰其他未跟踪文件。
