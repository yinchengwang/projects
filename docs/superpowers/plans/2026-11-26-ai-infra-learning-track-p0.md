# AI Infra 学习轨 P0：工装与骨架 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 study-hub 装上「AI Infra 学习轨」的工装：让 `learning/units/ai/` 的单元能被读取、标注本机可跑性、支持长时实验脚本，并产出 5 个骨架单元（2 地图 + 2 环境 + 1 基线探针）。

**Architecture:** 内容进 `learning` 仓（独立 git 仓），代码改本仓 `apps/study-hub/`。契约 §1.2 的 17 个 Unit 字段**一个字不动**——4 个新元数据字段装进一个 `unit.extra` 嵌套袋，由 loader C 挂上、loader A/B 恒为 `{}`。超时用**第二档上限**解决：原 `MAX_TIMEOUT_MS=60000` 留给用户提交代码不动，脚本另给 `MAX_SCRIPT_TIMEOUT_MS=600000`。

**Tech Stack:** Node.js ≥ 24（`node:sqlite`、内置 test runner）、零 npm 依赖；learning 仓单元 = markdown + Python 断言脚本。

## Global Constraints

- **提交纪律**：每次 commit 后立即 `git push origin main`，不累积多个 commit。
- **语言纪律**：所有对话、解释、注释、commit message 用简体中文；代码标识符与 API 名保持英文。
- **代码风格**：本仓 `apps/study-hub/` 用 2 空格缩进、双引号、`"use strict"` 开头、JSDoc 注释（中文）。**不用** 根目录 `.clang-format`（那是 C/C++ 的）。
- **测试命令**：`cd apps/study-hub && node tests/run-all.js`（自动发现 `tests/*.test.js`，不需要登记）。
- **单套件命令**：`cd apps/study-hub && node tests/<name>.test.js`。
- **零依赖**：不新增任何 npm 包。
- **契约冻结**：`docs/superpowers/plans/2026-11-12-study-hub-接口契约.md` §1.2 的 17 个 Unit 字段**不许加字段、不许改语义**。
- **learning 仓纪律**：写入前查脏（`core/sync/rounds.js` 的 `assertCleanWorktree`），**绝不自动 commit**，写回只改目标行。
- **单元文件格式**：loader C 的 frontmatter 收尾 `---` 必需；`## 讲解` / `## 关联` 两个小节；`kind: card` 的单元**不写** `## 测试用例`。
- **本机事实**（实测 2026-11-26，勿凭记忆改）：Windows Python 3.12 装在系统级 `Python312`（torch 2.15.0+**cpu**，同目录有 sentence-transformers 5.1.2，**不许覆盖安装**）；WSL2 `Ubuntu-24.04` 已建、`nvidia-smi -L` 认到 RTX 4060、内存 7745 MB、`nvcc` 不存在；`C:\Users\yinch\.wslconfig` 不存在。

---

## File Structure

| 文件 | 归属 | 职责 |
|---|---|---|
| `apps/study-hub/core/unit/extra.js` | 新建 | `unit.extra` 的形状常量与构造/归一函数（纯函数，可单测） |
| `apps/study-hub/core/unit/loader-c.js` | 改 | 把 4 个附加字段装进 `unit.extra` |
| `apps/study-hub/core/unit/index.js` | 改 | `refreshUnitIndex` 把 `runs_local` 写进 units 表新列 |
| `apps/study-hub/core/unit/unit.js` | 改 | `createUnit` 接受并挂 `extra`（不动 17 个冻结字段） |
| `apps/study-hub/server/db.js` | 改 | `units` 表加 `runs_local` 列 + 迁移 + `replaceUnits`/`listUnits`/`parseUnitRow` |
| `apps/study-hub/server/api-units.js` | 改 | `GET /api/units` 支持 `runs_local` 过滤 |
| `apps/study-hub/judge/sandbox.js` | 改 | `clampTimeoutMs(ms, cap)` 加第二档 + `MAX_SCRIPT_TIMEOUT_MS` |
| `apps/study-hub/judge/runner.js` | 改 | 卡片脚本预算读 `unit.extra.timeout_s` |
| `apps/study-hub/core/supply/unit-file.js` | 改 | `FRONTMATTER_ORDER` 加 4 个附加字段（写回时保留） |
| `apps/study-hub/tests/extra.test.js` | 新建 | `unit.extra` 的单元测试 |
| `apps/study-hub/tests/runs-local-filter.test.js` | 新建 | `runs_local` 列与过滤的测试 |
| `apps/study-hub/tests/script-timeout.test.js` | 新建 | 超时第二档的测试 |
| `learning/units/ai/*.md` + `*.demo.py` + `*.assert.py` | 新建（learning 仓） | 5 个骨架单元 |
| `learning/units/ai/_meta/acceptance.md` | 新建（learning 仓） | 验收记录模板 |

**为什么 `unit.extra` 挂 `createUnit` 而不是 loader C 单独挂**：契约 §1.2 说 loader A/B/C 共用同一个 Unit 抽象。若只有 loader C 有 `extra`，下游读 `unit.extra.runs_local` 时必须先判 `unit.extra` 存在与否，形状会漂。挂进 `createUnit` 后**所有** Unit 都有 `extra`（loader A/B 是 `{}`），形状统一，代价是 `createUnit` 要多处理一个键。

---

## Task 1：`unit.extra` 的形状与构造

**Files:**
- Create: `apps/study-hub/core/unit/extra.js`
- Modify: `apps/study-hub/core/unit/unit.js:184-217`（`createUnit`）
- Test: `apps/study-hub/tests/extra.test.js`

**Interfaces:**
- Consumes: 无（首个任务）
- Produces:
  - `core/unit/extra.js` 导出 `EXTRA_KEYS`（`["runs_local", "needs", "fallback", "timeout_s"]`）、`RUNS_LOCAL_VALUES`（`["yes", "partial", "no"]`）、`NEEDS_VALUES`（`["gpu-count", "vram", "nvcc", "fp8-train", "ram"]`）、`DEFAULT_TIMEOUT_S`（`30`）、`MAX_TIMEOUT_S`（`600`）、`makeExtra(input)`、`normalizeExtra(unit)`。
  - `createUnit(input)` 新增接受 `input.extra`（对象），产出 `unit.extra` 恒存在、四个键恒在。

- [ ] **Step 1: 写失败测试**

新建 `apps/study-hub/tests/extra.test.js`：

```javascript
"use strict"
/**
 * extra.test.js —— unit.extra 的形状与归一
 *
 * unit.extra 是 4 个附加元数据字段的嵌套袋（契约 §1.2 的 17 个冻结字段一个字不动）。
 * 四个键恒存在，形状不随来源漂移：loader A/B 是全缺省，loader C 是 frontmatter 里的值。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const extra = require("../core/unit/extra.js")
const { createUnit } = require("../core/unit/unit.js")

test("EXTRA_KEYS 是约定的四个键，顺序固定", function () {
  assert.deepStrictEqual(extra.EXTRA_KEYS, ["runs_local", "needs", "fallback", "timeout_s"])
})

test("RUNS_LOCAL_VALUES 是 yes / partial / no", function () {
  assert.deepStrictEqual(extra.RUNS_LOCAL_VALUES, ["yes", "partial", "no"])
})

test("makeExtra：不给输入时四个键都在，值全是缺省", function () {
  const e = extra.makeExtra()
  assert.strictEqual(e.runs_local, "no")
  assert.deepStrictEqual(e.needs, [])
  assert.strictEqual(e.fallback, "")
  assert.strictEqual(e.timeout_s, extra.DEFAULT_TIMEOUT_S)
})

test("makeExtra：runs_local 非法值落回 no（宁可说跑不了，也不误报能跑）", function () {
  assert.strictEqual(extra.makeExtra({ runs_local: "yes" }).runs_local, "yes")
  assert.strictEqual(extra.makeExtra({ runs_local: "PARTIAL" }).runs_local, "no")
  assert.strictEqual(extra.makeExtra({ runs_local: "也许" }).runs_local, "no")
  assert.strictEqual(extra.makeExtra({ runs_local: "" }).runs_local, "no")
})

test("makeExtra：needs 只留枚举内的值", function () {
  const e = extra.makeExtra({ needs: ["vram", "nvcc", "外星科技"] })
  assert.deepStrictEqual(e.needs, ["vram", "nvcc"])
})

test("makeExtra：timeout_s 夹到 [1, 600]", function () {
  assert.strictEqual(extra.makeExtra({ timeout_s: 300 }).timeout_s, 300)
  assert.strictEqual(extra.makeExtra({ timeout_s: 99999 }).timeout_s, extra.MAX_TIMEOUT_S)
  assert.strictEqual(extra.makeExtra({ timeout_s: 0 }).timeout_s, extra.DEFAULT_TIMEOUT_S)
  assert.strictEqual(extra.makeExtra({ timeout_s: -5 }).timeout_s, extra.DEFAULT_TIMEOUT_S)
  assert.strictEqual(extra.makeExtra({ timeout_s: "abc" }).timeout_s, extra.DEFAULT_TIMEOUT_S)
})

test("createUnit：产出的单元恒有 extra（loader A/B 走这条路）", function () {
  const unit = createUnit({
    id: "algo:lc-00001",
    track: "algorithm",
    kind: "problem",
    title: "两数之和",
    entry: { graded: "user", script: "", language: "cpp" },
    asserts: [],
    topics: [],
    subtracks: [],
    path: "bank/interview/lc-00001.md",
  })
  assert.ok(unit.extra, "extra 必须在场")
  assert.deepStrictEqual(Object.keys(unit.extra).sort(), extra.EXTRA_KEYS.slice().sort())
  assert.strictEqual(unit.extra.runs_local, "no")
})

test("createUnit：传入 extra 时原样带上", function () {
  const unit = createUnit({
    id: "ai:probe",
    track: "ai",
    kind: "card",
    title: "探针",
    entry: { graded: "script", script: "units/ai/probe.assert.py", language: "python" },
    asserts: [],
    topics: [],
    subtracks: [],
    path: "units/ai/probe.md",
    extra: { runs_local: "yes", needs: ["vram"], fallback: "降级读文档", timeout_s: 120 },
  })
  assert.strictEqual(unit.extra.runs_local, "yes")
  assert.deepStrictEqual(unit.extra.needs, ["vram"])
  assert.strictEqual(unit.extra.fallback, "降级读文档")
  assert.strictEqual(unit.extra.timeout_s, 120)
})

test("createUnit：extra 里的非法值被归一，不让脏值进 Unit", function () {
  const unit = createUnit({
    id: "ai:probe2",
    track: "ai",
    kind: "card",
    title: "探针二",
    entry: { graded: "script", script: "units/ai/probe2.assert.py", language: "python" },
    asserts: [],
    topics: [],
    subtracks: [],
    path: "units/ai/probe2.md",
    extra: { runs_local: "胡说", needs: ["vram", "外星"], timeout_s: 999999 },
  })
  assert.strictEqual(unit.extra.runs_local, "no")
  assert.deepStrictEqual(unit.extra.needs, ["vram"])
  assert.strictEqual(unit.extra.timeout_s, extra.MAX_TIMEOUT_S)
})

test("normalizeExtra：对象不是 extra 就当全缺省", function () {
  const e = extra.normalizeExtra(undefined)
  assert.strictEqual(e.runs_local, "no")
  assert.deepStrictEqual(e.needs, [])
  assert.strictEqual(e.fallback, "")
  assert.strictEqual(e.timeout_s, extra.DEFAULT_TIMEOUT_S)
})
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd apps/study-hub && node tests/extra.test.js`
Expected: FAIL，错误是 `Cannot find module '../core/unit/extra.js'`

