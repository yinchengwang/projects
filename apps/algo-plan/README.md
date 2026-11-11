# algo-plan · 算法能力提升计划 App

本机运行的算法学习助手：本地静态题库打底，LLM 做增强，判题走真实编译执行，
进度与错题写回 learning 仓进 git。

当前进度：**M3 判题**（服务起得来 / SQLite schema / 题库 md 解析 / 真实编译执行的判题器 / 结果面板）。
题库导入属于 M2（已完成，`learning/bank/` 下有几百道题）；复习与轮次闭环属于 M4，
诊断属于 M5，LLM 属于 M6，尚未实现。

## 快速开始

需要 Node ≥ 24（用到内置 `node:sqlite`）。零 npm 依赖、零构建。

```bash
# 跑测试
node tests/run-all.js

# 接上 learning 仓并启动
ALGO_PLAN_LEARNING_ROOT=/path/to/learning node server/main.js
# 打开 http://127.0.0.1:5173
```

没设 `ALGO_PLAN_LEARNING_ROOT` 也能启动——那是「纯本地模式」：只有 SQLite，
不与 learning 仓同步。

## 环境变量

| 变量 | 说明 | 默认 |
|---|---|---|
| `ALGO_PLAN_LEARNING_ROOT` | learning 仓根目录。**不硬编码相对路径** | 无（未设＝纯本地模式） |
| `PORT` | 服务端口 | `5173` |

## 目录结构

```
algo-plan/
├── core/            纯函数层：Problem 模型、markdown 解析、provider、路径解析
│   ├── bank/        legacy.js：死脚本内联清单的静态解析
│   │                solutions-scan.js：存量题解的静态扫描
│   └── judge-spec/  判题契约纯函数：用例归一化、输出比对
├── server/          组合根、路由、API、SQLite 存储
├── judge/           判题执行器：沙箱（临时目录/探测/杀进程树）、编排器（verdict）
├── web/             原生 JS 单页（无构建）
├── tools/           seed-bank.js / import-catalog.js / import-solutions.js / import-bank.js
├── fixtures/        样板题 + 判题 fixture（题面、用例、正例与三类负例）
└── tests/           node:test 套件，一个命令跑全部
```

## 题库格式

题库放在 `learning/bank/<track>/<id>.md`，markdown + frontmatter，人和 agent 都能直接编辑：

````markdown
---
id: lc-00001
source: leetcode
title: 两数之和
difficulty: easy
topics: [哈希表, 数组]
tracks: [interview, fundamentals]
status: todo
needs_fill: false
created: 2026-11-11
updated: 2026-11-11
---
## 题面
...
## 测试用例
| input | expect |
|---|---|
| [2,7,11,15], 9 | [0,1] |
## 参考解法
```cpp
...
```
````

`id` 是 git 侧与 SQLite 侧的唯一连接键，格式 `<前缀>-<5 位数字>`，
前缀取自 `lc` / `nc` / `jz` / `self` / `llm`。

导入样板题：

```bash
ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/seed-bank.js --dry-run
ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/seed-bank.js
```

## 题库导入（M2）

题库有两路来源，都由 `tools/` 下的导入器负责。**导入器一律不碰 learning 仓的 git**：

| 工具 | 来源 | 产出 |
|---|---|---|
| `tools/import-catalog.js` | `projects/scripts/` 三个死脚本里的内联题目清单 | `learning/bank/<track>/<id>.md`（只有元数据） |
| `tools/import-solutions.js` | `learning/code-solutions/` 与 `learning/algo-c/` 里的存量题解 | 同上，多一段「## 参考解法」 |
| `tools/import-bank.js` | 上面两个，按「先目录、后题解」的顺序跑一遍 | 同上 |

```bash
# 先试运行，看要写哪些文件
ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/import-bank.js --dry-run

# 真导入
ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/import-bank.js

# 已有同名文件时强制覆盖（默认是跳过并计入 skipped）
ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/import-bank.js --force
```

导入的题**一律 `needs_fill: true`**：死脚本与存量题解里都只有元数据与代码，
没有题面和测试用例，补内容是后面的活。

导入器**绝不执行** `projects/scripts/` 里的死脚本——那三个脚本都指向已删除的
`practice-data.js`，而且里面有 6 处题号写成了未定义标识符（`offer22` / `offer18` /
`deleteNode` / `offer23` / `offer09` / `offer30`），跑起来必然 `ReferenceError`。
题目清单是**静态解析**出来的。

