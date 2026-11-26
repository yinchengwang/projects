# Task 5 完成报告：`GET /api/units` 的 `runs_local` 过滤

## 实现内容

- `/api/units` 路由读取 `runs_local` 查询参数，并传入 `store.listUnits` 过滤条件。
- 使用 `runsLocal !== null` 判断参数是否存在；因此 `?runs_local=` 会以空字符串精确过滤未标注单元，不会因 truthiness 判断而被忽略。
- 冻结契约未变更；仅修改 `apps/study-hub/server/api-units.js` 与 `apps/study-hub/tests/api-units.test.js`。

## TDD 记录

- RED：新增真实 HTTP 用例，向 store 插入 `runs_local: ""` 和 `runs_local: "yes"` 两条记录后请求 `/api/units?runs_local=`。路由尚未传过滤条件时，预期总数 1、实际总数 2，测试按预期失败。
- GREEN：添加 presence/null 判断后，同一用例通过。

## 验证结果

- `node tests/api-units.test.js`：9/9 通过。
- `node tests/run-all.js`：122/123 套件通过；唯一失败是已知存量 `remediation-python-reload.test.js` NumPy 版本不匹配：环境为 `2.1.3`，测试期望 `2.5.5`。Task 5 的相关用例通过。

## 交付

按任务要求仅提交路由、测试与本报告；工作区原有未跟踪文件未纳入本次提交。