- [ ] **Step 3: 写 `core/unit/extra.js`**

新建 `apps/study-hub/core/unit/extra.js`：

```javascript
"use strict"
/**
 * extra.js —— unit.extra：附加元数据的形状与归一
 *
 * 为什么要有这个嵌套袋：契约 §1.2 的 17 个 Unit 字段是**冻结**的
 * （三个 loader 共用，createUnit 校验，多处测试钉住形状），加顶层字段要动
 * createUnit、字段表与所有断言 Unit 形状的测试。
 *
 * 契约 §3.3.1 已给过先例：算法轨的 negative: / source_id: / url: 「都不进 Unit」，
 * 由消费方自己再读一遍 frontmatter。这里换个更省的做法——loader C 读一次 frontmatter
 * 就把 4 个字段装进 unit.extra，下游不必重读文件。
 *
 * 四个键**恒存在**（loader A/B 是全缺省），形状不随来源漂移。
 */

/** extra 的四个键，顺序固定 */
const EXTRA_KEYS = ["runs_local", "needs", "fallback", "timeout_s"]

/** runs_local 的取值域：yes = 本机真跑得起来；partial = 只能玩具化；no = 跑不了 */
const RUNS_LOCAL_VALUES = ["yes", "partial", "no"]

/** needs 的取值域：缺什么 */
const NEEDS_VALUES = ["gpu-count", "vram", "nvcc", "fp8-train", "ram"]

/** 断言脚本默认预算（秒）——与 judge/runner.js 的 SCRIPT_TIMEOUT_MS 对齐 */
const DEFAULT_TIMEOUT_S = 30

/** 断言脚本预算上限（秒）——与 judge/sandbox.js 的 MAX_SCRIPT_TIMEOUT_MS 对齐 */
const MAX_TIMEOUT_S = 600

/**
 * 把一组原始值归一成 extra。
 * 非法值一律落缺省，**不抛错**：一个单元文件的脏字段不该让整个索引加载失败。
 * @param {object} input
 * @returns {{runs_local: string, needs: string[], fallback: string, timeout_s: number}}
 */
function makeExtra(input) {
  const src = input && typeof input === "object" ? input : {}

  // runs_local：非法值落 "no"。宁可把能跑的误标成跑不了（用户会自己发现），
  // 也不能把跑不了的误标成能跑——后者会让人卡在一个永远跑不通的实验上。
  const rawRuns = String(src.runs_local === undefined || src.runs_local === null ? "" : src.runs_local).trim()
  const runs_local = RUNS_LOCAL_VALUES.indexOf(rawRuns) !== -1 ? rawRuns : "no"

  const rawNeeds = Array.isArray(src.needs) ? src.needs : []
  const needs = []
  rawNeeds.forEach(function (item) {
    const text = String(item === undefined || item === null ? "" : item).trim()
    if (text === "") return
    if (NEEDS_VALUES.indexOf(text) === -1) return
    if (needs.indexOf(text) !== -1) return
    needs.push(text)
  })

  const fallback = String(src.fallback === undefined || src.fallback === null ? "" : src.fallback).trim()

  const rawTimeout = Number(src.timeout_s)
  let timeout_s = DEFAULT_TIMEOUT_S
  if (Number.isFinite(rawTimeout) && rawTimeout > 0) {
    timeout_s = Math.min(Math.floor(rawTimeout), MAX_TIMEOUT_S)
  }

  return {
    runs_local: runs_local,
    needs: needs,
    fallback: fallback,
    timeout_s: timeout_s,
  }
}

/**
 * 取一个单元的 extra；没有就造一份全缺省的。
 * @param {object} unit
 * @returns {{runs_local: string, needs: string[], fallback: string, timeout_s: number}}
 */
function normalizeExtra(unit) {
  const src = unit && typeof unit === "object" ? unit : {}
  return makeExtra(src.extra)
}

module.exports = {
  EXTRA_KEYS: EXTRA_KEYS,
  RUNS_LOCAL_VALUES: RUNS_LOCAL_VALUES,
  NEEDS_VALUES: NEEDS_VALUES,
  DEFAULT_TIMEOUT_S: DEFAULT_TIMEOUT_S,
  MAX_TIMEOUT_S: MAX_TIMEOUT_S,
  makeExtra: makeExtra,
  normalizeExtra: normalizeExtra,
}
```

- [ ] **Step 4: 改 `createUnit` 挂上 extra**

改 `apps/study-hub/core/unit/unit.js`。在文件顶部的 require 区（第 11 行 `const { TRACKS, trackOf } = require("../track/tracks.js")` 之后）加：

```javascript
const { normalizeExtra } = require("./extra.js")
```

在 `createUnit` 里，`created` 对象的最后一个字段 `path: ...` 之后加一行：

```javascript
    path: String(src.path === undefined || src.path === null ? "" : src.path),
    // extra 是附加元数据的嵌套袋（4 个键恒在）。契约 §1.2 的 17 个字段一个字没动，
    // 新东西全在这个袋子里，形状对三个 loader 一致。
    extra: normalizeExtra(src),
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cd apps/study-hub && node tests/extra.test.js`
Expected: PASS，`ℹ tests 9 / ℹ pass 9 / ℹ fail 0`

- [ ] **Step 6: 跑全量测试确认没打破既有套件**

Run: `cd apps/study-hub && node tests/run-all.js`
Expected: 全部套件通过（`121 / 121 套件通过`）

**已核对的既有断言（不必改）**：
- `tests/unit-model.test.js:258` 的 `toUnitSummary` 断言的是 **8 个字段的精确集合**
  （difficulty / id / kind / status / subtracks / title / topics / track），
  `toUnitSummary` 不读 `extra`，所以加 `extra` 不影响它。
- `tests/unit-db.test.js:130` 的 `NEW_COLUMNS` 是**正向断言**（「至少要有这些列」），
  加列不破坏它。

若仍红，读失败信息定位——多半是某个套件断言了 `createUnit` 产物的**完整字段集合**
（`Object.keys(unit)`），按新形状补上 `extra` 即可。

- [ ] **Step 7: 提交**

```bash
git add apps/study-hub/core/unit/extra.js apps/study-hub/core/unit/unit.js apps/study-hub/tests/extra.test.js
git commit -m "feat(unit): 加 unit.extra 嵌套袋，装 4 个附加元数据字段"
git push origin main
```

---

## Task 2：loader C 把 frontmatter 的 4 个字段装进 extra

**Files:**
- Modify: `apps/study-hub/core/unit/loader-c.js:106-251`（`parseUnitFile`）
- Test: `apps/study-hub/tests/unit-loader-c.test.js`（追加用例）

**Interfaces:**
- Consumes: Task 1 的 `core/unit/extra.js`（`makeExtra`）。
- Produces: `parseUnitFile` 产出的 `unit.extra` 反映 frontmatter 的 `runs_local` / `needs` / `fallback` / `timeout_s`；frontmatter 没有这些键时落缺省。

- [ ] **Step 1: 写失败测试**

在 `apps/study-hub/tests/unit-loader-c.test.js` 末尾追加：

