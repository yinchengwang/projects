# Task 6 执行报告：脚本超时第二档

## 结果

- `MAX_TIMEOUT_MS` 保持 `60000` 毫秒，用户提交代码路径未调整。
- 新增 `MAX_SCRIPT_TIMEOUT_MS = 600000` 毫秒；`clampTimeoutMs(ms, cap)` 和 `runCommand` 可使用独立上限，默认仍沿用用户代码上限。
- 卡片脚本预算读取 `unit.extra.timeout_s`（毫秒换算），缺失或非法时回落至 30 秒默认值，并通过脚本上限截断；用户代码执行仍使用既有默认上限。

## TDD RED / GREEN

- **RED**：新增 `script-timeout.test.js` 后首次运行，5 条断言中 2 条失败：脚本上限未导出（`undefined`）且自定义 `600000` 上限被原 `60000` 截断，确认测试能捕获缺失功能。
- **GREEN**：实现常量导出、可选 clamp 上限、runner 卡片预算读取和 `runCommand` 上限透传后，专项测试 5/5 通过。

## 验证

- `node tests/p2-sandbox.test.js`：17/17 通过。
- `node tests/p2-runner.test.js`：18/18 通过。
- `node tests/script-timeout.test.js`：5/5 通过。
- `node tests/run-all.js`：123/124 套件通过；唯一失败为已知环境差异 `remediation-python-reload.test.js`，本机 NumPy 为 `2.1.3`，测试预期 `2.5.5`。此次超时相关套件均通过。
