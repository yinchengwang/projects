"use strict"
/**
 * p2-e2e-accept.test.js —— 契约 §12 的 P2 验收清单（端到端闸门）
 *
 * 这一套是「验收项在真链路上过一遍」：
 *   GET /api/units/:id  →  statement_html  →  渲染左栏右栏  →  POST judge  →  verdict
 *
 * 全程不联网（G12）：只有 127.0.0.1 上的自己，LLM 注入假客户端。
 * 判题要真的跑 Python，所以本机没有 Python 时**明确 skip 并写清原因**，
 * 不静默跳过（契约 §12 的「环境缺失就明确 skip 并给出原因」）。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const stub = require("./helpers/dom-stub.js")

// 页面脚本要 document 才能拼 DOM
const dom = stub.createDocument()
global.document = dom.document

const { createApp } = require("../server/app.js")
const { detectPython } = require("../judge/sandbox.js")
const unitPage = require("../web/pages/unit.js")

/** 本机有没有 Python 解释器——没有就跑不了判题 */
const PY = detectPython(null)

/** 契约 §3.5 形状的断言脚本（demo 回显 + 抓字节解码 + 失败回显 stderr） */
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
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "p2-e2e-"))

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

/** 列出目录下的全部文件（相对路径，排序）——用来验「零污染」 */
function listFiles(root) {
  const out = []
  function walk(dir, prefix) {
    fs.readdirSync(dir, { withFileTypes: true }).forEach(function (e) {
      const rel = prefix ? prefix + "/" + e.name : e.name
      if (e.isDirectory()) walk(path.join(dir, e.name), rel)
      else out.push(rel)
    })
  }
  walk(root, "")
  return out.sort()
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

/** 起一次服务，跑完就关 */
async function withApp(fn) {
  const learningRoot = tempLearning()
  const app = createApp({
    dbPath: ":memory:",
    learningRoot: learningRoot,
    localBank: null,
    llmClient: makeFakeClient(),
    env: {},
  })
  const port = await app.listen(0)
  const base = "http://127.0.0.1:" + port
  try {
    await fn(base, app, learningRoot)
  } finally {
    await app.close()
    fs.rmSync(learningRoot, { recursive: true, force: true })
  }
}

async function getJson(base, urlPath) {
  const res = await fetch(base + urlPath)
  return { status: res.status, body: await res.json() }
}

async function postJson(base, urlPath, body) {
  const res = await fetch(base + urlPath, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body || {}),
  })
  return { status: res.status, body: await res.json() }
}