```javascript
test("loader C：附加字段装进 unit.extra", function () {
  const front = [
    "id: ai:kv-cache-formula",
    "track: ai",
    "kind: card",
    "title: KV Cache 公式",
    "difficulty: medium",
    "topics: [infra:显存]",
    "source: generated",
    "language: python",
    "demo: units/ai/kv-cache-formula.demo.py",
    "graded: script",
    "script: units/ai/kv-cache-formula.assert.py",
    "status: draft",
    "reviewed: false",
    "reviewed_at:",
    "runs_local: yes",
    "needs: [vram, nvcc]",
    "fallback: 读文档代替",
    "timeout_s: 120",
  ].join("\n")
  const res = loaderC.parseUnitFile(makeUnitFile(front, "## 讲解\n\n正文\n"), {
    path: "ai/kv-cache-formula.md",
  })
  assert.ok(res.unit, JSON.stringify(res.warnings))
  assert.strictEqual(res.unit.extra.runs_local, "yes")
  assert.deepStrictEqual(res.unit.extra.needs, ["vram", "nvcc"])
  assert.strictEqual(res.unit.extra.fallback, "读文档代替")
  assert.strictEqual(res.unit.extra.timeout_s, 120)
})

test("loader C：没有附加字段时 extra 落缺省（runs_local = no）", function () {
  const res = loaderC.parseUnitFile(makeUnitFile(GOOD_FRONT, GOOD_BODY), {
    path: "database/mvcc.md",
  })
  assert.ok(res.unit)
  assert.strictEqual(res.unit.extra.runs_local, "no")
  assert.deepStrictEqual(res.unit.extra.needs, [])
  assert.strictEqual(res.unit.extra.fallback, "")
  assert.strictEqual(res.unit.extra.timeout_s, 30)
})

test("loader C：附加字段的脏值被归一，不让单元整个报废", function () {
  const front = GOOD_FRONT + "\nruns_local: 胡说\nneeds: [vram, 外星]\ntimeout_s: abc"
  const res = loaderC.parseUnitFile(makeUnitFile(front, GOOD_BODY), {
    path: "database/mvcc.md",
  })
  assert.ok(res.unit, JSON.stringify(res.warnings))
  assert.strictEqual(res.unit.extra.runs_local, "no")
  assert.deepStrictEqual(res.unit.extra.needs, ["vram"])
  assert.strictEqual(res.unit.extra.timeout_s, 30)
})
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd apps/study-hub && node tests/unit-loader-c.test.js`
Expected: FAIL，错误形如 `Expected values to be strictly equal: 'no' !== 'yes'`（`unit.extra` 还没有内容）

- [ ] **Step 3: 改 loader C**

改 `apps/study-hub/core/unit/loader-c.js`。在 require 区（第 23 行 `} = require("./unit")` 之后）加：

```javascript
// extra 的归一在 core/unit/extra.js 里（唯一一份）。
const { makeExtra } = require("./extra.js")
```

在 `parseUnitFile` 里，`createUnit({...})` 的实参末尾（`related: related,` 之后）加：

```javascript
      // 4 个附加元数据字段：frontmatter 有就用，没有就落缺省。
      // 脏值由 makeExtra 归一（不抛错）——一个单元文件的脏字段不该让整个索引加载失败。
      extra: makeExtra({
        runs_local: fm.runs_local,
        needs: fm.needs,
        fallback: fm.fallback,
        timeout_s: fm.timeout_s,
      }),
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd apps/study-hub && node tests/unit-loader-c.test.js`
Expected: PASS，新增 3 条全过

- [ ] **Step 5: 跑全量测试**

Run: `cd apps/study-hub && node tests/run-all.js`
Expected: 全部通过

- [ ] **Step 6: 提交**

```bash
git add apps/study-hub/core/unit/loader-c.js apps/study-hub/tests/unit-loader-c.test.js
git commit -m "feat(unit): loader C 把 4 个附加字段装进 unit.extra"
git push origin main
```

---

## Task 3：`units` 表加 `runs_local` 列

**Files:**
- Modify: `apps/study-hub/server/db.js:102-113`（SCHEMA）、`apps/study-hub/server/db.js:24-36`（`parseUnitRow`）、`apps/study-hub/server/db.js:740-791`（`replaceUnits` / `listUnits`）
- Test: `apps/study-hub/tests/runs-local-filter.test.js`（新建）

**Interfaces:**
- Consumes: Task 1 的 `unit.extra.runs_local`。
- Produces:
  - `units` 表新增列 `runs_local TEXT NOT NULL DEFAULT ''`。
  - `store.replaceUnits(rows)` 接受 row 里的 `runs_local`。
  - `store.listUnits({runs_local})` 支持按该列过滤。
  - `parseUnitRow(row)` 返回的摘要带 `runs_local` 字符串。

- [ ] **Step 1: 写失败测试**

新建 `apps/study-hub/tests/runs-local-filter.test.js`：

```javascript
"use strict"
/**
 * runs-local-filter.test.js —— units 表的 runs_local 列与过滤
 *
 * runs_local 标的是「这个单元本机跑不跑得起来」。它必须能被筛出来，
 * 否则界面上看不见等于没标。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const { openDb } = require("../server/db.js")
const { tmpDir } = require("./helpers/tmp.js")

function freshStore() {
  const dir = tmpDir("study-hub-runs-local-")
  const db = openDb(path.join(dir, "app.db"))
  return db.store
}

const path = require("node:path")

test("units 表有 runs_local 列", function () {
  const store = freshStore()
  store.replaceUnits([
    {
      id: "ai:probe",
      track: "ai",
      kind: "card",
      title: "探针",
      path: "units/ai/probe.md",
      status: "draft",
      topics: JSON.stringify(["infra:显存"]),
      difficulty: "medium",
      subtracks: JSON.stringify([]),
      runs_local: "yes",
      updated: "2026-11-26T00:00:00.000Z",
    },
  ])
  const rows = store.listUnits({})
  assert.strictEqual(rows.length, 1)
  assert.strictEqual(rows[0].runs_local, "yes")
})

test("listUnits 能按 runs_local 过滤", function () {
  const store = freshStore()
  store.replaceUnits([
    { id: "ai:a", track: "ai", kind: "card", title: "A", path: "p", status: "draft", topics: "[]", difficulty: "medium", subtracks: "[]", runs_local: "yes", updated: "" },
    { id: "ai:b", track: "ai", kind: "card", title: "B", path: "p", status: "draft", topics: "[]", difficulty: "medium", subtracks: "[]", runs_local: "no", updated: "" },
    { id: "ai:c", track: "ai", kind: "card", title: "C", path: "p", status: "draft", topics: "[]", difficulty: "medium", subtracks: "[]", runs_local: "partial", updated: "" },
  ])
  assert.strictEqual(store.listUnits({ runs_local: "yes" }).length, 1)
  assert.strictEqual(store.listUnits({ runs_local: "no" }).length, 1)
  assert.strictEqual(store.listUnits({ runs_local: "partial" }).length, 1)
  assert.strictEqual(store.listUnits({}).length, 3)
})

test("老库迁移后 runs_local 列存在且缺省为空串", function () {
  const dir = tmpDir("study-hub-runs-local-migrate-")
  const file = path.join(dir, "app.db")
  const first = openDb(file)
  first.store.replaceUnits([
    { id: "ai:x", track: "ai", kind: "card", title: "X", path: "p", status: "draft", topics: "[]", difficulty: "medium", subtracks: "[]", updated: "" },
  ])
  first.close()
  const second = openDb(file)
  const rows = second.store.listUnits({})
  assert.strictEqual(rows.length, 1)
  assert.strictEqual(rows[0].runs_local, "")
  second.close()
})

test("迁移是幂等的：第二次 openDb 不再加列、不报警告", function () {
  const dir = tmpDir("study-hub-runs-local-idem-")
  const file = path.join(dir, "app.db")
  const first = openDb(file)
  first.close()
  const second = openDb(file)
  const again = migrate(second)
  assert.deepStrictEqual(again.applied, [], "第二次不该再加列")
  assert.deepStrictEqual(again.warnings, [], "第二次不该有警告")
  second.close()
})
```

**顶部 import**（若还没有）：

```javascript
const path = require("node:path")
const { openDb, migrate } = require("../server/db.js")
const { tmpDir } = require("./helpers/tmp.js")
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd apps/study-hub && node tests/runs-local-filter.test.js`
Expected: FAIL，错误是 `rows[0].runs_local` 为 `undefined`（列还不存在）

- [ ] **Step 3: SCHEMA 加列**

改 `apps/study-hub/server/db.js` 第 102-113 行的 `units` 建表语句，在 `subtracks` 之后加一行：

```javascript
   `CREATE TABLE IF NOT EXISTS units (
      id         TEXT PRIMARY KEY,
      track      TEXT NOT NULL,
      kind       TEXT NOT NULL,
      title      TEXT NOT NULL DEFAULT '',
      path       TEXT NOT NULL DEFAULT '',
      status     TEXT NOT NULL DEFAULT 'todo',
      topics     TEXT NOT NULL DEFAULT '',
      difficulty TEXT NOT NULL DEFAULT 'medium',
      subtracks  TEXT NOT NULL DEFAULT '',
      runs_local TEXT NOT NULL DEFAULT '',
      updated    TEXT NOT NULL DEFAULT ''
    )`,
```

**注意**：`CREATE TABLE IF NOT EXISTS` 对**已存在**的表不生效，所以老库必须靠 Task 3 Step 4 的迁移补列。

- [ ] **Step 4: 迁移补列**