同 id 的冲突按 **id** 判断（不按路径）：同一个 id 在两路来源里算出来的轨道可能不同，
只看自己那条路径会漏判，会写出两份同 id 的文件。

## 判题契约

提交给判题器的是**完整的 stdin→stdout 程序，必须自己写 `main()`**。

- 判题器不注入 Solution 包装、不自动生成 `main()`、不猜方法名。
- C++ 走 `g++ -std=c++17 -O2`（不加 `-Werror`、不加 `-Wall`），Python 走
  `python3` → `python` → `py -3` 依次探测（第一个能 `--version` 的胜出）。
- 单用例默认超时 2000ms，超时杀整棵进程树（Windows `taskkill /T /F`，POSIX 进程组 kill）。
- verdict 只有五个：`pass` / `wrong_answer` / `timeout` / `compile_error` / `runtime_error`。
- 单次运行的 stdout/stderr 各封顶 **2MB**，超了截断——不封顶的话，一个死循环打印的
  程序会把输出撑到几百 MB，拼字符串时撞上 V8 的长度上限抛 `ERR_STRING_TOO_LONG`，
  那行代码在事件回调里，抛出去就是 `uncaughtException`，**整个服务进程直接死**。
- 杀进程树之后还有 **2 秒宽限期**：到点还没结束就强行按超时收摊，
  免得请求永远挂着、临时目录永远删不掉。

**判题不是安全沙箱**：它防跑飞、死循环、爆内存、编译垃圾，**不防恶意代码**。
用户代码以当前用户权限在系统临时目录里跑，每题一个临时目录，判完即删。

## 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/health` | 运行状态、learning 仓连接情况、题库概况 |
| GET | `/api/bank` | 题库列表，支持 `track` / `topic` / `difficulty` / `status` / `prefix` / `limit` |
| GET | `/api/problem/:id` | 单题详情。**只回前 2 个样例用例 + 用例总数，不回参考解法** |
| POST | `/api/attempt` | 提交代码判题。入参 `{problemId, code, language, mode}`；响应 `{attemptId, verdict, passedCases, totalCases, durationMs, failedCase, message, compilerOutput}` |

## 测试

```bash
node tests/run-all.js
```

| 套件 | 覆盖 |
|---|---|
| `smoke.test.js` | 静态页 200 / 404 / 越界 400 / 方法不对 405 |
| `router.test.js` | 路径参数匹配、方法匹配、JSON body 解析 |
| `problem.test.js` | ID 生成、topic 归一化、字段校验 |
| `parse.test.js` | frontmatter、用例表、代码块、往返等价 |
| `db.test.js` | 建表、配置、流水、复习卡片、知识点 |
| `provider.test.js` | 题库读写、过滤、坏文件容错 |
| `api.test.js` | 三个接口（健康 / 题库 / 单题）、纯本地模式降级、用例不外泄 |
| `e2e.test.js` | 导入 → 起服务 → 打接口 → 落盘 |
| `legacy.test.js` | 死脚本内联清单的静态解析：分类合并、题号提取、标题还原、标签归一化 |
| `solutions-scan.test.js` | 存量题解扫描：标记切分、文件名取号、专题识别、索引解析 |
| `import-catalog.test.js` | 目录导入器：落盘位置、needs_fill、坏题号跳过、冲突跳过、dry-run |
| `import-solutions.test.js` | 题解导入器：落盘位置、参考解法、索引题名难度、命名过滤、重复导入 |
| `m2-e2e.test.js` | 端到端：死脚本 / 题解 → 题库文件 → 读回来 |
| `judge-spec.test.js` | 用例归一化、输出比对、容差比较 |
| `sandbox.test.js` | 工具链探测、临时目录、喂 stdin、超时杀进程树 |
| `judge-fixtures.test.js` | fixture 自检：正例对、三类负例确实坏 |
| `runner.test.js` | 五类 verdict 端到端、判题契约反例、输出上限、宽限期兜底、临时目录清理 |
| `attempt.test.js` | `POST /api/attempt` 的形状、校验、流水落库 |
| `web-panel.test.js` | 结果面板数据整理（纯函数） |
| `web-dom.test.js` | 前端渲染路径（DOM 桩）：样例渲染、草稿不丢、渲染竞态 |

## 归属

- 服务端、判题器、前端、测试 → `projects/apps/algo-plan/`（本仓）
- 题目本体、轮次清单、错题订正 → `learning/bank/`、`learning/progress/`、`learning/notes/`（学习仓）
