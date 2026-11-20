"use strict"
/**
 * p2-attempts.test.js —— 新链路必须记作答流水
 *
 * 为什么要有这个套件：实测发现**当时全仓只有旧路由 `POST /api/attempt` 会写 `attempts` 表**，
 * 而 P2 的新链路（`POST /api/units/:id/judge` 与 `POST /api/session/submit`）一条都不写。
 * 后果是 P3 的诊断（掌握度、知识点统计、每日题量曲线）全部依赖 `attempts`，
 * 新链路上练得再多，诊断页也是一片 0——一张会被读、却没人写的表。
 * （旧路由下线后，写这张表的就是这两条新链路。）
 *
 * 契约里的依据：
 *   §4.1 `insertAttempt` 入参「P1 起新增可选 unit_id、track、assert_total、assert_passed」；
 *   §4.1 `listAttemptsByProblem`「P2 起新增别名 `listAttemptsByUnit`」；
 *   §8.2.1「P2 接入判题后：`judged` 变 `true`，`verdict` 取 G20 的五类之一」。
 *
 * 口径（本套件钉住）：
 *   - 判一次就记一条流水，`problem_id` 与 `unit_id` 都写（旧口径与新口径都能查到）；
 *   - 卡类判题不携带用户代码，`language` 记 `python`（断言脚本是 Python）；
 *   - `attempts.verdict` 是 NOT NULL，所以「没判过的完成」不记流水（不是记一条空 verdict）；
 *   - 完成（不带代码）仍然不记流水，保持 P1 的「只排不判」语义。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { createApp } = require("../server/app.js")
const { openDb, createStore } = require("../server/db.js")

/** 契约 §3.5 形状的断言脚本（简化版，行为一致） */
const CARD_SCRIPT = [
  "import os, shutil, subprocess, sys, tempfile",
  "DEMO = os.environ.get('STUDY_HUB_DEMO') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'main.py')",
  "CHECKS = [('demo 跑起来了', 'OK demo ran')]",
  "def decode(raw):",
  "    for enc in ('utf-8', 'gbk'):",
  "        try:",
  "            return raw.decode(enc)",
  "        except UnicodeDecodeError:",
  "            continue",
  "    return raw.decode('utf-8', errors='replace')",
  "def run_demo():",
  "    if not os.path.exists(DEMO):",
  "        return None, '找不到 demo：' + DEMO",
  "    sandbox = tempfile.mkdtemp(prefix='study-hub-assert-')",
  "    try:",
  "        target = os.path.join(sandbox, os.path.basename(DEMO))",
  "        shutil.copy2(DEMO, target)",
  "        proc = subprocess.run([sys.executable, target], capture_output=True, timeout=180)",
  "        return proc, None",
  "    finally:",
  "        shutil.rmtree(sandbox, ignore_errors=True)",
  "def main():",
  "    proc, err = run_demo()",
  "    if err is not None:",
  "        print(err)",
  "        return 1",
  "    out = decode(proc.stdout)",
  "    print('─── demo 输出开始 ───')",
  "    print(out.rstrip())",
  "    print('─── demo 输出结束 ───')",
  "    failed = []",
  "    for name, needle in CHECKS:",
  "        if needle not in out:",
  "            failed.append(name + '（缺少：' + needle + '）')",
  "    if proc.returncode != 0:",
  "        failed.append('demo 退出码非 0：' + str(proc.returncode))",
  "    if failed:",
  "        for item in failed:",
  "            print('断言失败：' + item)",
  "        return 1",
  "    print('全部断言通过（%d 条）' % len(CHECKS))",
  "    return 0",
  "if __name__ == '__main__':",
  "    sys.exit(main())",
  "",
].join("\n")