改 `apps/study-hub/server/db.js` 的 `ADD_COLUMNS` 常量（第 156-164 行），在数组末尾加一项：

```javascript
  { table: "units", column: "runs_local", type: "TEXT NOT NULL DEFAULT ''" },
```

`migrate(db)` 已经是幂等的（列在就跳过），且 `openDb` 会调它。

- [ ] **Step 5: `parseUnitRow` 带上该列**

改 `apps/study-hub/server/db.js` 的 `parseUnitRow`（第 24-36 行），在 `subtracks: parseJsonList(row.subtracks),` 之后加：

```javascript
    runs_local: String(row.runs_local || ""),
```

- [ ] **Step 6: `replaceUnits` 写入该列**

改 `apps/study-hub/server/db.js` 的 `replaceUnits`（第 740-759 行）。SQL 串与 `stmt.run` 的参数同步加一项：

```javascript
    replaceUnits: function (rows) {
      db.exec("DELETE FROM units")
      const stmt = db.prepare(
        "INSERT INTO units(id, track, kind, title, path, status, topics, difficulty, subtracks, runs_local, updated) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
      )
      ;(rows || []).forEach(function (row) {
        stmt.run(
          String(row.id),
          String(row.track),
          String(row.kind),
          String(row.title || ""),
          String(row.path || ""),
          String(row.status || "todo"),
          String(row.topics || ""),
          String(row.difficulty || "medium"),
          String(row.subtracks || ""),
          String(row.runs_local || ""),
          String(row.updated || "")
        )
      })
    },
```

同时更新该函数上方的 JSDoc `@param` 行，在 `subtracks: string,` 之后插入 `runs_local: string,`。

- [ ] **Step 7: `listUnits` 支持过滤**

改 `apps/study-hub/server/db.js` 的 `listUnits`（第 767-791 行）。在 `if (f.kind) {...}` 块之后加：

```javascript
      if (f.runs_local) {
        where.push("runs_local = ?")
        args.push(String(f.runs_local))
      }
```

并把该函数 JSDoc 的 `@param` 改成：

```javascript
     * @param {{track?: string, status?: string, kind?: string, runs_local?: string}} [filter]
```

- [ ] **Step 8: 跑测试确认通过**

Run: `cd apps/study-hub && node tests/runs-local-filter.test.js`
Expected: PASS，3 条全过

- [ ] **Step 9: 跑全量测试**

Run: `cd apps/study-hub && node tests/run-all.js`
Expected: 全部通过。若 `unit-db.test.js` 红，多半是它断言了 `listUnits` 返回的精确形状，需补 `runs_local` 字段。

- [ ] **Step 10: 提交**

```bash
git add apps/study-hub/server/db.js apps/study-hub/tests/runs-local-filter.test.js
git commit -m "feat(db): units 表加 runs_local 列与过滤"
git push origin main
```

---

## Task 4：`refreshUnitIndex` 把 `runs_local` 写进表

**Files:**
- Modify: `apps/study-hub/core/unit/index.js:237-262`（`refreshUnitIndex`）
- Test: `apps/study-hub/tests/unit-index.test.js`（追加用例）

**Interfaces:**
- Consumes: Task 1 的 `unit.extra.runs_local`、Task 3 的 `store.replaceUnits`。
- Produces: `refreshUnitIndex` 写入的 row 带 `runs_local`；`unit.extra` 缺失时写空串。

- [ ] **Step 1: 写失败测试**

在 `apps/study-hub/tests/unit-index.test.js` 末尾追加：

```javascript
test("refreshUnitIndex 把 runs_local 写进 units 表", function () {
  const dir = tmpDir("study-hub-index-runs-local-")
  const db = openDb(path.join(dir, "app.db"))
  const res = refreshUnitIndex(
    db.store,
    {},
    {
      units: [
        {
          id: "ai:probe",
          track: "ai",
          kind: "card",
          title: "探针",
          path: "units/ai/probe.md",
          status: "draft",
          topics: ["infra:显存"],
          difficulty: "medium",
          subtracks: [],
          extra: { runs_local: "yes", needs: [], fallback: "", timeout_s: 30 },
        },
        {
          id: "ai:probe2",
          track: "ai",
          kind: "card",
          title: "探针二",
          path: "units/ai/probe2.md",
          status: "draft",
          topics: [],
          difficulty: "medium",
          subtracks: [],
        },
      ],
      warnings: [],
      byId: {},
    }
  )
  assert.strictEqual(res.count, 2)
  const rows = db.store.listUnits({})
  assert.strictEqual(rows.length, 2)
  assert.strictEqual(rows[0].runs_local, "yes")
  assert.strictEqual(rows[1].runs_local, "")
  db.close()
})
```

若该文件顶部还没 import `tmpDir` / `openDb` / `path`，补上：

```javascript
const path = require("node:path")
const { openDb } = require("../server/db.js")
const { tmpDir } = require("./helpers/tmp.js")
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd apps/study-hub && node tests/unit-index.test.js`
Expected: FAIL，`rows[0].runs_local` 是 `undefined` 或 `""`（还没写进去）

- [ ] **Step 3: 改 `refreshUnitIndex`**

改 `apps/study-hub/core/unit/index.js` 的 `refreshUnitIndex`（第 244-258 行）。在 `const rows = res.units.map(function (unit) {` 的映射里，在 `subtracks: JSON.stringify(unit.subtracks || []),` 之后加：

```javascript
      // runs_local 来自 unit.extra（Task 1 的嵌套袋）。
      // extra 缺失时写空串——loader A/B 的单元没有这层信息，空串比 "no" 诚实：
      // "no" 是一个断言（跑不了），空串是「没标」。
      runs_local: unit.extra ? String(unit.extra.runs_local || "") : "",
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd apps/study-hub && node tests/unit-index.test.js`
Expected: PASS

- [ ] **Step 5: 跑全量测试**

Run: `cd apps/study-hub && node tests/run-all.js`
Expected: 全部通过

- [ ] **Step 6: 提交**

```bash
git add apps/study-hub/core/unit/index.js apps/study-hub/tests/unit-index.test.js
git commit -m "feat(unit): refreshUnitIndex 写入 runs_local"
git push origin main
```

---

## Task 5：`GET /api/units` 支持 `runs_local` 过滤

**Files:**
- Modify: `apps/study-hub/server/api-units.js:76-110`（列表路由）
- Test: `apps/study-hub/tests/api-units.test.js`（追加用例）

**Interfaces:**
- Consumes: Task 3 的 `store.listUnits({runs_local})`。
- Produces: `GET /api/units?runs_local=yes` 只返回该值的单元。

- [ ] **Step 1: 写失败测试**

在 `apps/study-hub/tests/api-units.test.js` 末尾追加：

```javascript
test("GET /api/units 支持 runs_local 过滤", async function () {
  const learning = tempLearning()
  const app = await createApp({ learningRoot: learning })
  const res = await fetch(app.baseUrl + "/api/units?runs_local=yes")
  assert.strictEqual(res.status, 200)
  const body = await res.json()
  assert.ok(body.total >= 0)
  body.items.forEach(function (item) {
    assert.strictEqual(item.runs_local, "yes")
  })
  await app.close()
})
```

**注意**：该测试里 `tempLearning()` 与 `createApp` 的用法要照抄本文件里既有的测试（先读一遍本文件的现有用例，别猜 API 形状）。

- [ ] **Step 2: 跑测试确认失败**

Run: `cd apps/study-hub && node tests/api-units.test.js`
Expected: FAIL，`runs_local` 过滤不生效（返回全部）

- [ ] **Step 3: 改路由**

改 `apps/study-hub/server/api-units.js` 的列表路由。在 `const status = query.get("status")` 块之后加：

```javascript
      const runsLocal = query.get("runs_local")
      if (runsLocal) filter.runs_local = runsLocal
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd apps/study-hub && node tests/api-units.test.js`
Expected: PASS

- [ ] **Step 5: 跑全量测试**

Run: `cd apps/study-hub && node tests/run-all.js`
Expected: 全部通过

- [ ] **Step 6: 提交**

```bash
git add apps/study-hub/server/api-units.js apps/study-hub/tests/api-units.test.js
git commit -m "feat(api): /api/units 支持 runs_local 过滤"
git push origin main
```

---

## Task 6：超时第二档（脚本 10 分钟，用户代码仍 60 秒）

**Files:**
- Modify: `apps/study-hub/judge/sandbox.js:31-35`（常量）、`apps/study-hub/judge/sandbox.js:230-240`（`clampTimeoutMs`）、`apps/study-hub/judge/sandbox.js:426-428`（`runCommand`）
- Modify: `apps/study-hub/judge/runner.js:270`（卡片脚本预算）
- Test: `apps/study-hub/tests/script-timeout.test.js`（新建）

**Interfaces:**
- Consumes: Task 1 的 `unit.extra.timeout_s`。
- Produces:
  - `sandbox.MAX_SCRIPT_TIMEOUT_MS`（`600000`）。
  - `clampTimeoutMs(ms, cap)`：第二个参数是上限；不给时用 `MAX_TIMEOUT_MS`（**原行为不变**）。
  - `runCommand(cmd, args, {timeoutMs, maxTimeoutMs})`：透传上限。
  - `runner.js` 卡片分支按 `unit.extra.timeout_s` 算预算。

- [ ] **Step 1: 写失败测试**

新建 `apps/study-hub/tests/script-timeout.test.js`：

