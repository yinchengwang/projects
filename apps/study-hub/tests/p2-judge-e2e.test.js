"use strict"
/**
 * p2-judge-e2e.test.js —— 判题链路的端到端验收
 *
 * 覆盖：
 *   1. 五类 verdict 各一条：pass / wrong_answer / timeout / compile_error / runtime_error
 *   2. 四型断言各一条：output / exit / contains / file
 *   3. 零污染：判完之后 learning 仓不多出任何文件
 *   4. 真实资产：P0 已落的 4 个数据库卡断言脚本经 judgeUnit → pass
 *
 * 本文件**只读**引用 learning 仓（P0 的 Task 10 已经落了那 4 个断言脚本），
 * 自己不写 learning 仓、不提交 learning 仓。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const runner = require("../judge/runner.js")
const sandboxMod = require("../judge/sandbox.js")
const assertsMod = require("../judge/asserts.js")

const ROOT = path.resolve(__dirname, "..")

/**
 * 定位 learning 仓：先看环境变量，再看仓库默认布局（apps/study-hub 上三级）。
 * 两处都找不到就跳过——但**必须给出理由**，不许静默跳过。
 * @returns {string|null}
 */
function resolveLearningRoot() {
  const fromEnv = process.env.STUDY_HUB_LEARNING_ROOT
  if (fromEnv && fs.existsSync(fromEnv)) return path.resolve(fromEnv)
  const fallback = path.resolve(ROOT, "..", "..", "..", "learning")
  if (fs.existsSync(fallback)) return fallback
  return null
}

const LEARNING_ROOT = resolveLearningRoot()
const PYTHON = sandboxMod.detectPython()

/**
 * 造一个临时 learning 仓。传入「相对路径 → 文件内容」的映射。
 * @param {Object<string,string>} files
 * @returns {string} 临时仓根目录
 */
function makeLearningRoot(files) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-e2e-"))
  Object.keys(files || {}).forEach(function (rel) {
    const abs = path.join(root, rel)
    fs.mkdirSync(path.dirname(abs), { recursive: true })
    fs.writeFileSync(abs, files[rel], "utf8")
  })
  return root
}

/**
 * 列出一个目录下的全部文件（相对路径，斜杠分隔）。`.git` 跳过。
 * 用它来证明「判完之后 learning 仓没有多出任何文件」。
 * @param {string} root
 * @returns {Array<string>}
 */
function listFiles(root) {
  const out = []
  const walk = function (dir) {
    let names = []
    try {
      names = fs.readdirSync(dir)
    } catch (e) {
      return
    }
    names.sort().forEach(function (name) {
      if (name === ".git") return
      const abs = path.join(dir, name)
      let st = null
      try {
        st = fs.statSync(abs)
      } catch (e) {
        return
      }
      if (st.isDirectory()) walk(abs)
      else out.push(path.relative(root, abs).split(path.sep).join("/"))
    })
  }
  walk(root)
  return out
}

/**
 * 按契约 §3.5 的模板生成一个断言脚本。
 * 骨架与冻结模板一致：subprocess 跑 demo（不 import）、把 demo 复制进临时目录再跑、
 * 抓字节按 utf-8 → gbk 依次试解码（**不写死 text=True**）、
 * 全过打印「全部断言通过（N 条）」、有失败打印「断言失败：…」并 exit 1、
 * 失败时把 demo 的 stderr 转发到脚本自己的 stderr。
 * @param {{checks?: Array<[string,string]>, pre?: string}} opts
 * @returns {string} Python 源码
 */