/** 造一个带一张数据库卡 + 一道算法题的 learning 仓 */
function tempLearning() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "p2-attempts-"))

  const cardDir = path.join(root, "scaffold", "grokking", "mvcc")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "README.md"), "# 测试卡\n\n这是讲解正文。\n", "utf8")
  fs.writeFileSync(path.join(cardDir, "main.py"), "print('OK demo ran')\n", "utf8")

  const unitsDir = path.join(root, "units", "database")
  fs.mkdirSync(unitsDir, { recursive: true })
  fs.writeFileSync(path.join(unitsDir, "mvcc.assert.py"), CARD_SCRIPT, "utf8")

  const bankDir = path.join(root, "bank", "interview")
  fs.mkdirSync(bankDir, { recursive: true })
  fs.writeFileSync(
    path.join(bankDir, "lc-00007.md"),
    [
      "---",
      "id: lc-00007",
      "source: leetcode",
      "title: 两数之和",
      "difficulty: easy",
      "topics: [哈希表, 数组]",
      "tracks: [interview]",
      "status: todo",
      "needs_fill: false",
      "created: 2026-11-12",
      "updated: 2026-11-12",
      "---",
      "",
      "## 题面",
      "",
      "给定两个整数，求它们的和。",
      "",
      "## 测试用例",
      "",
      "| input | expect |",
      "|---|---|",
      "| 1 2 | 3 |",
      "",
    ].join("\n"),
    "utf8"
  )
  return root
}

async function withApp(fn, opts) {
  const options = opts || {}
  const app = createApp(
    Object.assign(
      {
        dbPath: ":memory:",
        learningRoot: tempLearning(),
        localBank: null,
        llmClient: null,
        env: {},
      },
      options.app || {}
    )
  )
  const port = await app.listen(0)
  try {
    await fn("http://127.0.0.1:" + port, app)
  } finally {
    await app.close()
  }
}

async function postJson(base, urlPath, body) {
  const res = await fetch(base + urlPath, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body || {}),
  })
  return { status: res.status, body: await res.json() }
}

function cppSum() {
  return [
    "#include <cstdio>",
    "int main(){int a,b;if(scanf(\"%d %d\",&a,&b)!=2) return 1;printf(\"%d\\n\",a+b);return 0;}",
    "",
  ].join("\n")
}

test("db.js：P2 起有 listAttemptsByUnit 别名（契约 §4.1）", function () {
  const db = openDb(":memory:")
  const store = createStore(db)
  try {
    assert.strictEqual(typeof store.listAttemptsByUnit, "function", "缺 listAttemptsByUnit")
    store.insertAttempt({
      problem_id: "lc-00001",
      unit_id: "algo:lc-00001",
      track: "algorithm",
      ts: "2026-11-12T00:00:00.000Z",
      language: "cpp",
      code: "int main(){}",
      verdict: "pass",
      passed_cases: 1,
      total_cases: 1,
      duration_ms: 10,
      mode: "solve",
    })
    assert.strictEqual(store.listAttemptsByUnit("algo:lc-00001").length, 1)
    // 旧名保留一个版本：两个口径查得到同一行
    assert.strictEqual(store.listAttemptsByProblem("lc-00001").length, 1)
    assert.strictEqual(store.listAttemptsByUnit("algo:根本没有").length, 0)
  } finally {
    db.close()
  }
})

test("卡类判题记一条流水：problem_id 与 unit_id 都写，verdict 是真实判定", async function () {
  await withApp(async function (base, app) {
    const before = app.store.countAttempts()
    const r = await postJson(base, "/api/units/db:mvcc/judge", {})
    assert.strictEqual(r.status, 200)
    assert.strictEqual(r.body.verdict, "pass", r.body.message)

    const after = app.store.countAttempts()
    assert.strictEqual(after, before + 1, "判一次就要记一条流水，实测从 " + before + " 变成 " + after)

    const rows = app.store.listAttemptsByUnit("db:mvcc")
    assert.strictEqual(rows.length, 1, "能按 unit_id 查到这一条")
    const row = rows[0]
    assert.strictEqual(row.verdict, "pass")
    assert.strictEqual(row.unit_id, "db:mvcc", "新口径写 unit_id")
    // 旧口径也要能查到：problem_id 不是空串
    assert.ok(String(row.problem_id).length > 0, "problem_id 也要写，旧口径才不会查空")
    assert.strictEqual(Number(row.passed_cases), 2, "契约 §7.4 的两条隐含断言都过")
    assert.strictEqual(Number(row.total_cases), 2)
    assert.strictEqual(Number(row.duration_ms) > 0, true, "要记耗时")
  })
})

test("算法题判题记流水，track 是 algorithm", async function () {
  await withApp(async function (base, app) {
    const r = await postJson(base, "/api/units/algo:lc-00007/judge", {
      code: cppSum(),
      language: "cpp",
    })
    assert.strictEqual(r.status, 200)
    assert.strictEqual(r.body.verdict, "pass", r.body.message)

    const rows = app.store.listAttemptsByUnit("algo:lc-00007")
    assert.strictEqual(rows.length, 1)
    assert.strictEqual(rows[0].verdict, "pass")
    assert.strictEqual(rows[0].track, "algorithm")
    assert.strictEqual(rows[0].unit_id, "algo:lc-00007")
  })
})