```javascript
"use strict"
/**
 * script-timeout.test.js —— 断言脚本的超时第二档
 *
 * AI Infra 的实验真实耗时 60–600 秒（量化三组对照、并发拐点压测），
 * 而用户提交代码的超时上限必须留在 60 秒。所以给脚本另开一档：
 * MAX_TIMEOUT_MS 不动，脚本用 MAX_SCRIPT_TIMEOUT_MS。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const sandbox = require("../judge/sandbox.js")

test("MAX_TIMEOUT_MS 仍然是 60000（用户代码的上限不动）", function () {
  assert.strictEqual(sandbox.MAX_TIMEOUT_MS, 60000)
})

test("MAX_SCRIPT_TIMEOUT_MS 是 600000（脚本 10 分钟）", function () {
  assert.strictEqual(sandbox.MAX_SCRIPT_TIMEOUT_MS, 600000)
})

test("clampTimeoutMs 不给上限时行为与从前一致", function () {
  assert.strictEqual(sandbox.clampTimeoutMs(1500), 1500)
  assert.strictEqual(sandbox.clampTimeoutMs(999999), 60000)
  assert.strictEqual(sandbox.clampTimeoutMs(0), sandbox.DEFAULT_TIMEOUT_MS)
  assert.strictEqual(sandbox.clampTimeoutMs(-5), sandbox.DEFAULT_TIMEOUT_MS)
  assert.strictEqual(sandbox.clampTimeoutMs("abc"), sandbox.DEFAULT_TIMEOUT_MS)
})

test("clampTimeoutMs 给上限时按上限夹", function () {
  assert.strictEqual(sandbox.clampTimeoutMs(300000, 600000), 300000)
  assert.strictEqual(sandbox.clampTimeoutMs(999999, 600000), 600000)
})

test("clampTimeoutMs 的上限非法时退回默认上限", function () {
  assert.strictEqual(sandbox.clampTimeoutMs(999999, 0), 60000)
  assert.strictEqual(sandbox.clampTimeoutMs(999999, "abc"), 60000)
  assert.strictEqual(sandbox.clampTimeoutMs(999999, -1), 60000)
})
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd apps/study-hub && node tests/script-timeout.test.js`
Expected: FAIL，`sandbox.MAX_SCRIPT_TIMEOUT_MS` 是 `undefined`

- [ ] **Step 3: 改 `sandbox.js` 的常量与 `clampTimeoutMs`**

改 `apps/study-hub/judge/sandbox.js` 第 31-35 行：

```javascript
/**
 * 单次运行的超时硬上限（毫秒）。
 * 上游（请求参数、settings）可以调小，不许调大到能把服务挂死的程度。
 * 传进来的 timeoutMs 一律先夹到这里再交给 runCommand。
 */
const MAX_TIMEOUT_MS = 60000

/**
 * 断言脚本的超时上限（毫秒）——**第二档**，10 分钟。
 *
 * 为什么另开一档：AI Infra 的实验真实耗时 60–600 秒（量化三组对照、
 * 并发拐点压测、vLLM 端到端 TTFT/TPOT），30 秒的默认预算跑不完。
 * 但用户提交代码的 60 秒上限不能动——那是 OJ 判题的时限契约，
 * 抬高它会让一个死循环程序占住服务十分钟。
 */
const MAX_SCRIPT_TIMEOUT_MS = 600000
```

再改 `clampTimeoutMs`（第 230-240 行）：

```javascript
/**
 * 把传入的超时夹到 [1, 上限]。
 * 非法值（非数字、<= 0）退化成 DEFAULT_TIMEOUT_MS。
 * cap 是可选的上限：不给用 MAX_TIMEOUT_MS（用户代码那档），给了用给的。
 * @param {number|string|undefined|null} ms
 * @param {number|string|undefined|null} [cap]
 * @returns {number}
 */
function clampTimeoutMs(ms, cap) {
  const n = Number(ms)
  const c = Number(cap)
  const ceiling = Number.isFinite(c) && c > 0 ? c : MAX_TIMEOUT_MS
  if (!Number.isFinite(n) || n <= 0) return DEFAULT_TIMEOUT_MS
  return Math.min(Math.floor(n), ceiling)
}
```

- [ ] **Step 4: `runCommand` 透传上限**

改 `apps/study-hub/judge/sandbox.js` 的 `runCommand`（第 426-429 行）：

```javascript
function runCommand(cmd, args, options) {
  const opts = options || {}
  const timeoutMs = clampTimeoutMs(opts.timeoutMs, opts.maxTimeoutMs)
  const started = Date.now()
```

并把该函数上方的 JSDoc `@param` 改为：

```javascript
 * @param {{cwd?: string, input?: string, timeoutMs?: number, maxTimeoutMs?: number, env?: object}} [options]
```

- [ ] **Step 5: 改 `runner.js` 的卡片分支**

改 `apps/study-hub/judge/runner.js` 第 270 行：

```javascript
  const timeoutMs = p.timeoutMs ? clampTimeoutMs(p.timeoutMs) : SCRIPT_TIMEOUT_MS
```

改成：

```javascript
  // 卡片的脚本预算：单元可以用 unit.extra.timeout_s 申请更长的预算
  // （AI Infra 的实验要跑几分钟），但上限仍由 sandbox 的第二档夹住——
  // 单元文件写 9999 也不会真跑 9999 秒。
  const extra = ctx.unit && ctx.unit.extra ? ctx.unit.extra : null
  const declared = extra ? Number(extra.timeout_s) * 1000 : NaN
  const scriptBudget = Number.isFinite(declared) && declared > 0 ? declared : SCRIPT_TIMEOUT_MS
  const timeoutMs = p.timeoutMs ? clampTimeoutMs(p.timeoutMs) : scriptBudget
```

再改第 290-294 行的 `runCommand` 调用，加 `maxTimeoutMs`：

```javascript
    const run = await runCommand(python.cmd, python.args.concat([scriptTarget]), {
      cwd: sandbox.dir,
      timeoutMs: timeoutMs,
      maxTimeoutMs: MAX_SCRIPT_TIMEOUT_MS,
      env: env,
    })
```

在 `judge/runner.js` 顶部的 require 区加上：

```javascript
const { MAX_SCRIPT_TIMEOUT_MS } = require("./sandbox.js")
```

（若该文件已经整体 require 了 sandbox.js，则改为从既有解构里取，不要重复 require。）

- [ ] **Step 6: 跑测试确认通过**

Run: `cd apps/study-hub && node tests/script-timeout.test.js`
Expected: PASS，6 条全过

- [ ] **Step 7: 跑全量测试**

Run: `cd apps/study-hub && node tests/run-all.js`
Expected: 全部通过。**若 `p2-sandbox.test.js` 红**：它钉着 `MAX_TIMEOUT_MS === 60000`，我们没改这个常量，应该仍绿；红了说明改动越界，回退检查。

- [ ] **Step 8: 提交**

```bash
git add apps/study-hub/judge/sandbox.js apps/study-hub/judge/runner.js apps/study-hub/tests/script-timeout.test.js
git commit -m "feat(judge): 断言脚本超时加第二档（10 分钟），用户代码上限不动"
git push origin main
```

---

## Task 7：`renderUnitFile` 与 `FRONTMATTER_ORDER` 认附加字段

**Files:**
- Modify: `apps/study-hub/core/supply/unit-file.js:24-42`（`FRONTMATTER_ORDER`）、`apps/study-hub/core/supply/unit-file.js:91-120`（`renderUnitFile`）
- Test: `apps/study-hub/tests/supply-unit-file.test.js`（追加用例）

**Interfaces:**
- Consumes: Task 1 的 `core/unit/extra.js`。
- Produces: `renderUnitFile` 渲染出的单元文件含 `runs_local` / `needs` / `fallback` / `timeout_s` 四行，且**排在 `subtracks` 之后**；`applyFrontmatterPatch` 写回 `status` 时**不丢**这四行。

**为什么必须做**：供给流水线的 `writeVerdict`（`core/supply/verify.js:180`）在单元文件不存在时会调 `renderUnitFile` 生成一个新文件。`FRONTMATTER_ORDER` 是写死的数组——不加这四个键，新生成的文件**根本没有附加字段**，loader C 读出来 `extra` 全是缺省，`runs_local` 永远筛不出来。

- [ ] **Step 1: 写失败测试**

在 `apps/study-hub/tests/supply-unit-file.test.js` 末尾追加：