function makeScript(opts) {
  const o = opts || {}
  const checks = o.checks || []
  const L = []
  L.push("import os")
  L.push("import shutil")
  L.push("import subprocess")
  L.push("import sys")
  L.push("import tempfile")
  L.push("")
  L.push("DEMO = os.environ.get(\"STUDY_HUB_DEMO\") or os.path.join(os.path.dirname(os.path.abspath(__file__)), \"main.py\")")
  L.push("CHECKS = [")
  checks.forEach(function (c) {
    L.push("    (" + JSON.stringify(c[0]) + ", " + JSON.stringify(c[1]) + "),")
  })
  L.push("]")
  L.push("")
  L.push("")
  L.push("def decode(raw):")
  L.push("    for enc in (\"utf-8\", \"gbk\"):")
  L.push("        try:")
  L.push("            return raw.decode(enc)")
  L.push("        except UnicodeDecodeError:")
  L.push("            continue")
  L.push("    return raw.decode(\"utf-8\", errors=\"replace\")")
  L.push("")
  L.push("")
  L.push("def run_demo():")
  L.push("    sandbox = tempfile.mkdtemp(prefix=\"study-hub-assert-\")")
  L.push("    try:")
  L.push("        target = os.path.join(sandbox, os.path.basename(DEMO))")
  L.push("        shutil.copy2(DEMO, target)")
  L.push("        proc = subprocess.run([sys.executable, target], capture_output=True, timeout=180)")
  L.push("        return proc, None")
  L.push("    finally:")
  L.push("        shutil.rmtree(sandbox, ignore_errors=True)")
  L.push("")
  L.push("")
  L.push("def main():")
  if (o.pre) {
    o.pre.split("\n").forEach(function (line) {
      L.push("    " + line)
    })
  }
  L.push("    proc, err = run_demo()")
  L.push("    if err is not None:")
  L.push("        print(err)")
  L.push("        return 1")
  L.push("    out = decode(proc.stdout)")
  L.push("    print(\"─── demo 输出开始 ───\")")
  L.push("    print(out.rstrip())")
  L.push("    print(\"─── demo 输出结束 ───\")")
  L.push("    failed = []")
  L.push("    for name, needle in CHECKS:")
  L.push("        if needle not in out:")
  L.push("            failed.append(name + \"（缺少：\" + needle + \"）\")")
  L.push("    if proc.returncode != 0:")
  L.push("        failed.append(\"demo 退出码非 0：\" + str(proc.returncode))")
  L.push("        failed.append(\"demo 的 stderr 末尾：\" + decode(proc.stderr)[-500:])")
  L.push("    if failed:")
  L.push("        for item in failed:")
  L.push("            print(\"断言失败：\" + item)")
  L.push("        print(\"─── demo 错误输出开始 ───\", file=sys.stderr)")
  L.push("        print(decode(proc.stderr).rstrip(), file=sys.stderr)")
  L.push("        print(\"─── demo 错误输出结束 ───\", file=sys.stderr)")
  L.push("        return 1")
  L.push("    print(\"全部断言通过（%d 条）\" % len(CHECKS))")
  L.push("    return 0")
  L.push("")
  L.push("")
  L.push("if __name__ == \"__main__\":")
  L.push("    sys.exit(main())")
  L.push("")
  return L.join("\n")
}

/**
 * 造一张卡类单元（graded = script）。
 * @param {string} id
 * @param {string} scriptRel 相对 learning 仓根的断言脚本路径
 * @param {string} demoRel 相对 learning 仓根的 demo 路径
 * @returns {object}
 */
function cardUnit(id, scriptRel, demoRel) {
  return {
    id: id,
    track: "database",
    kind: "card",
    title: "端到端测试卡",
    entry: { demo: demoRel, reference: null, graded: "script", script: scriptRel, language: "python" },
    asserts: [],
    path: scriptRel,
  }
}

/**
 * 造一道题目类单元（graded = user）。
 * @param {string} id
 * @param {Array<object>} asserts
 * @returns {object}
 */
function problemUnit(id, asserts) {
  return {
    id: id,
    track: "algorithm",
    kind: "problem",
    title: "端到端测试题",
    entry: { demo: null, reference: null, graded: "user", script: null, language: "python" },
    asserts: asserts,
    path: "bank/algorithm/" + id + ".md",
  }
}

const DEMO_OK = ["print(\"hello demo\")", ""].join("\n")

/** 本机没有 Python 时统一用的跳过理由。 */
function needPython(t) {
  t.skip("本机没有 Python 解释器（python3 / python / py -3 都试过了），判不了")
  return false
}
// ── 五类 verdict ─────────────────────────────────────────────────

