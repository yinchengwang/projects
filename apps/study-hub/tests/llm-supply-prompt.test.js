"use strict"
/**
 * llm-supply-prompt.test.js —— 按轨的模板表与四个供给场景（契约 §7.2）
 *
 * 关键纪律：**轨道模板是数据，不是代码**。
 * 这套测试专门盯两件事：
 *   1. 模板表覆盖三条轨 × 四个场景，渲染后不残留占位符；
 *   2. prompts.js 源码里**不许**出现按轨的硬分支（`=== "database"` 之类）。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const {
  SUPPLY_SCENARIOS,
  TRACK_TEMPLATES,
  renderTemplate,
  buildSupplyPrompt,
  hasSupplyTemplate,
  parseSupplyResponse,
} = require("../core/llm/prompts.js")

/** 一份能填满所有占位符的上下文 */
const CONTEXT = {
  title: "MVCC（多版本并发控制）",
  difficulty: "medium",
  topics: "事务、并发",
  statement: "多版本并发控制是一种让读不阻塞写的技术。",
  demo: "def begin():\n    return 1\n",
  readme: "# MVCC\n",
  notes: "## 要点\n版本链\n",
  question: "这个机制在什么情况下会失效？",
}

test("四个供给场景都在场景表里", function () {
  assert.deepStrictEqual(SUPPLY_SCENARIOS, ["generate-unit", "generate-assert", "write-explain", "deepen"])
})

test("模板表覆盖三条轨", function () {
  assert.deepStrictEqual(Object.keys(TRACK_TEMPLATES).sort(), ["ai", "algorithm", "database"])
  ;["algorithm", "database", "ai"].forEach(function (track) {
    assert.ok(TRACK_TEMPLATES[track].label, track + " 轨要有中文标签")
    assert.ok(TRACK_TEMPLATES[track].defaultQuestion, track + " 轨要有默认的深挖问题")
  })
})

test("每条轨的每个场景都有模板，渲染后不残留占位符", function () {
  const tracks = Object.keys(TRACK_TEMPLATES)
  let checked = 0
  tracks.forEach(function (track) {
    const scenarios = TRACK_TEMPLATES[track].scenarios
    Object.keys(scenarios).forEach(function (scenario) {
      const prompt = buildSupplyPrompt(scenario, track, CONTEXT)
      assert.ok(prompt.system.length > 0, track + "/" + scenario + " 的 system 不能为空")
      assert.ok(prompt.user.length > 0, track + "/" + scenario + " 的 user 不能为空")
      assert.strictEqual(
        prompt.user.indexOf("{{"),
        -1,
        track + "/" + scenario + " 的 user 还残留占位符：" + prompt.user.slice(0, 120)
      )
      assert.strictEqual(
        prompt.system.indexOf("{{"),
        -1,
        track + "/" + scenario + " 的 system 还残留占位符"
      )
      assert.ok(["json", "text"].indexOf(prompt.response) !== -1, "response 只能是 json 或 text")
      checked += 1
    })
  })
  assert.ok(checked >= 9, "模板条数不该少于 9 条，实际 " + checked)
})

test("模板表是数据，不是代码：源码里不许有按轨的硬分支", function () {
  const text = fs.readFileSync(path.join(__dirname, "..", "core", "llm", "prompts.js"), "utf8")
  ;[
    '=== "database"',
    '=== "ai"',
    '=== "algorithm"',
    "=== 'database'",
    "=== 'ai'",
    "=== 'algorithm'",
    '!== "database"',
    '!== "ai"',
  ].forEach(function (needle) {
    assert.strictEqual(text.indexOf(needle), -1, "prompts.js 里出现了按轨的硬分支：" + needle)
  })
})