```javascript
test("renderUnitFile 渲染出 4 个附加字段，排在 subtracks 之后", function () {
  const text = renderUnitFile(
    {
      id: "ai:probe",
      track: "ai",
      kind: "card",
      title: "探针",
      difficulty: "medium",
      topics: ["infra:显存"],
      source: "generated",
      language: "python",
      demo: "units/ai/probe.demo.py",
      reference: "",
      graded: "script",
      script: "units/ai/probe.assert.py",
      status: "draft",
      reviewed: false,
      reviewed_at: "",
      subtracks: [],
      runs_local: "yes",
      needs: ["vram"],
      fallback: "降级读文档",
      timeout_s: 120,
    },
    { explain: "讲解正文", related: ["- 层：L0"] }
  )
  const lines = text.split("\n")
  const idx = function (key) {
    for (let i = 0; i < lines.length; i += 1) {
      if (lines[i].indexOf(key + ":") === 0) return i
    }
    return -1
  }
  assert.ok(idx("runs_local") > idx("subtracks"), "runs_local 要排在 subtracks 之后")
  assert.ok(idx("needs") > idx("subtracks"))
  assert.ok(idx("fallback") > idx("subtracks"))
  assert.ok(idx("timeout_s") > idx("subtracks"))
  assert.strictEqual(lines[idx("runs_local")], "runs_local: yes")
  assert.strictEqual(lines[idx("needs")], "needs: [vram]")
  assert.strictEqual(lines[idx("timeout_s")], "timeout_s: 120")
})

test("applyFrontmatterPatch 改 status 时不丢附加字段", function () {
  const before = [
    "---",
    "id: ai:probe",
    "track: ai",
    "kind: card",
    "title: 探针",
    "status: draft",
    "subtracks: []",
    "runs_local: yes",
    "needs: [vram]",
    "fallback: 降级读文档",
    "timeout_s: 120",
    "---",
    "",
    "## 讲解",
    "",
    "正文",
    "",
  ].join("\n")
  const after = applyFrontmatterPatch(before, { status: "verified" })
  assert.ok(after.indexOf("runs_local: yes") !== -1, "runs_local 不能被丢掉")
  assert.ok(after.indexOf("needs: [vram]") !== -1)
  assert.ok(after.indexOf("fallback: 降级读文档") !== -1)
  assert.ok(after.indexOf("timeout_s: 120") !== -1)
  assert.ok(after.indexOf("status: verified") !== -1)
})
```

若该文件顶部还没 import `renderUnitFile` / `applyFrontmatterPatch`，从 `../core/supply/unit-file.js` 里补上。

- [ ] **Step 2: 跑测试确认失败**

Run: `cd apps/study-hub && node tests/supply-unit-file.test.js`
Expected: FAIL，`idx("runs_local")` 是 `-1`（字段还没渲染）

- [ ] **Step 3: 改 `FRONTMATTER_ORDER`**

改 `apps/study-hub/core/supply/unit-file.js` 第 25-42 行的 `FRONTMATTER_ORDER`：

```javascript
const FRONTMATTER_ORDER = [
  "id",
  "track",
  "kind",
  "title",
  "difficulty",
  "topics",
  "source",
  "language",
  "demo",
  "reference",
  "graded",
  "script",
  "status",
  "reviewed",
  "reviewed_at",
  "subtracks",
  // —— 以下 4 个是 AI Infra 轨的附加元数据（契约 §1.2 的 17 个冻结字段之外的）——
  // 顺序固定：子tracks 之后。缺省值见 core/unit/extra.js。
  "runs_local",
  "needs",
  "fallback",
  "timeout_s",
]
```

- [ ] **Step 4: 改 `renderUnitFile` 的缺省值**

改 `apps/study-hub/core/supply/unit-file.js` 的 `renderUnitFile`（第 95-107 行），在现有的缺省分支里加两个：

```javascript
    FRONTMATTER_ORDER.forEach(function (key) {
      let value = f[key]
      if (value === undefined || value === null) {
        if (key === "topics" || key === "subtracks" || key === "needs") value = []
        else if (key === "reviewed") value = false
        else if (key === "difficulty") value = "medium"
        else if (key === "status") value = "draft"
        else if (key === "kind") value = "card"
        else if (key === "graded") value = "script"
        else if (key === "runs_local") value = "no"
        else if (key === "timeout_s") value = DEFAULT_TIMEOUT_S
        else value = ""
      }
      lines.push(key + ": " + formatFieldValue(value))
    })
```

在文件顶部的 require 区加：

```javascript
const { DEFAULT_TIMEOUT_S } = require("../unit/extra.js")
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cd apps/study-hub && node tests/supply-unit-file.test.js`
Expected: PASS

- [ ] **Step 6: 跑全量测试**

Run: `cd apps/study-hub && node tests/run-all.js`
Expected: 全部通过。**若 `supply-contract.test.js` 红**：它钉着跨计划的契约，可能断言了 `FRONTMATTER_ORDER` 的长度或内容——按新形状更新那条断言。

- [ ] **Step 7: 提交**

```bash
git add apps/study-hub/core/supply/unit-file.js apps/study-hub/tests/supply-unit-file.test.js
git commit -m "feat(supply): FRONTMATTER_ORDER 加 4 个附加字段，写回不丢"
git push origin main
```

---

## Task 8：learning 仓的 `units/ai/` 目录与 5 个骨架单元

**Files:**
- Create: `learning/units/ai/ai-map-dependency.md` + `.demo.py` + `.assert.py`
- Create: `learning/units/ai/ai-map-positioning.md` + `.demo.py` + `.assert.py`
- Create: `learning/units/ai/ai-env-windows.md` + `.demo.py` + `.assert.py`
- Create: `learning/units/ai/ai-env-wsl.md` + `.demo.py` + `.assert.py`
- Create: `learning/units/ai/ai-baseline-probe.md` + `.demo.py` + `.assert.py`
- Create: `learning/units/ai/_meta/acceptance.md`

**Interfaces:**
- Consumes: Task 2 的 loader C（读附加字段）、Task 7 的 `FRONTMATTER_ORDER`（写回不丢字段）。
- Produces: 5 个能被 loader C 读出的 Unit；每个带 `.demo.py`（可运行演示）与 `.assert.py`（断言脚本）。

**前置检查（必做）**：learning 仓当前工作区**不干净**（有 `notes/` 的改动与未跟踪文件）。`core/sync/rounds.js` 的 `assertCleanWorktree` 会因此拒绝写回。**必须先处理干净**，否则 Task 8 的写回全被挡住。

- [ ] **Step 1: 检查 learning 仓工作区（发现脏就停下来问，不要自行处置）**

```bash
cd /c/code/learning
git status --short
```

**已知现状（实测 2026-11-26）**：learning 仓工作区**不干净**，
有 `notes/` 下的若干改动与未跟踪文件（`notes/_index/db.md`、`notes/_index/home.md`、
`notes/_meta/schema.md`、`notes/learn_deep/db/README.md`、`notes/_briefing/`、
`notes/learn_deep/db/pg-hot-update.md`）。

**这些是用户的学习笔记，不是本计划的产物。** 处理方式只有两种：
1. **用户已提交** → 直接往下走
2. **用户未提交** → **停下来问用户**：这些笔记改动要不要先提交？还是先 `git stash`？
   **不许擅自 `git add` / `git commit` / `git checkout` / `git stash` 用户的笔记改动。**

原因：`core/sync/rounds.js` 的 `assertCleanWorktree` 会因工作区脏而**拒绝一切写回**，
本任务后面所有写单元文件的步骤都会被挡住；而且这些笔记属于用户，不该由本计划处置。

> 提示：Task 8 的 demo/assert 脚本是**新建文件**，不经过 `assertCleanWorktree`
> （那是写回既有文件时的检查）。但 Task 9 之后用 `supply.js verify` 写回 `status: verified`
> 时会被挡。所以这个检查必须做。

- [ ] **Step 2: 建目录**

```bash
mkdir -p /c/code/learning/units/ai/_meta
```

- [ ] **Step 3: 写 `ai-map-dependency` 单元文件**

`/c/code/learning/units/ai/ai-map-dependency.md`：

```markdown
---
id: ai:map-dependency
track: ai
kind: card
title: AI Infra 知识地图：依赖图
difficulty: easy
topics: [infra:地图]
source: self
language: python
demo: units/ai/ai-map-dependency.demo.py
reference: ""
graded: script
script: units/ai/ai-map-dependency.assert.py
status: draft
reviewed: false
reviewed_at: ""
subtracks: []
runs_local: yes
needs: []
fallback: ""
timeout_s: 60
---

## 讲解

### 这张图解决什么问题

AI Infra 不是一个技术名词清单，而是一张**依赖图**。学一个节点的正确问法不是「它是什么」，
而是三个问题：

1. **它解决什么问题** —— 没有它，系统会在哪一步崩掉或浪费
2. **它的上游是谁** —— 不理解上游，这个节点就是死记
3. **它的下游被谁支撑** —— 不知道谁受益，就判断不出它的取舍

**边比节点更重要。** 节点是名词，边是因果。

### 五层的依赖链

```
L0 数值与显存底座   ──造出「一把尺子」（显存容量 + 带宽）
        │
        ▼
L1 一次前向的真实代价 ──用尺子量一次前向（FLOPs / 字节 / KV）
        │
        ▼
L2 引擎怎么不浪费   ──单请求利用率太低，逼出连续批处理与 PagedAttention
        │
        ▼
L3 更少显存 + 更专业务 ──量化与延迟归因
        │
        ▼
L4 另外三个方向     ──训练分布式 / 算子编译 / 检索与平台（平行链，骨架级）
```

L0 → L1 的边界：**不先把尺子造好，L1 只能变成公式背诵。**
L1 → L2 的边界：单请求 decode 的 GPU 利用率天然低于 20% —— 这个事实直接推出 L2 的全部动机。
L2 → L3 的边界：先亲手把 8GB 打满看到 OOM，才有资格问「怎么多塞 3 倍请求」。

### 图的边怎么读

一条边 `A ──▶ B` 读作：**A 的结论是 B 存在的前提**。
反过来读（B 不存在会怎样）就是**A 解决的问题**。

## 关联

- 层：L0
- 前置：无（这是第一个单元）
- 后继：ai:storage-hierarchy, ai:map-positioning
- 下一步：读 `ai:map-positioning` 拿到每个节点的一句话定位
```

- [ ] **Step 4: 写它的 demo**