test("verdict · pass：断言全过", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const root = makeLearningRoot({
    "demo/main.py": DEMO_OK,
    "units/database/ok.assert.py": makeScript({ checks: [["演示输出", "hello demo"]] }),
  })
  try {
    const unit = cardUnit("db:e2e-ok", "units/database/ok.assert.py", "demo/main.py")
    const r = await runner.judgeUnit({ unit: unit, learningRoot: root })
    assert.strictEqual(r.verdict, "pass", r.message)
    assert.ok(r.message.indexOf("通过") !== -1, r.message)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("verdict · wrong_answer：输出与期望不匹配", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const root = makeLearningRoot({
    "demo/main.py": DEMO_OK,
    "units/database/no.assert.py": makeScript({ checks: [["不存在的结论", "这句话不在 demo 输出里"]] }),
  })
  try {
    const unit = cardUnit("db:e2e-no", "units/database/no.assert.py", "demo/main.py")
    const r = await runner.judgeUnit({ unit: unit, learningRoot: root })
    assert.strictEqual(r.verdict, "wrong_answer", r.message)
    assert.ok(r.stdout.indexOf("断言失败") !== -1, "脚本应当打印中文失败原因：" + r.stdout)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("verdict · timeout：脚本挂住不退出", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const root = makeLearningRoot({
    "demo/main.py": DEMO_OK,
    "units/database/hang.assert.py": makeScript({ pre: "import time\ntime.sleep(999)" }),
  })
  try {
    const unit = cardUnit("db:e2e-hang", "units/database/hang.assert.py", "demo/main.py")
    const r = await runner.judgeUnit({ unit: unit, learningRoot: root, timeoutMs: 1500 })
    assert.strictEqual(r.verdict, "timeout", r.message)
    assert.ok(r.message.indexOf("超时") !== -1, r.message)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("verdict · compile_error：断言脚本不存在（中文说明，不是 500、不是抛异常）", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const root = makeLearningRoot({ "demo/main.py": DEMO_OK })
  try {
    const unit = cardUnit("db:e2e-missing", "units/database/missing.assert.py", "demo/main.py")
    const r = await runner.judgeUnit({ unit: unit, learningRoot: root })
    assert.strictEqual(r.verdict, "compile_error", r.message)
    assert.ok(r.message.indexOf("断言脚本不存在") !== -1, r.message)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("verdict · runtime_error：断言脚本自己抛异常", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const root = makeLearningRoot({
    "demo/main.py": DEMO_OK,
    "units/database/boom.assert.py": makeScript({ pre: "raise RuntimeError(\"boom\")" }),
  })
  try {
    const unit = cardUnit("db:e2e-boom", "units/database/boom.assert.py", "demo/main.py")
    const r = await runner.judgeUnit({ unit: unit, learningRoot: root })
    assert.strictEqual(r.verdict, "runtime_error", r.message)
    assert.ok(r.message.indexOf("崩了") !== -1, r.message)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

// ── 四型断言 ─────────────────────────────────────────────────────

test("断言 · output：匹配得上 → pass", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const unit = problemUnit("algo:e2e-output-ok", [{ type: "output", input: "1 2", expect: "3", cmp: "exact" }])
  const code = "a, b = map(int, input().split())\nprint(a + b)\n"
  const r = await runner.judgeUnit({ unit: unit, code: code, language: "python" })
  assert.strictEqual(r.verdict, "pass", r.message)
  assert.strictEqual(r.passedCases, 1)
})

test("断言 · output：匹配不上 → wrong_answer，failedCase.type 是 output", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const unit = problemUnit("algo:e2e-output-no", [{ type: "output", input: "1 2", expect: "3", cmp: "exact" }])
  const code = "a, b = map(int, input().split())\nprint(a * b)\n"
  const r = await runner.judgeUnit({ unit: unit, code: code, language: "python" })
  assert.strictEqual(r.verdict, "wrong_answer", r.message)
  assert.ok(r.failedCase, "必须指出是哪一条断言没过")
  assert.strictEqual(r.failedCase.type, "output")
})

test("断言 · exit：expect 0 → pass", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const unit = problemUnit("algo:e2e-exit-ok", [{ type: "exit", expect: 0 }])
  const r = await runner.judgeUnit({ unit: unit, code: "print('ok')\n", language: "python" })
  assert.strictEqual(r.verdict, "pass", r.message)
})

test("断言 · exit：expect 非法 → wrong_answer，理由是中文", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const unit = problemUnit("algo:e2e-exit-bad", [{ type: "exit", expect: 1 }])
  const r = await runner.judgeUnit({ unit: unit, code: "print('ok')\n", language: "python" })
  assert.strictEqual(r.verdict, "wrong_answer", r.message)
  assert.ok(r.failedCase, "必须指出是哪一条断言没过")
  assert.strictEqual(r.failedCase.type, "exit")
  assert.ok(r.failedCase.reason.indexOf("断言非法") !== -1, r.failedCase.reason)
})

test("断言 · contains：找得到 → pass", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const unit = problemUnit("algo:e2e-contains-ok", [{ type: "contains", text: "答案" }])
  const r = await runner.judgeUnit({ unit: unit, code: "print('答案是 42')\n", language: "python" })
  assert.strictEqual(r.verdict, "pass", r.message)
})

test("断言 · contains：找不到 → wrong_answer，failedCase.type 是 contains", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const unit = problemUnit("algo:e2e-contains-no", [{ type: "contains", text: "这个片段不存在" }])
  const r = await runner.judgeUnit({ unit: unit, code: "print('答案是 42')\n", language: "python" })
  assert.strictEqual(r.verdict, "wrong_answer", r.message)
  assert.ok(r.failedCase, "必须指出是哪一条断言没过")
  assert.strictEqual(r.failedCase.type, "contains")
})

test("断言 · file：产物对得上 → pass", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const unit = problemUnit("algo:e2e-file-ok", [{ type: "file", path: "out.txt", expect: "hello" }])
  // 写文件必须显式 encoding="utf-8"：本机默认编码是 GBK，
  // 而 readArtifact 读的是 utf-8（契约 §2 的 file 口径）。
  const code = "open('out.txt', 'w', encoding='utf-8').write('hello')\n"
  const r = await runner.judgeUnit({ unit: unit, code: code, language: "python" })
  assert.strictEqual(r.verdict, "pass", r.message)
})

test("断言 · file：产物对不上 → wrong_answer，failedCase.type 是 file", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  const unit = problemUnit("algo:e2e-file-no", [{ type: "file", path: "out.txt", expect: "hello" }])
  const code = "open('out.txt', 'w', encoding='utf-8').write('world')\n"
  const r = await runner.judgeUnit({ unit: unit, code: code, language: "python" })
  assert.strictEqual(r.verdict, "wrong_answer", r.message)
  assert.ok(r.failedCase, "必须指出是哪一条断言没过")
  assert.strictEqual(r.failedCase.type, "file")
})

test("四型断言的校验器本身也认这四种类型", function () {
  const res = assertsMod.validateAsserts(
    [
      { type: "output", expect: "hi", cmp: "exact" },
      { type: "exit", expect: 0 },
      { type: "contains", text: "hi" },
      { type: "file", path: "out.txt", expect: "hi" },
    ],
    { stdout: "hi", stderr: "", exitCode: 0, timedOut: false, artifacts: { "out.txt": "hi" } }
  )
  assert.strictEqual(res.total, 4)
  assert.strictEqual(res.passed, 4)
  assert.deepStrictEqual(res.failures, [])
})

// ── 零污染 ───────────────────────────────────────────────────────

test("零污染：判完之后 learning 仓不多出任何文件", async function (t) {
  if (!PYTHON) {
    needPython(t)
    return
  }
  // demo 会在自己旁边写 .db——正是要盯住的那种污染
  const demoWithDb = [
    "import os",
    "open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'demo.db'), 'w').write('x')",
    "print('hello demo')",
    "",
  ].join("\n")
  const root = makeLearningRoot({
    "demo/main.py": demoWithDb,
    "units/database/ok.assert.py": makeScript({ checks: [["演示输出", "hello demo"]] }),
  })
  try {
    const before = listFiles(root)
    const unit = cardUnit("db:e2e-clean", "units/database/ok.assert.py", "demo/main.py")
    const r = await runner.judgeUnit({ unit: unit, learningRoot: root })
    const after = listFiles(root)
    assert.strictEqual(r.verdict, "pass", r.message)
    assert.deepStrictEqual(after, before, "learning 仓里不能多出任何文件")
    assert.strictEqual(after.filter(function (f) { return /\.db$/.test(f) }).length, 0)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

// ── 真实资产：P0 已落的 4 个数据库卡 ─────────────────────────────

const DB_CARDS = ["mvcc", "transaction", "index_design", "sql_fundamentals"]

DB_CARDS.forEach(function (stem) {
  test("真实资产 · " + stem + " 的断言脚本经 judgeUnit → pass", async function (t) {
    if (!LEARNING_ROOT) {
      t.skip("找不到 learning 仓：设 STUDY_HUB_LEARNING_ROOT 或在仓库默认布局下运行")
      return
    }
    if (!PYTHON) {
      needPython(t)
      return
    }
    const scriptRel = "units/database/" + stem + ".assert.py"
    const demoRel = "scaffold/grokking/" + stem + "/main.py"
    assert.ok(fs.existsSync(path.join(LEARNING_ROOT, scriptRel)), "断言脚本不存在：" + scriptRel)
    assert.ok(fs.existsSync(path.join(LEARNING_ROOT, demoRel)), "demo 不存在：" + demoRel)

    const unit = cardUnit("db:" + stem, scriptRel, demoRel)
    const r = await runner.judgeUnit({ unit: unit, learningRoot: LEARNING_ROOT })
    assert.strictEqual(
      r.verdict,
      "pass",
      stem + " 判题没通过：" + r.message + "\nstdout:\n" + r.stdout + "\nstderr:\n" + r.stderr
    )
    assert.ok(r.stdout.indexOf("全部断言通过") !== -1, "脚本应当打印通过标记：" + r.stdout)
  })
})