test("未知轨道 / 未知场景抛中文错", function () {
  assert.throws(
    function () {
      buildSupplyPrompt("write-explain", "frontend", CONTEXT)
    },
    /没有这条轨的模板/
  )
  assert.throws(
    function () {
      buildSupplyPrompt("generate-assert", "algorithm", CONTEXT)
    },
    /没有场景/
  )
  assert.strictEqual(hasSupplyTemplate("database", "generate-assert"), true)
  assert.strictEqual(hasSupplyTemplate("algorithm", "generate-assert"), false)
})

test("renderTemplate 填空：缺失的占位符换成空串", function () {
  assert.strictEqual(renderTemplate("甲{{a}}乙{{b}}丙", { a: "1" }), "甲1乙丙")
  assert.strictEqual(renderTemplate("{{x}}", {}), "")
})

test("generate-unit：解析出题面、用例与解法", function () {
  const raw = JSON.stringify({
    title: "两数之和",
    statement: "给定数组与目标值。",
    difficulty: "easy",
    topics: ["哈希表"],
    cases: [{ input: "2 7 11 15", expect: "0 1" }],
    solution: "print(1)",
    solutionLang: "python",
  })
  const parsed = parseSupplyResponse(raw, "generate-unit")
  assert.strictEqual(parsed.ok, true)
  assert.strictEqual(parsed.unit.title, "两数之和")
  assert.strictEqual(parsed.unit.cases.length, 1)
  assert.strictEqual(parsed.unit.cases[0].expect, "0 1")
  assert.strictEqual(parsed.unit.solutionLang, "python")
  assert.deepStrictEqual(parsed.unit.topics, ["哈希表"])
})

test("generate-unit：缺用例要被拒", function () {
  const raw = JSON.stringify({ title: "t", statement: "s", solution: "x" })
  const parsed = parseSupplyResponse(raw, "generate-unit")
  assert.strictEqual(parsed.ok, false)
  assert.ok(parsed.reason.indexOf("缺测试用例") !== -1, "理由要说清缺什么：" + parsed.reason)
})

test("generate-assert：脚本必须照统一模板（subprocess 跑 demo + STUDY_HUB_DEMO + 退出码）", function () {
  const good = JSON.stringify({
    assert_script:
      "import os\n" +
      "import shutil\n" +
      "import subprocess\n" +
      "import sys\n" +
      "import tempfile\n" +
      "DEMO = os.environ.get('STUDY_HUB_DEMO') or os.path.join(\n" +
      "    os.path.dirname(os.path.abspath(__file__)), 'main.py')\n" +
      "def decode(raw):\n" +
      "    for enc in ('utf-8', 'gbk'):\n" +
      "        try:\n" +
      "            return raw.decode(enc)\n" +
      "        except UnicodeDecodeError:\n" +
      "            continue\n" +
      "    return raw.decode('utf-8', errors='replace')\n" +
      "def main():\n" +
      "    sandbox = tempfile.mkdtemp(prefix='study-hub-assert-')\n" +
      "    try:\n" +
      "        target = os.path.join(sandbox, os.path.basename(DEMO))\n" +
      "        shutil.copy2(DEMO, target)\n" +
      "        proc = subprocess.run([sys.executable, target], capture_output=True, timeout=120)\n" +
      "    finally:\n" +
      "        shutil.rmtree(sandbox, ignore_errors=True)\n" +
      "    if '[[OK]] 表已创建' not in decode(proc.stdout):\n" +
      "        print('断言失败：表没建出来')\n" +
      "        return 1\n" +
      "    return 0\n" +
      "if __name__ == '__main__':\n" +
      "    sys.exit(main())\n",
    topics: ["事务"],
  })
  const parsed = parseSupplyResponse(good, "generate-assert")
  assert.strictEqual(parsed.ok, true, parsed.reason)
  assert.ok(parsed.script.indexOf("STUDY_HUB_DEMO") !== -1)
  assert.deepStrictEqual(parsed.topics, ["事务"])

  const noEnv = JSON.stringify({ assert_script: "import main\nimport sys\nsys.exit(0)\n" })
  const bad1 = parseSupplyResponse(noEnv, "generate-assert")
  assert.strictEqual(bad1.ok, false)
  assert.ok(bad1.reason.indexOf("STUDY_HUB_DEMO") !== -1, bad1.reason)

  const noSubprocess = JSON.stringify({
    assert_script: "import os\nimport sys\nDEMO = os.environ.get('STUDY_HUB_DEMO')\nsys.exit(0)\n",
  })
  const bad2 = parseSupplyResponse(noSubprocess, "generate-assert")
  assert.strictEqual(bad2.ok, false)
  assert.ok(bad2.reason.indexOf("subprocess") !== -1, bad2.reason)

  const noExit = JSON.stringify({
    assert_script: "import os\nimport subprocess\nDEMO = os.environ.get('STUDY_HUB_DEMO')\nprint('hi')\n",
  })
  const bad3 = parseSupplyResponse(noExit, "generate-assert")
  assert.strictEqual(bad3.ok, false)
  assert.ok(bad3.reason.indexOf("退出码") !== -1, bad3.reason)
})