`/c/code/learning/units/ai/ai-map-dependency.demo.py`：

```python
#!/usr/bin/env python3
"""ai-map-dependency 的演示：把五层依赖链与层内节点数打出来。

这个 demo 的作用不是「算什么」，而是**证明地图的结构是机器可核对的**：
层数、每层的节点数、依赖方向都对得上，就说明地图没有漂。
"""

LAYERS = [
    ("L0", "数值与显存底座", 3),
    ("L1", "一次前向的真实代价", 7),
    ("L2", "引擎怎么不浪费", 11),
    ("L3", "更少显存 + 更专业务", 9),
    ("L4", "另外三个方向", 7),
]

# 层内的前置关系（只记「谁是谁的前置」，边不带权重）
EDGES = [
    ("L0", "L1"),
    ("L1", "L2"),
    ("L2", "L3"),
    ("L3", "L4"),
]


def main():
    print("=== AI Infra 五层依赖链 ===")
    for code, name, count in LAYERS:
        print("%s  %-16s  单元数 %d" % (code, name, count))

    total = sum(count for _, _, count in LAYERS)
    print("合计单元数（不含 4 个非讲单元）：%d" % total)

    print("=== 层间依赖 ===")
    for src, dst in EDGES:
        print("%s --> %s" % (src, dst))

    # 单调性检查：层号必须严格递增，且 L4 之后没有出边
    codes = [code for code, _, _ in LAYERS]
    ordered = codes == sorted(codes)
    print("层号单调递增：%s" % ("是" if ordered else "否"))

    dangling = [src for src, _ in EDGES if src not in codes]
    print("悬空边数：%d" % len(dangling))

    print("演示跑完。")


if __name__ == "__main__":
    main()
```

- [ ] **Step 5: 写它的 assert**

`/c/code/learning/units/ai/ai-map-dependency.assert.py`：

```python
#!/usr/bin/env python3
"""ai-map-dependency 的断言脚本：核对的层数、节点数与依赖方向。

不复用 demo 的函数——**独立复算**：demo 的 LAYERS 改错了，这里照样红。
"""

import os
import shutil
import subprocess
import sys
import tempfile


DEMO = os.environ.get("STUDY_HUB_DEMO") or os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "ai-map-dependency.demo.py"
)

# 独立的一份期望（不 import demo）
EXPECTED_LAYERS = ["L0", "L1", "L2", "L3", "L4"]
EXPECTED_TOTAL = 37  # 28 讲 + 9 实验（不含 4 个非讲单元）
EXPECTED_EDGES = 4


def decode(raw):
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("latin-1")


def run_demo():
    if not os.path.exists(DEMO):
        return None, "找不到 demo：" + DEMO
    sandbox = tempfile.mkdtemp(prefix="study-hub-assert-")
    try:
        target = os.path.join(sandbox, os.path.basename(DEMO))
        # 复制进临时目录再跑：demo 可能在自己目录里写文件，直接在 learning 仓里跑会污染工作树。
        shutil.copy2(DEMO, target)
        proc = subprocess.run([sys.executable, target], capture_output=True, timeout=180)
        return proc, None
    finally:
        shutil.rmtree(sandbox, ignore_errors=True)


def main():
    proc, err = run_demo()
    if err is not None:
        print(err)
        return 1
    out = decode(proc.stdout)
    print("─── demo 输出开始 ───")
    print(out.rstrip())
    print("─── demo 输出结束 ───")

    failed = []

    for code in EXPECTED_LAYERS:
        if code not in out:
            failed.append("缺少层：" + code)

    if "合计单元数（不含 4 个非讲单元）：%d" % EXPECTED_TOTAL not in out:
        failed.append("合计单元数不是 %d" % EXPECTED_TOTAL)

    for src, dst in [("L0", "L1"), ("L1", "L2"), ("L2", "L3"), ("L3", "L4")]:
        if ("%s --> %s" % (src, dst)) not in out:
            failed.append("缺少依赖边：%s --> %s" % (src, dst))

    if "层号单调递增：是" not in out:
        failed.append("层号不是单调递增的")

    if "悬空边数：0" not in out:
        failed.append("存在悬空边")

    if proc.returncode != 0:
        failed.append("demo 退出码非 0：" + str(proc.returncode))

    if failed:
        for item in failed:
            print("断言失败：" + item)
        print("─── demo 错误输出开始 ───", file=sys.stderr)
        print(decode(proc.stderr).rstrip(), file=sys.stderr)
        print("─── demo 错误输出结束 ───", file=sys.stderr)
        return 1

    print("全部断言通过（%d 条）" % (len(EXPECTED_LAYERS) + 3))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 6: 跑 demo + assert 验证**

Run（Windows 侧）:
```bash
cd /c/code/learning/units/ai
py -3 ai-map-dependency.demo.py
py -3 ai-map-dependency.assert.py
```
Expected: demo 打出五层与合计 37；assert 打印 `全部断言通过（8 条）`，退出码 0

- [ ] **Step 7: 写其余 4 个单元**

按同样的三件套结构写：

| id | 内容要点 | `runs_local` | `timeout_s` |
|---|---|---|---|
| `ai:map-positioning` | 41 个单元的一句话定位表（`id \| 解决什么问题 \| 上游 \| 下游 \| runs_local`）。demo 打印表头与行数；assert 独立复算行数 = 41、且每个 id 前缀都是 `ai:` | `yes` | 60 |
| `ai:env-windows` | 环境 A：改 `.wslconfig`（memory=11GB / swap=12GB）+ WSL venv + CUDA 版 torch。demo 跑 `wsl -- bash -lc "free -g"` 并打印；assert 检查 `Mem: total` ≥ 10 GB | `yes` | 300 |
| `ai:env-wsl` | 环境 B：vLLM + Triton + 模型下载 + 冒烟。demo 跑 `python -c "import vllm"` 与 `torch.cuda.is_available()`；assert 检查 CUDA 可用且 capability 是 `(8, 9)` | `yes` | 600 |
| `ai:baseline-probe` | 20 题基线探针。demo 打印 20 道题与作答区；assert 读 `answers.json` 算分，判三档（≥16 直达 / 10–15 加 2 单元 / <10 加 5 单元） | `yes` | 120 |

**每个 assert 脚本都要满足**：
- 末尾打印 `全部断言通过（N 条）`（`SCRIPT_PASS_MARK`，契约冻结）
- 失败时逐条打印 `断言失败：<原因>`（`SCRIPT_FAIL_PREFIX`，契约冻结）
- demo 的 stdout 回显出来（单元页右栏要给用户看 demo 到底打印了什么）
- **不复用 demo 的函数**——独立复算

- [ ] **Step 8: 写 `_meta/acceptance.md` 模板**

`/c/code/learning/units/ai/_meta/acceptance.md`：

```markdown
# AI Infra 轨验收记录

本文件是**线 2**（链路真跑通）的落点：六条命令的实测输出 + 手算预测值的对照。

> `_meta/` 是子目录，不是 `units/ai/` 的直系文件 —— 直系下的 `.md` 会被 loader C
> 当单元文件读，没有 frontmatter 的会报「单元 id 非法」。

## 环境基线

| 项 | 值 |
|---|---|
| GPU | |
| 显存 | |
| 驱动 | |
| CUDA | |
| Python | |
| vLLM | |

## 六条命令的实测

### 1. GPU 与算力基线

```
（贴 nvidia-smi --query-gpu=... 的输出）
```

### 2. torch 真能用 GPU

```
（贴 python -c "import torch;print(...)" 的输出）
```

### 3. 实测 HBM 带宽（期望 200–300 GB/s）

```
（贴 bench_hbm.py 的输出）
```

### 4. 推理 TTFT / TPOT（期望 TTFT < 1.5s、TPOT < 40ms）

```
（贴 bench_infer.py 的 JSON）
```

### 5. KV Cache 随并发增长

**手算预测**：

| 序列长度 | 并发 | 预测 KV 字节 | 实测 | 偏差 |
|---|---|---|---|---|
| | | | | |

**偏差解释**：

### 6. 量化三组对照（期望 INT4 比 FP16 快 1.5–2.5×，精度损失 < 5%）

```
（贴 bench_quant.py 的输出）
```

## 终期判定表

