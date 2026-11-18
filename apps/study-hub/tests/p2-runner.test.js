"use strict"
/**
 * p2-runner.test.js —— judgeUnit：一条路径判两种题
 *
 * kind: problem 走用户提交的代码，kind: card 走单元自带的断言脚本，
 * 两者共用同一套「跑 → 收集 → 逐条校验 → verdict」的核心。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const runner = require("../judge/runner.js")
const { VERDICTS } = runner

/** 造一个假的 learning 仓根 */
function makeLearningRoot(files) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "p2-lroot-"))
  Object.keys(files).forEach(function (rel) {
    const abs = path.join(root, rel)
    fs.mkdirSync(path.dirname(abs), { recursive: true })
    fs.writeFileSync(abs, files[rel], "utf8")
  })
  return root
}

/** 列出目录下的全部相对路径（用来验「零污染」） */
function listFiles(root) {
  const out = []
  function walk(dir, prefix) {
    const entries = fs.readdirSync(dir, { withFileTypes: true })
    entries.forEach(function (e) {
      const rel = prefix ? prefix + "/" + e.name : e.name
      if (e.isDirectory()) walk(path.join(dir, e.name), rel)
      else out.push(rel)
    })
  }
  walk(root, "")
  return out.sort()
}

/**
 * 契约 §3.5 的断言脚本模板（测试里用简化版，行为一致）。
 *
 * 三条硬性约定一条都不能少：
 *   ① demo 用 subprocess 跑，不 import；
 *   ② 脚本自己把 demo 复制进 tempfile.mkdtemp() 再跑，跑完 rmtree；
 *   ③ 不写死 text=True——抓字节，再按 utf-8 → gbk 依次试解码。
 * 少了 ③，本机（Windows 中文环境，sys.stdout.encoding == "gbk"）会把
 * demo 的输出解成乱码，needle 必然匹配不上，verdict 被误判成 wrong_answer。
 */
