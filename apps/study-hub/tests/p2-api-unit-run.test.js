"use strict"
/**
 * p2-api-unit-run.test.js —— P2 的单元运行接口
 *
 * 钉住：
 *   1. 四条路由的路径、入参与响应形状（契约 §8.5）；
 *   2. 断言不过时 verdict 是 wrong_answer，不是 500；
 *   3. 断言脚本缺失时 verdict 是 compile_error（附中文说明），不是 500；
 *   4. 单元不存在一律 404「单元不存在」；
 *   5. LLM 走注入的假客户端，全程不联网。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { createApp } = require("../server/app.js")

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
  "        print('─── demo 错误输出开始 ───', file=sys.stderr)",
  "        print(decode(proc.stderr).rstrip(), file=sys.stderr)",
  "        print('─── demo 错误输出结束 ───', file=sys.stderr)",
  "        return 1",
  "    print('全部断言通过（%d 条）' % len(CHECKS))",
  "    return 0",
  "if __name__ == '__main__':",
  "    sys.exit(main())",
  "",
].join("\n")

/** 造一个带一张数据库卡 + 一道算法题的 learning 仓 */
function tempLearning() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "p2-learning-"))

  const cardDir = path.join(root, "scaffold", "grokking", "mvcc")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "README.md"), "# 测试卡\n\n这是讲解正文。\n", "utf8")
  fs.writeFileSync(path.join(cardDir, "NOTES.md"), "## 笔记\n\n要点一。\n", "utf8")
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
      "## 参考解法",
      "",
      "```cpp",
      "int main() { return 0; }",
      "```",
      "",
    ].join("\n"),
    "utf8"
  )
  return root
}

/** 假客户端：不联网（G12） */
function makeFakeClient() {
  return {
    name: "ark",
    model: "fake-model",
    complete: async function () {
      return "这是一段模型回答。"
    },
  }
}