test("判不过也记流水（wrong_answer 是真实练习记录，不是异常）", async function () {
  await withApp(async function (base, app) {
    // 恒输出 0，与 1+2=3 不符
    const r = await postJson(base, "/api/units/algo:lc-00007/judge", {
      code: "#include <cstdio>\nint main(){printf(\"0\\n\");return 0;}\n",
      language: "cpp",
    })
    assert.strictEqual(r.status, 200)
    assert.strictEqual(r.body.verdict, "wrong_answer", r.body.message)

    const rows = app.store.listAttemptsByUnit("algo:lc-00007")
    assert.strictEqual(rows.length, 1, "判不过也要记，否则首次通过率永远算不出来")
    assert.strictEqual(rows[0].verdict, "wrong_answer")
  })
})

test("判题失败没有把整条链路搞崩：单元不存在不记流水", async function () {
  await withApp(async function (base, app) {
    const r = await postJson(base, "/api/units/db:根本没有这张卡/judge", {})
    assert.strictEqual(r.status, 404)
    assert.strictEqual(app.store.countAttempts(), 0, "404 不该记流水")
  })
})

test("空代码 / 不支持语言这些 400 也不记流水", async function () {
  await withApp(async function (base, app) {
    const empty = await postJson(base, "/api/units/algo:lc-00007/judge", { code: "   " })
    assert.strictEqual(empty.status, 400)
    const badLang = await postJson(base, "/api/units/algo:lc-00007/judge", {
      code: "print(1)\n",
      language: "rust",
    })
    assert.strictEqual(badLang.status, 400)
    assert.strictEqual(app.store.countAttempts(), 0)
  })
})

test("流水能被 /api/health 与 /api/progress 看见（P3 诊断与首页都靠它）", async function () {
  await withApp(async function (base) {
    await postJson(base, "/api/units/db:mvcc/judge", {})
    const health = await (await fetch(base + "/api/health")).json()
    assert.strictEqual(health.attempts, 1, "health.attempts 要跟着涨")
    const progress = await (await fetch(base + "/api/progress")).json()
    assert.ok(progress.solved >= 1, "今天做过 1 个，progress.solved 不能是 0：" + progress.solved)
  })
})

test("session/submit 不带代码时不记流水（保持 P1 的「只排不判」）", async function () {
  await withApp(async function (base, app) {
    const started = await postJson(base, "/api/session/start", { n: 1 })
    assert.strictEqual(started.status, 200)
    const unitId = started.body.queue[0].unit_id
    const submitted = await postJson(base, "/api/session/submit", { unit_id: unitId })
    assert.strictEqual(submitted.status, 200)
    assert.strictEqual(submitted.body.judged, false, "不带代码就还是 P1 的只排不判")
    assert.strictEqual(submitted.body.verdict, null)
    assert.strictEqual(
      app.store.countAttempts(),
      0,
      "attempts.verdict 是 NOT NULL，「没判过的完成」不该记一条空 verdict"
    )
  })
})

test("session/submit 带代码时判题并记流水（契约 §8.2.1 的 P2 口径）", async function () {
  await withApp(async function (base, app) {
    const started = await postJson(base, "/api/session/start", { n: 1 })
    assert.strictEqual(started.status, 200)
    const unitId = started.body.queue[0].unit_id
    assert.strictEqual(unitId, "algo:lc-00007", "这一轮只放得下一道算法题")

    const submitted = await postJson(base, "/api/session/submit", {
      unit_id: unitId,
      code: cppSum(),
      language: "cpp",
    })
    assert.strictEqual(submitted.status, 200)
    assert.strictEqual(submitted.body.judged, true, "带代码就要真判")
    assert.strictEqual(submitted.body.verdict, "pass", submitted.body.message)
    assert.ok(submitted.body.passed >= 1)
    assert.ok(submitted.body.total >= 1)
    assert.strictEqual(app.store.countAttempts(), 1, "判过就要记一条流水")
    assert.strictEqual(app.store.listAttemptsByUnit(unitId).length, 1)
  })
})