const SCRIPT_TEMPLATE = [
  "import os, shutil, subprocess, sys, tempfile",
  "DEMO = os.environ.get('STUDY_HUB_DEMO') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'main.py')",
  "CHECKS = [('demo 跑起来了', '__NEEDLE__')]",
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
  "        failed.append('demo 的 stderr 末尾：' + decode(proc.stderr)[-500:])",
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

const DEMO_OK = "print('__NEEDLE__ demo ran')\n"

/** demo 直接往 stdout 的字节流里写 GBK 字节：模拟「本机 locale 是 GBK」的情形 */
const DEMO_GBK = [
  "import sys",
  "sys.stdout.buffer.write('演示输出：你好'.encode('gbk'))",
  "",
].join("\n")

const CARD_UNIT = {
  id: "db:testcard",
  track: "database",
  kind: "card",
  title: "测试卡",
  entry: {
    demo: "scaffold/grokking/testcard/main.py",
    reference: null,
    graded: "script",
    script: "units/database/testcard.assert.py",
    language: "python",
  },
  asserts: [],
  path: "units/database/testcard.md",
}

function cardRoot(needle, extra) {
  const files = {
    "scaffold/grokking/testcard/main.py": DEMO_OK.replace("__NEEDLE__", needle),
    "units/database/testcard.assert.py": SCRIPT_TEMPLATE.replace("__NEEDLE__", needle),
  }
  if (extra) Object.keys(extra).forEach(function (k) { files[k] = extra[k] })
  return makeLearningRoot(files)
}

test("judgeUnit 已导出，SCRIPT_PASS_MARK 与契约一致", function () {
  assert.strictEqual(typeof runner.judgeUnit, "function")
  assert.strictEqual(runner.SCRIPT_PASS_MARK, "全部断言通过")
  assert.strictEqual(runner.SCRIPT_TIMEOUT_MS, 30000)
})

test("卡类：断言全过 → pass", async function () {
  const root = cardRoot("OK")
  try {
    const r = await runner.judgeUnit({ unit: CARD_UNIT, learningRoot: root })
    assert.strictEqual(r.verdict, "pass", r.message)
    assert.strictEqual(r.passedCases, 2, "隐含断言两条：exit 0 + contains 成功标记")
    assert.strictEqual(r.totalCases, 2)
    assert.strictEqual(r.failedCase, null)
    assert.ok(r.stdout.indexOf("demo 输出开始") !== -1, "脚本必须回显 demo 输出")
    assert.ok(r.message.indexOf("通过") !== -1, r.message)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("卡类：demo 输出是 GBK 字节时，断言仍能匹配到 needle（不许写死 text=True）", async function () {
  // 本机 sys.stdout.encoding 是 gbk。若脚本用 text=True，demo 的字节会被
  // locale 编码解码，utf-8 机器上解成乱码，needle 必然匹配不上。
  const needle = "演示输出：你好"
  const root = cardRoot(needle, { "scaffold/grokking/testcard/main.py": DEMO_GBK })
  try {
    const r = await runner.judgeUnit({ unit: CARD_UNIT, learningRoot: root })
    assert.strictEqual(r.verdict, "pass", r.message)
    assert.strictEqual(r.passedCases, 2)
    assert.ok(r.stdout.indexOf(needle) !== -1, "回显里必须能看到解码后的中文：" + r.stdout)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("卡类：断言不通过 → wrong_answer，message 带中文说明", async function () {
  const root = cardRoot("OK")
  try {
    // 把脚本里的 needle 换成 demo 里不会出现的东西
    const scriptPath = path.join(root, "units/database/testcard.assert.py")
    fs.writeFileSync(
      scriptPath,
      SCRIPT_TEMPLATE.replace("__NEEDLE__", "绝不会出现的字符串"),
      "utf8"
    )
    const r = await runner.judgeUnit({ unit: CARD_UNIT, learningRoot: root })
    assert.strictEqual(r.verdict, "wrong_answer", r.message)
    assert.strictEqual(r.passedCases, 0)
    assert.ok(r.message.indexOf("断言失败") !== -1, r.message)
    assert.ok(r.failedCase, "失败时必须给出 failedCase")
    assert.strictEqual(r.failedCase.type, "exit")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("卡类：断言脚本不存在 → compile_error（不是 500，不是抛异常）", async function () {
  const root = cardRoot("OK")
  try {
    fs.rmSync(path.join(root, "units/database/testcard.assert.py"))
    const r = await runner.judgeUnit({ unit: CARD_UNIT, learningRoot: root })
    assert.strictEqual(r.verdict, "compile_error", r.message)
    assert.ok(r.message.indexOf("断言脚本不存在") !== -1, r.message)
    assert.ok(r.message.indexOf("testcard.assert.py") !== -1, r.message)
    // totalCases 要是「断言集的大小」而不是 0 —— 断言集是已知的（隐含两条 + 单元自带），
    // 只是还没跑。给 0 会让 UI 显示「0/0」，与其它 compile_error 分支不一致。
    assert.strictEqual(r.totalCases, 2, "隐含两条断言应当算进 totalCases：" + String(r.totalCases))
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("卡类：没连上 learning 仓 → compile_error", async function () {
  const r = await runner.judgeUnit({ unit: CARD_UNIT, learningRoot: null })
  assert.strictEqual(r.verdict, "compile_error", r.message)
  assert.ok(r.message.indexOf("learning 仓") !== -1, r.message)
})

test("卡类：脚本有语法错 → compile_error", async function () {
  const root = cardRoot("OK", { "units/database/testcard.assert.py": "def broken(:\n" })
  try {
    const r = await runner.judgeUnit({ unit: CARD_UNIT, learningRoot: root })
    assert.strictEqual(r.verdict, "compile_error", r.message)
    assert.ok(r.message.indexOf("语法错误") !== -1, r.message)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("卡类：脚本缺 numpy → runtime_error，且文案点明「环境缺失」", async function () {
  // 这是 P2 最重要的一条 verdict 分支：AI 轨的 13 张卡 + clustering 都依赖 numpy。
  // 缺 numpy 时 demo 抛 ModuleNotFoundError，靠 §3.5 的 stderr 转发才判得出来 ——
  // 若映射错成 wrong_answer，用户会以为「这张卡我答错了」，而不是「我环境缺东西」。
  const py = [
    "import sys",
    'print("ModuleNotFoundError: No module named numpy", file=sys.stderr)',
    "sys.exit(1)",
    "",
  ].join("\n")
  const root = cardRoot("OK", { "units/database/testcard.assert.py": py })
  try {
    const r = await runner.judgeUnit({ unit: CARD_UNIT, learningRoot: root })
    assert.strictEqual(r.verdict, "runtime_error", r.message)
    assert.ok(r.message.indexOf("环境缺失") !== -1, "文案要点明是环境问题：" + r.message)
    assert.ok(r.message.indexOf("numpy") !== -1, "要点名缺的是什么：" + r.message)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("卡类：隐含断言是 exit 0 + contains 成功标记（两条）", async function () {
  // 契约 §7.4 冻结：判卡时自动生成这两条隐含断言。
  // 把 exit 那条换成别的，脚本退出码非 0 也能混过去 —— 这里用「脚本 exit 1 但打印了成功标记」
  // 的夹具把它钉住。
  const py = [
    "print('全部断言通过（1 条）')",
    "raise SystemExit(1)",
    "",
  ].join("\n")
  const root = cardRoot("OK", { "units/database/testcard.assert.py": py })
  try {
    const r = await runner.judgeUnit({ unit: CARD_UNIT, learningRoot: root })
    assert.strictEqual(
      r.verdict,
      "wrong_answer",
      "脚本 exit 1 即使打印了成功标记也不能判 pass：" + r.message
    )
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("卡类：脚本自己崩了 → runtime_error", async function () {
  const root = cardRoot("OK", { "units/database/testcard.assert.py": "raise RuntimeError('boom')\n" })
  try {
    const r = await runner.judgeUnit({ unit: CARD_UNIT, learningRoot: root })
    assert.strictEqual(r.verdict, "runtime_error", r.message)
    assert.ok(r.message.indexOf("崩了") !== -1, r.message)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("卡类：判完之后 learning 仓工作树零污染（没有多出 .db 文件）", async function () {
  // demo 会在自己目录里写 .db，模拟真实卡的行为
  const demoWithDb = [
    "import os",
    "open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'demo.db'), 'w').write('x')",
    "print('OK demo ran')",
    "",
  ].join("\n")
  const root = cardRoot("OK", { "scaffold/grokking/testcard/main.py": demoWithDb })
  try {
    const before = listFiles(root)
    const r = await runner.judgeUnit({ unit: CARD_UNIT, learningRoot: root })
    const after = listFiles(root)
    assert.strictEqual(r.verdict, "pass", r.message)
    assert.deepStrictEqual(after, before, "learning 仓里不能多出任何文件")
    assert.strictEqual(after.filter(function (f) { return /\.db$/.test(f) }).length, 0)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("用户类：Python 代码通过 → pass", async function () {
  const unit = {
    id: "algo:lc-00007",
    track: "algorithm",
    kind: "problem",
    title: "两数之和",
    entry: { demo: null, reference: null, graded: "user", script: null, language: "python" },
    asserts: [
      { type: "output", input: "1 2", expect: "3", cmp: "exact" },
      { type: "output", input: "2 3", expect: "5", cmp: "exact" },
    ],
    path: "bank/algorithm/lc-00007.md",
  }
  const code = "a, b = map(int, input().split())\nprint(a + b)\n"
  const r = await runner.judgeUnit({ unit: unit, code: code, language: "python" })
  assert.strictEqual(r.verdict, "pass", r.message)
  assert.strictEqual(r.passedCases, 2)
  assert.strictEqual(r.totalCases, 2)
})

test("用户类：输出不一致 → wrong_answer", async function () {
  const unit = {
    id: "algo:lc-00008",
    track: "algorithm",
    kind: "problem",
    title: "错解",
    entry: { demo: null, reference: null, graded: "user", script: null, language: "python" },
    asserts: [{ type: "output", input: "1 2", expect: "3", cmp: "exact" }],
    path: "bank/algorithm/lc-00008.md",
  }
  const code = "a, b = map(int, input().split())\nprint(a * b)\n"
  const r = await runner.judgeUnit({ unit: unit, code: code, language: "python" })
  assert.strictEqual(r.verdict, "wrong_answer", r.message)
  assert.strictEqual(r.passedCases, 0)
  assert.strictEqual(r.failedCase.type, "output")
})

test("用户类：非零退出 → runtime_error", async function () {
  const unit = {
    id: "algo:lc-00009",
    track: "algorithm",
    kind: "problem",
    title: "崩溃",
    entry: { demo: null, reference: null, graded: "user", script: null, language: "python" },
    asserts: [{ type: "output", input: "", expect: "3", cmp: "exact" }],
    path: "bank/algorithm/lc-00009.md",
  }
  const r = await runner.judgeUnit({ unit: unit, code: "raise SystemExit(3)\n", language: "python" })
  assert.strictEqual(r.verdict, "runtime_error", r.message)
})

test("用户类：不支持的语言 → compile_error", async function () {
  const unit = {
    id: "algo:lc-00010",
    track: "algorithm",
    kind: "problem",
    title: "未知语言",
    entry: { demo: null, reference: null, graded: "user", script: null, language: "python" },
    asserts: [{ type: "output", input: "", expect: "3", cmp: "exact" }],
    path: "bank/algorithm/lc-00010.md",
  }
  const r = await runner.judgeUnit({ unit: unit, code: "print(1)\n", language: "rust" })
  assert.strictEqual(r.verdict, "compile_error", r.message)
  assert.ok(r.message.indexOf("不支持的语言") !== -1, r.message)
})

test("用户类：没有断言 → wrong_answer 并说清原因", async function () {
  const unit = {
    id: "algo:lc-00011",
    track: "algorithm",
    kind: "problem",
    title: "空用例",
    entry: { demo: null, reference: null, graded: "user", script: null, language: "python" },
    asserts: [],
    path: "bank/algorithm/lc-00011.md",
  }
  const r = await runner.judgeUnit({ unit: unit, code: "print(1)\n", language: "python" })
  assert.strictEqual(r.verdict, "wrong_answer", r.message)
  assert.ok(r.message.indexOf("没有测试用例") !== -1, r.message)
})

test("judgeUnit 与 judge() 对同一段代码给出同样的 verdict 与通过数", async function () {
  const cases = [
    { input: "1 2", expect: "3" },
    { input: "2 3", expect: "5" },
    { input: "10 20", expect: "30" },
  ]
  const code = "a, b = map(int, input().split())\nprint(a + b)\n"
  const unit = {
    id: "algo:lc-00012",
    track: "algorithm",
    kind: "problem",
    title: "一致性",
    entry: { demo: null, reference: null, graded: "user", script: null, language: "python" },
    asserts: cases.map(function (c) {
      return { type: "output", input: c.input, expect: c.expect, cmp: "exact" }
    }),
    path: "bank/algorithm/lc-00012.md",
  }
  const viaJudge = await runner.judge({ problem: { cases: cases }, code: code, language: "python" })
  const viaUnit = await runner.judgeUnit({ unit: unit, code: code, language: "python" })
  assert.strictEqual(viaUnit.verdict, viaJudge.verdict, "verdict 必须一致")
  assert.strictEqual(viaUnit.passedCases, viaJudge.passedCases, "通过数必须一致")
  assert.strictEqual(viaUnit.totalCases, viaJudge.totalCases, "总用例数必须一致")
})

test("judgeUnit 的 verdict 永远落在五类之内", async function () {
  const unit = {
    id: "algo:lc-00013",
    track: "algorithm",
    kind: "problem",
    title: "五类",
    entry: { demo: null, reference: null, graded: "user", script: null, language: "python" },
    asserts: [{ type: "output", input: "1", expect: "2", cmp: "exact" }],
    path: "bank/algorithm/lc-00013.md",
  }
  const r = await runner.judgeUnit({ unit: unit, code: "print(1)\n", language: "python" })
  assert.ok(VERDICTS.indexOf(r.verdict) !== -1, r.verdict)
})