async function withApp(fn) {
  const app = createApp({
    dbPath: ":memory:",
    learningRoot: tempLearning(),
    // 不加载工程仓自带的题库：测试只看临时 learning 仓里的两个单元
    localBank: null,
    llmClient: makeFakeClient(),
    env: {},
  })
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

test("POST /api/units/:id/judge 卡片通过时 verdict 是 pass", async function () {
  await withApp(async function (base) {
    const r = await postJson(base, "/api/units/db:mvcc/judge", {})
    assert.equal(r.status, 200)
    assert.equal(r.body.verdict, "pass", r.body.message)
    assert.equal(r.body.passed, 2)
    assert.equal(r.body.total, 2)
    assert.equal(r.body.failed, null)
    assert.equal(r.body.unit.id, "db:mvcc")
    assert.equal(r.body.unit.kind, "card")
    assert.equal(r.body.unit.track, "database")
    assert.ok(r.body.stdout.indexOf("全部断言通过") !== -1, r.body.stdout)
    assert.deepEqual(Object.keys(r.body).sort(), [
      "durationMs", "failed", "message", "passed", "stderr",
      "total", "unit", "verdict", "stdout",
    ].sort())
  })
})

test("POST /api/units/:id/judge 断言不过时是 wrong_answer，不是 500", async function () {
  await withApp(async function (base, app) {
    // 把脚本的断言改坏
    const scriptPath = path.join(app.learning.root, "units", "database", "mvcc.assert.py")
    fs.writeFileSync(
      scriptPath,
      CARD_SCRIPT.replace("OK demo ran", "绝对不会出现的字符串"),
      "utf8"
    )
    const r = await postJson(base, "/api/units/db:mvcc/judge", {})
    assert.equal(r.status, 200)
    assert.equal(r.body.verdict, "wrong_answer", r.body.message)
    assert.ok(r.body.failed, "失败时必须给出 failed")
    assert.equal(r.body.failed.type, "exit")
  })
})

test("POST /api/units/:id/judge 断言脚本缺失时是 compile_error（附中文说明），不是 500", async function () {
  await withApp(async function (base, app) {
    fs.rmSync(path.join(app.learning.root, "units", "database", "mvcc.assert.py"))
    const r = await postJson(base, "/api/units/db:mvcc/judge", {})
    assert.equal(r.status, 200)
    assert.equal(r.body.verdict, "compile_error", r.body.message)
    assert.ok(r.body.message.indexOf("断言脚本不存在") !== -1, r.body.message)
  })
})

test("POST /api/units/:id/judge 题目类：代码为空返回 400", async function () {
  await withApp(async function (base) {
    const r = await postJson(base, "/api/units/algo:lc-00007/judge", { code: "   " })
    assert.equal(r.status, 400)
    assert.equal(r.body.error, "代码不能为空")
  })
})

test("POST /api/units/:id/judge 题目类：语言不支持返回 400", async function () {
  await withApp(async function (base) {
    const r = await postJson(base, "/api/units/algo:lc-00007/judge", {
      code: "print(1)\n",
      language: "rust",
    })
    assert.equal(r.status, 400)
    assert.ok(r.body.error.indexOf("不支持的语言") !== -1, r.body.error)
    assert.deepEqual(r.body.supported, ["cpp", "python"])
  })
})

test("POST /api/units/:id/judge 题目类：Python 代码通过时 verdict 是 pass", async function () {
  await withApp(async function (base) {
    const r = await postJson(base, "/api/units/algo:lc-00007/judge", {
      code: "a, b = map(int, input().split())\nprint(a + b)\n",
      language: "python",
    })
    assert.equal(r.status, 200)
    assert.equal(r.body.verdict, "pass", r.body.message)
    assert.equal(r.body.passed, 1)
    assert.equal(r.body.total, 1)
  })
})

test("POST /api/units/:id/judge 单元不存在返回 404", async function () {
  await withApp(async function (base) {
    const r = await postJson(base, "/api/units/db:根本没有这张卡/judge", {})
    assert.equal(r.status, 404)
    assert.equal(r.body.error, "单元不存在")
    assert.equal(r.body.id, "db:根本没有这张卡")
  })
})

test("POST /api/units/:id/complete 把单元进度置为 done", async function () {
  await withApp(async function (base, app) {
    const r = await postJson(base, "/api/units/db:mvcc/complete", {})
    assert.equal(r.status, 200)
    assert.equal(r.body.unitId, "db:mvcc")
    assert.equal(r.body.progress, "done")
    assert.ok(r.body.lastAt.length > 0)
    const rows = app.store.listUnitProgress()
    const hit = rows.find(function (x) { return x.unit_id === "db:mvcc" })
    assert.ok(hit, "unit_progress 表里要有这一行")
    assert.equal(hit.progress, "done")
  })
})

test("POST /api/units/:id/complete 单元不存在返回 404", async function () {
  await withApp(async function (base) {
    const r = await postJson(base, "/api/units/db:根本没有这张卡/complete", {})
    assert.equal(r.status, 404)
    assert.equal(r.body.error, "单元不存在")
  })
})

test("POST /api/units/:id/ask 返回 {text, model}", async function () {
  await withApp(async function (base) {
    const r = await postJson(base, "/api/units/db:mvcc/ask", { level: "思路" })
    assert.equal(r.status, 200)
    assert.equal(typeof r.body.text, "string")
    assert.ok(r.body.text.length > 0)
    assert.equal(r.body.model, "fake-model")
    assert.deepEqual(Object.keys(r.body).sort(), ["model", "text"])
  })
})

test("POST /api/units/:id/ask 单元不存在返回 404", async function () {
  await withApp(async function (base) {
    const r = await postJson(base, "/api/units/db:根本没有这张卡/ask", {})
    assert.equal(r.status, 404)
    assert.equal(r.body.error, "单元不存在")
  })
})

test("GET /api/env 返回环境自检结果", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/env")
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.deepEqual(Object.keys(body).sort(), ["compiler", "notes", "numpy", "python"])
    assert.equal(typeof body.python.available, "boolean")
    assert.equal(typeof body.compiler.available, "boolean")
    assert.equal(typeof body.numpy.available, "boolean")
    assert.ok(Array.isArray(body.notes))
    assert.ok(body.notes.length > 0)
    body.notes.forEach(function (n) {
      assert.equal(typeof n, "string")
      assert.ok(n.length > 0)
    })
  })
})

test("GET /api/env 的 notes 与探测结果一致（缺 numpy 要点名缺什么）", async function () {
  // 上一条只断言 notes 是非空字符串数组 —— 把 notes 写成固定文案「运行环境正常」也照样绿。
  // 这条把 notes 与三个 available 标志对上：缺什么就必须点名什么。
  await withApp(async function (base) {
    const body = await (await fetch(base + "/api/env")).json()
    const text = body.notes.join(" ")
    if (body.numpy.available === false) {
      assert.ok(text.indexOf("numpy") !== -1, "缺 numpy 时要点名：" + text)
      assert.ok(text.indexOf("环境正常") === -1, "缺东西时不能说「环境正常」：" + text)
    }
    if (body.compiler.available === false) {
      assert.ok(text.indexOf("g++") !== -1, "缺编译器时要点名：" + text)
    }
    if (body.python.available === false) {
      assert.ok(text.indexOf("Python") !== -1 || text.indexOf("python") !== -1, "缺 Python 时要点名：" + text)
    }
    if (body.numpy.available && body.compiler.available && body.python.available) {
      assert.ok(text.indexOf("正常") !== -1, "全都有时说「运行环境正常」：" + text)
    }
  })
})