test("generate-assert：不许就地跑 demo，必须复制进临时目录（契约 §3.5 第②条）", function () {
  // 缺 tempfile.mkdtemp 的脚本：就地执行 demo，第二次跑必然因表已存在而失败
  const noSandbox = JSON.stringify({
    assert_script:
      "import os\n" +
      "import subprocess\n" +
      "import sys\n" +
      "DEMO = os.environ.get('STUDY_HUB_DEMO')\n" +
      "def main():\n" +
      "    proc = subprocess.run([sys.executable, DEMO], capture_output=True, timeout=120)\n" +
      "    return 0 if proc.returncode == 0 else 1\n" +
      "sys.exit(main())\n",
  })
  const bad = parseSupplyResponse(noSandbox, "generate-assert")
  assert.strictEqual(bad.ok, false)
  assert.ok(bad.reason.indexOf("临时目录") !== -1, bad.reason)
})

test("generate-assert：不许写死 text=True（契约 §3.5 第③条）", function () {
  // text=True 会用本机 locale 编码去解码 demo 的输出；本机是 GBK，解出来是乱码，needle 必然匹配不上
  const withText = JSON.stringify({
    assert_script:
      "import os\n" +
      "import shutil\n" +
      "import subprocess\n" +
      "import sys\n" +
      "import tempfile\n" +
      "DEMO = os.environ.get('STUDY_HUB_DEMO')\n" +
      "def main():\n" +
      "    sandbox = tempfile.mkdtemp(prefix='study-hub-assert-')\n" +
      "    try:\n" +
      "        proc = subprocess.run([sys.executable, DEMO], capture_output=True, text=True, timeout=120)\n" +
      "    finally:\n" +
      "        shutil.rmtree(sandbox, ignore_errors=True)\n" +
      "    return 0\n" +
      "sys.exit(main())\n",
  })
  const bad = parseSupplyResponse(withText, "generate-assert")
  assert.strictEqual(bad.ok, false)
  assert.ok(bad.reason.indexOf("text=True") !== -1, bad.reason)
})

test("write-explain 与 deepen：纯文本", function () {
  const parsed = parseSupplyResponse("# 讲解\n\n正文", "write-explain")
  assert.strictEqual(parsed.ok, true)
  assert.strictEqual(parsed.text, "# 讲解\n\n正文")

  const deep = parseSupplyResponse("深挖正文", "deepen")
  assert.strictEqual(deep.ok, true)
  assert.strictEqual(deep.text, "深挖正文")
})

test("空回复与非法 JSON 都要给中文理由", function () {
  const empty = parseSupplyResponse("", "write-explain")
  assert.strictEqual(empty.ok, false)
  assert.ok(empty.reason.indexOf("没有返回内容") !== -1, empty.reason)

  const broken = parseSupplyResponse("这不是 JSON", "generate-assert")
  assert.strictEqual(broken.ok, false)
  assert.ok(broken.reason.indexOf("合法 JSON") !== -1, broken.reason)
})