/** 在树里找出所有 class 里含有这个词的节点 */
function findAllByClassToken(node, token) {
  const out = []
  function walk(n) {
    if (!n) return
    const cls = String(n.className || "").split(/\s+/)
    if (cls.indexOf(token) !== -1) out.push(n)
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
}

/** 把树里所有文本拼起来 */
function collectText(node) {
  let out = ""
  function walk(n) {
    if (!n) return
    if (n.textContent) out += " " + n.textContent
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
}

/** 从 statement_html 里抠出第一段能渲染出来的文字（用来对照左栏渲染结果） */
function firstTextRun(html) {
  const parts = String(html || "").split(/<[^>]*>/)
  for (let i = 0; i < parts.length; i += 1) {
    const t = parts[i]
      .replace(/&nbsp;/g, " ")
      .replace(/&lt;/g, "<")
      .replace(/&gt;/g, ">")
      .replace(/&quot;/g, '"')
      .replace(/&#39;/g, "'")
      .replace(/&amp;/g, "&")
      .trim()
    if (t.length >= 2) return t
  }
  return ""
}

test("验收：GET /api/units/:id 给出 statement_html，左栏把它渲染成讲解", async function () {
  await withApp(async function (base) {
    const r = await getJson(base, "/api/units/db:mvcc")
    assert.strictEqual(r.status, 200)
    const unit = r.body.unit || {}
    const html = String(r.body.statement_html || "")
    assert.strictEqual(unit.id, "db:mvcc", "单元 id 要符合 <track>:<stem>")
    assert.strictEqual(unit.kind, "card")
    assert.ok(html.length > 0, "契约 §8.1：GET /api/units/:id 必须给 statement_html")

    const root = unitPage.renderUnit(unitPage.buildUnitView(unit, html), unit, html)
    const readCol = findAllByClassToken(root, "unit-col-read")[0]
    assert.ok(readCol, "要有左栏（读）")
    const run = firstTextRun(html)
    assert.ok(run.length > 0, "statement_html 里得有能渲染的文字")
    assert.ok(collectText(readCol).indexOf(run) !== -1, "左栏要渲染出 statement_html 的正文")
  })
})

test("验收：数据库卡右栏能跑 demo，判题结果显示中文结论、条数与 demo 输出", async function (t) {
  if (!PY) {
    t.skip("本机没有 Python 解释器（python3 / python / py -3 都试过了），判题类验收项跳过")
    return
  }
  await withApp(async function (base) {
    const r = await postJson(base, "/api/units/db:mvcc/judge", {})
    assert.strictEqual(r.status, 200, "判题失败不该是 500")
    assert.strictEqual(r.body.verdict, "pass", r.body.message)
    assert.strictEqual(r.body.passed, r.body.total, "全部断言都该过")
    assert.ok(r.body.total >= 2, "卡类至少有 exit 与 contains 两条断言")

    const g = await getJson(base, "/api/units/db:mvcc")
    const unit = g.body.unit || {}
    const html = String(g.body.statement_html || "")
    const root = unitPage.renderUnit(unitPage.buildUnitView(unit, html), unit, html)
    const doCol = findAllByClassToken(root, "unit-col-do")[0]
    assert.ok(stub.findById(doCol, "unit-run"), "右栏要有「运行断言」按钮")
    const box = stub.findById(doCol, "unit-result")
    assert.ok(box, "右栏要有判题结果区")
    unitPage.paintResult(box, r.body)
    const text = collectText(box)
    assert.ok(text.indexOf("通过") !== -1, "结果区要有中文结论：" + text)
    assert.ok(text.indexOf("demo 输出") !== -1, "结果区要显示 demo 输出：" + text)
    assert.ok(text.indexOf(r.body.passed + "/" + r.body.total) !== -1, "结果区要显示通过条数：" + text)
  })
})

test("验收：算法题左栏是题面，右栏是代码编辑区 + 判题结果区", async function () {
  await withApp(async function (base) {
    const g = await getJson(base, "/api/units/algo:lc-00007")
    assert.strictEqual(g.status, 200)
    const unit = g.body.unit || {}
    const html = String(g.body.statement_html || "")
    assert.strictEqual(unit.kind, "problem")
    assert.ok(html.length > 0, "契约 §8.1：题面也要给 statement_html")

    const root = unitPage.renderUnit(unitPage.buildUnitView(unit, html), unit, html)
    const readCol = findAllByClassToken(root, "unit-col-read")[0]
    const doCol = findAllByClassToken(root, "unit-col-do")[0]
    assert.ok(readCol && doCol, "要有左栏与右栏")
    const readText = collectText(readCol)
    const run = firstTextRun(html)
    assert.ok(run.length > 0, "题面里得有能渲染的文字")
    assert.ok(readText.indexOf(run) !== -1, "左栏要渲染出题面：" + readText)

    const editor = stub.findById(doCol, "unit-code")
    assert.ok(editor, "右栏要有代码编辑区")
    assert.strictEqual(editor.tagName, "textarea", "代码编辑区是 textarea")
    assert.ok(stub.findById(doCol, "unit-judge"), "右栏要有「提交判题」按钮")
    assert.ok(stub.findById(doCol, "unit-result"), "右栏要有判题结果区")

    // 判题入口是通的：代码为空返回 400「代码不能为空」，不是 500
    const bad = await postJson(base, "/api/units/algo:lc-00007/judge", { code: "   " })
    assert.strictEqual(bad.status, 400)
    assert.strictEqual(bad.body.error, "代码不能为空")
  })
})

test("验收：断言不过时 verdict 是 wrong_answer，不是 500", async function (t) {
  if (!PY) {
    t.skip("本机没有 Python 解释器（python3 / python / py -3 都试过了），判题类验收项跳过")
    return
  }
  await withApp(async function (base, app, learningRoot) {
    const scriptPath = path.join(learningRoot, "units", "database", "mvcc.assert.py")
    fs.writeFileSync(
      scriptPath,
      CARD_SCRIPT.replace("OK demo ran", "绝对不会出现的字符串"),
      "utf8"
    )
    const r = await postJson(base, "/api/units/db:mvcc/judge", {})
    assert.strictEqual(r.status, 200, "断言不过不是 500")
    assert.strictEqual(r.body.verdict, "wrong_answer", r.body.message)
    assert.ok(r.body.failed, "失败时必须给出 failed")
    assert.ok(r.body.message.indexOf("断言失败") !== -1, "中文说明要说清哪条没过：" + r.body.message)
  })
})

test("验收：断言脚本缺失时 verdict 是 compile_error（附中文说明），不是 500", async function () {
  await withApp(async function (base, app, learningRoot) {
    fs.rmSync(path.join(learningRoot, "units", "database", "mvcc.assert.py"))
    const r = await postJson(base, "/api/units/db:mvcc/judge", {})
    assert.strictEqual(r.status, 200, "脚本缺失不是 500")
    assert.strictEqual(r.body.verdict, "compile_error", r.body.message)
    assert.ok(r.body.message.indexOf("断言脚本不存在") !== -1, r.body.message)
  })
})

test("验收：判完之后 learning 仓零污染（文件清单前后完全一致）", async function () {
  await withApp(async function (base, app, learningRoot) {
    const before = listFiles(learningRoot)
    await postJson(base, "/api/units/db:mvcc/judge", {})
    const after = listFiles(learningRoot)
    assert.deepStrictEqual(after, before, "判题不许在 learning 仓里留下任何文件")
  })
})

test("验收：单元不存在时 404「单元不存在」", async function () {
  await withApp(async function (base) {
    const r = await getJson(base, "/api/units/db:notexist")
    assert.strictEqual(r.status, 404)
    assert.strictEqual(r.body.error, "单元不存在")
    assert.strictEqual(r.body.id, "db:notexist")
  })
})

test("验收：判题响应的九个字段一个都不少（契约 §8.5）", async function () {
  await withApp(async function (base) {
    const r = await postJson(base, "/api/units/db:mvcc/judge", {})
    assert.strictEqual(r.status, 200)
    ;["verdict", "passed", "total", "durationMs", "message", "failed", "stdout", "stderr", "unit"].forEach(
      function (key) {
        assert.ok(Object.prototype.hasOwnProperty.call(r.body, key), "响应里少了字段：" + key)
      }
    )
    assert.strictEqual(r.body.unit.id, "db:mvcc")
    assert.strictEqual(r.body.unit.kind, "card")
    assert.strictEqual(r.body.unit.track, "database")
    assert.strictEqual(typeof r.body.durationMs, "number")
    assert.strictEqual(typeof r.body.stdout, "string")
    assert.strictEqual(typeof r.body.stderr, "string")
  })
})