| # | 项目 | 判据 | 结果 |
|---|---|---|---|
| 1 | 单元覆盖 | verified ≥ 37/41 | |
| 2 | 四闸门 | 36/36 全绿 | |
| 3 | topics 掌握度 | infra:* ≥ 0.5 | |
| 4 | 六条实测命令 | 全部落区间 | |
| 5 | 预测 vs 实测 | 偏差写清并解释 | |
| 6 | 9 个卡点问题 | 每题 ≥ 4/5 要素 | |
| 7 | 地图完整 | 41 节点图 + 定位表齐全 | |
| 8 | 环境可重建 | 完整环境记录，能复现 | |
```

- [ ] **Step 9: 验证 study-hub 能读出这 5 个单元**

```bash
cd /c/code/projects/apps/study-hub
STUDY_HUB_LEARNING_ROOT=/c/code/learning node -e "
const { loadUnits } = require('./core/unit/index.js');
const path = require('node:path');
const root = process.env.STUDY_HUB_LEARNING_ROOT;
const res = loadUnits({
  learningRoot: root,
  localBank: null,
  unitsDir: path.join(root, 'units'),
  grokkingDir: path.join(root, 'scaffold', 'grokking'),
});
const ai = res.units.filter(function (u) { return u.track === 'ai' && u.id.indexOf('ai:ai-') === 0; });
console.log('AI Infra 单元数：' + ai.length);
ai.forEach(function (u) {
  console.log(u.id + '  runs_local=' + u.extra.runs_local + '  timeout_s=' + u.extra.timeout_s);
});
console.log('警告数：' + res.warnings.length);
res.warnings.forEach(function (w) { console.log('  ⚠ ' + w); });
"
```

Expected:
- `AI Infra 单元数：5`
- 5 行单元，各自 `runs_local=`
- **`警告数：0`** —— 有警告说明 frontmatter 有问题（比如缺收尾 `---`），**警告必须清零**

- [ ] **Step 10: 验证供给流水线能看见它们**

```bash
cd /c/code/projects/apps/study-hub
STUDY_HUB_LEARNING_ROOT=/c/code/learning node tools/supply.js report --track ai
```

Expected: 打印 AI 轨的计数（total / ready / debt）与欠账明细。**如果报「单元 id 非法」，回 Step 9 修 frontmatter。**

- [ ] **Step 11: 验证判题器能判一个卡片单元**

```bash
cd /c/code/projects/apps/study-hub
STUDY_HUB_LEARNING_ROOT=/c/code/learning node -e "
const { judgeUnit } = require('./judge/runner.js');
judgeUnit({
  learningRoot: process.env.STUDY_HUB_LEARNING_ROOT,
  unit: { id: 'ai:ai-map-dependency', entry: { script: 'units/ai/ai-map-dependency.assert.py', demo: 'units/ai/ai-map-dependency.demo.py', graded: 'script' } },
}).then(function (r) {
  console.log('verdict=' + r.verdict);
  console.log(r.message);
});
"
```

Expected: `verdict=pass`，message 里含「全部断言通过」

- [ ] **Step 12: 提交（learning 仓，不自动 commit——先给用户看 diff）**

```bash
cd /c/code/learning
git status --short
git diff --stat
```

**把 diff 摆给用户看，问要不要提交。** 契约 §10：「绝不自动提交」。用户同意后：

```bash
git add units/ai/
git commit -m "feat(units): AI Infra 轨首批 5 个骨架单元"
git push origin main
```

---

## Task 9：验收清单与收口

**Files:**
- Modify: `apps/study-hub/tests/supply-contract.test.js`（若它钉了 `FRONTMATTER_ORDER` 的形状）
- Create: `learning/units/ai/_meta/README.md`（说明 `_meta/` 的约定，防后人把文件放错位置）

**Interfaces:**
- Consumes: Task 1–8 的全部产出。
- Produces: P0 阶段的完整验收证据。

- [ ] **Step 1: 跑全量测试**

Run: `cd apps/study-hub && node tests/run-all.js`
Expected: `122 / 122 套件通过`（P0 新增 3 个套件：extra / runs-local-filter / script-timeout）

- [ ] **Step 2: 验证契约文档与实现一致**

打开 `docs/superpowers/plans/2026-11-12-study-hub-接口契约.md` 的 §1.2，确认 17 个字段表**一个字没改**。

- [ ] **Step 3: 写 `_meta/README.md`**

`/c/code/learning/units/ai/_meta/README.md`：

```markdown
# `units/ai/_meta/` —— AI Infra 轨的附属文件

这个目录放**不是单元**的附属文件：验收记录、地图的机器可读副本、临时脚本。

**为什么必须是子目录**：`apps/study-hub/core/unit/index.js` 的 `listMarkdown` 会列出
`units/<track>/` 下**所有** `.md` 文件（非递归，但不过滤 `_` 前缀），逐个交给 loader C。
放在 `units/ai/` 直系下的 `.md` 没有 frontmatter，会因为 id 非法被拒，
**并在每次启动时产出警告**。

子目录名不以 `.md` 结尾，会被同一个过滤器排除，不污染单元索引。

## 放什么

| 文件 | 内容 |
|---|---|
| `acceptance.md` | 线 2 的六条命令实测输出 + 手算预测值对照 + 终期判定表 |
| `README.md` | 本文件 |

## 不放什么

单元本身（`.md` + `.demo.py` + `.assert.py`）一律放 `units/ai/` **直系**下。
```

- [ ] **Step 4: 确认 learning 仓里没有不该有的直系 `.md`**

```bash
cd /c/code/learning
ls units/ai/*.md 2>/dev/null | grep -v "^units/ai/ai-" || echo "直系下只有 ai-* 前缀的单元文件"
```

Expected: `直系下只有 ai-* 前缀的单元文件`

- [ ] **Step 5: 提交**

```bash
# engineering 仓
git add apps/study-hub/
git commit -m "test(study-hub): P0 收口，AI Infra 轨工装全部就位"
git push origin main

# learning 仓（先给用户看 diff）
cd /c/code/learning
git add units/ai/_meta/README.md
git commit -m "docs(units): 说明 _meta/ 的约定"
git push origin main
```

- [ ] **Step 6: P0 验收清单**

逐项打勾，全绿才算 P0 完成：

| # | 项目 | 判据 | 怎么验 |
|---|---|---|---|
| 1 | 单元可被读出 | 5 个 AI Infra 单元 | Task 8 Step 9 |
| 2 | 启动零警告 | `警告数：0` | Task 8 Step 9 |
| 3 | 附加字段保留 | `renderUnitFile` + `applyFrontmatterPatch` 测试 | Task 7 |
| 4 | runs_local 可筛 | `GET /api/units?runs_local=yes` | Task 5 |
| 5 | 长脚本能跑 | 卡片分支按 `timeout_s` 给预算，上限 10 分钟 | Task 6 |
| 6 | 用户代码上限未变 | `MAX_TIMEOUT_MS === 60000` | Task 6 Step 8 |
| 7 | 供给流水线可见 | `supply.js report --track ai` | Task 8 Step 10 |
| 8 | 判题器能判卡片 | `verdict=pass` | Task 8 Step 11 |
| 9 | 全量测试绿 | `122 / 122` | Task 9 Step 1 |
| 10 | 契约未被破坏 | §1.2 字段表一字未改 | Task 9 Step 2 |

---

## 后续计划（P0 完成后单独出）

| 阶段 | 交付物 | 单元数 |
|---|---|---|
| **P1** | L0 + L1（数值底座 + 一次前向的真实代价） | 10（含 2 实验） |
| **P2** | L2（引擎怎么不浪费）+ 线 2 六条命令首跑 | 11（含 4 实验） |
| **P3** | L3（量化 + 延迟归因） | 9（含 3 实验） |
| **P4** | L4（训练/算子/检索/平台，骨架级）+ 终期判定表打分 | 7 |

每个阶段的单元清单、demo 与 assert 的具体代码，在该阶段的计划里展开。
P0 只负责**工装与骨架**——代码改动、加载链路、附加字段、超时第二档、
5 个骨架单元、以及验证这些机制能跑通的最小闭环。

---

## Self-Review

**规格覆盖检查**（对照 `docs/superpowers/specs/2026-11-26-ai-infra-learning-track-design.md`）：

| 规格条目 | 覆盖任务 |
|---|---|
| §4.2 四个附加元数据字段 | Task 1 / 2 / 7 |
| §4.3 可跑性三级 | Task 1（`makeExtra` 的三档） |
| §6.2 零改动即可用 | Task 8 Step 9 验证（不改 loader 也能读出单元） |
| §6.3 六处代码改动 | Task 1 / 2 / 3 / 4 / 5 / 6 / 7（7 个任务对应规格的 6 个文件，其中 `unit.js` 与 `extra.js` 合并进 Task 1） |
| §6.3 `unit.extra` 嵌套袋 | Task 1 |
| §6.3 超时第二档 | Task 6 |
| §7.2 步骤 1–2（WSL 内存 + torch） | Task 8 的 `ai:env-windows` |
| §7.2 步骤 3–4（vLLM + 冒烟） | Task 8 的 `ai:env-wsl` |
| §5.1 地图与定位表 | Task 8 的 `ai-map-dependency` / `ai-map-positioning` |
| §9 基线自测 | Task 8 的 `ai-baseline-probe` |
| §8.3 线 2 六条命令 | Task 8 Step 8 的 `_meta/acceptance.md` 模板（P2 才真跑） |

**不在 P0 范围**（规格已明确排除）：P1–P4 的 37 个内容单元、`generate` 模板改造、
13 张 ML 基础卡、`judge/asserts.js` 的 cmp 四档、多卡与租云。

**占位符扫描**：本计划无 TBD / TODO / 「类似 Task N」/ 无代码的代码步骤。
每个代码步骤都给出了可直接粘贴的完整代码；需要「照抄现有测试」的步骤
（Task 5 Step 1、Task 8 Step 7）已显式指明要读哪个文件的哪一处，不能猜。

**类型一致性**：`unit.extra` 的四个键名在 Task 1（定义）、2（loader 读）、
4（写库）、6（runner 读 timeout_s）、7（写回）里一致：
`runs_local` / `needs` / `fallback` / `timeout_s`。
`makeExtra` / `normalizeExtra` / `EXTRA_KEYS` / `RUNS_LOCAL_VALUES` /
`NEEDS_VALUES` / `DEFAULT_TIMEOUT_S` / `MAX_TIMEOUT_S` 的拼写在各任务里一致。
`MAX_SCRIPT_TIMEOUT_MS` 只在 Task 6 引入，Task 6 之后才被引用。
