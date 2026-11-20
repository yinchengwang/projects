"use strict"
/**
 * supply-verify.test.js —— 执行器反向校验 + 变异测试
 *
 * 真实跑 Python（与判题器同一套执行器与沙箱），
 * 但**不联网**：断言脚本用 subprocess 跑本地的 demo。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const {
  verifyUnit,
  verifyBatch,
  mutateSource,
  MUTATION_TECHNIQUES,
  SUPPLY_STATUSES,
} = require("../core/supply/verify.js")
const { resolveToolchain } = require("../judge/sandbox.js")
const { stateFilePath } = require("../core/supply/unit-file.js")

const TOOLCHAIN = resolveToolchain({})
const HAS_PYTHON = Boolean(TOOLCHAIN.python)
const SKIP = HAS_PYTHON ? false : "本机没有可用的 Python 解释器，跑不了真实执行"

/** 契约 §3.5 冻结的断言脚本模板（这里按同样的形状写死一份） */
const SCRIPT_TEXT = [
  "#!/usr/bin/env python3",
  '"""demo 卡断言脚本 —— 校验 demo 的关键结论都出现在输出里。"""',
  "",
  "import os",
  "import shutil",
  "import subprocess",
  "import sys",
  "import tempfile",
  "",
  'DEMO = os.environ.get("STUDY_HUB_DEMO") or os.path.join(',
  '    os.path.dirname(os.path.abspath(__file__)), "main.py"',
  ")",
  "",
  "CHECKS = [",
  '    ("算对了", "[[OK]] 算对了"),',
  "]",
  "",
  "",
  "def decode(raw):",
  '    """把字节流解成文本：utf-8 → gbk → latin-1 依次试。"""',
  '    for enc in ("utf-8", "gbk"):',
  "        try:",
  "            return raw.decode(enc)",
  "        except UnicodeDecodeError:",
  "            continue",
  '    return raw.decode("latin-1")',
  "",
  "",
  "def main():",
  "    if not os.path.exists(DEMO):",
  '        print("找不到 demo：" + DEMO)',
  "        return 1",
  "    sandbox = tempfile.mkdtemp(prefix='study-hub-assert-')",
  "    try:",
  "        target = os.path.join(sandbox, os.path.basename(DEMO))",
  "        shutil.copy2(DEMO, target)",
  "        proc = subprocess.run(",
  "            [sys.executable, target],",
  "            capture_output=True,",
  "            timeout=120,",
  "        )",
  "    finally:",
  "        shutil.rmtree(sandbox, ignore_errors=True)",
  "    out = decode(proc.stdout)",
  "    failed = []",
  "    for name, needle in CHECKS:",
  "        if needle not in out:",
  '            failed.append(name + "（缺少：" + needle + "）")',
  "    if proc.returncode != 0:",
  '        failed.append("demo 退出码非 0：" + str(proc.returncode))',
  "    if failed:",
  "        for item in failed:",
  '            print("断言失败：" + item)',
  "        return 1",
  '    print("全部断言通过（%d 条）" % len(CHECKS))',
  "    return 0",
  "",
  "",
  'if __name__ == "__main__":',
  "    sys.exit(main())",
  "",
].join("\n")

/** 有分支、有返回值：变异改坏后输出会变，断言抓得住 */
const DEMO_MUTABLE = [
  "def compute(x):",
  "    return x + 1",
  "",
  "",
  "def main():",
  "    value = compute(1)",
  "    if value == 2:",
  '        print("[[OK]] 算对了")',
  "    else:",
  '        print("[[FAIL]] 算错了")',
  "",
  "",
  'if __name__ == "__main__":',
  "    main()",
  "",
].join("\n")

/** 恒定输出：改坏了也看不出来 → 断言无效 */
const DEMO_VACUOUS = [
  "def compute(x):",
  "    return x + 1",
  "",
  "",
  "def main():",
  '    print("[[OK]] 算对了")',
  "",
  "",
  'if __name__ == "__main__":',
  "    main()",
  "",
].join("\n")

/** 没有任何可改点 */
const DEMO_NO_POINT = ['print("[[OK]] 算对了")', ""].join("\n")

/** demo 自己就崩 */
const DEMO_BROKEN = ['raise RuntimeError("坏了")', ""].join("\n")

/** 依赖 numpy */
const DEMO_NUMPY = [
  "import numpy as np",
  "",
  'print("[[OK]] 算对了")',
  "",
].join("\n")

/** 造一个假的 learning 仓（真的 git 仓，好查脏） */
function makeRepo(demoText, withScript) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "supply-verify-repo-"))
  execFileSync("git", ["init"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["config", "user.email", "t@t"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["config", "user.name", "t"], { cwd: root, stdio: "ignore" })
  const cardDir = path.join(root, "scaffold", "grokking", "demo")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "main.py"), demoText, "utf8")
  const unitsDir = path.join(root, "units")
  fs.mkdirSync(path.join(unitsDir, "database"), { recursive: true })
  if (withScript !== false) {
    fs.writeFileSync(path.join(unitsDir, "database", "demo.assert.py"), SCRIPT_TEXT, "utf8")
  }
  execFileSync("git", ["add", "-A"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["commit", "-m", "init"], { cwd: root, stdio: "ignore" })
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), "supply-verify-state-"))
  return { root: root, unitsDir: unitsDir, stateFile: stateFilePath(stateDir) }
}

/** 一张卡的单元 */
function cardUnit(patch) {
  const p = patch || {}
  return Object.assign(
    {
      id: "db:demo",
      track: "database",
      kind: "card",
      title: "演示卡",
      statement: "# 演示卡\n\n要点\n",
      entry: {
        demo: "scaffold/grokking/demo/main.py",
        reference: "",
        graded: "script",
        script: "units/database/demo.assert.py",
        language: "python",
      },
      asserts: [],
      topics: [],
      difficulty: "medium",
      source: "grokking",
      status: "todo",
      progress: "todo",
      reviewed: false,
      reviewed_at: "",
      subtracks: [],
      path: "scaffold/grokking/demo/",
    },
    p
  )
}

/** 跑一次校验 */
function run(repo, unit, extra) {
  const o = extra || {}
  return verifyUnit(
    Object.assign(
      {
        unit: unit,
        learningRoot: repo.root,
        unitsDir: repo.unitsDir,
        repoRoot: repo.root,
        stateFile: repo.stateFile,
        numpyAvailable: true,
      },
      o
    )
  )
}

test("供给结论只有三种，且与 verdict 五类不是一回事", function () {
  assert.deepStrictEqual(SUPPLY_STATUSES, ["verified", "rejected", "skipped"])
  assert.deepStrictEqual(MUTATION_TECHNIQUES, ["assert", "if", "return"])
})

test("mutateSource：手法 1 把第一条 assert 改成 assert False", function () {
  const src = ["def f():", "    assert x == 1", "    assert y == 2", ""].join("\n")
  const m = mutateSource(src, "assert")
  assert.strictEqual(m.ok, true)
  assert.ok(m.code.indexOf("assert False") !== -1, m.code)
  assert.strictEqual(m.code.indexOf("assert x == 1"), -1)
  assert.ok(m.code.indexOf("assert y == 2") !== -1, "只改第一条")
})

test("mutateSource：手法 2 改块级 if，但不碰入口守卫", function () {
  const src = [
    'if __name__ == "__main__":',
    "    main()",
    "",
    "def f(x):",
    "    if x > 0:",
    "        return 1",
    "    return 0",
    "",
  ].join("\n")
  const m = mutateSource(src, "if")
  assert.strictEqual(m.ok, true)
  assert.ok(m.code.indexOf("if False:") !== -1, m.code)
  assert.ok(m.code.indexOf('if __name__ == "__main__":') !== -1, "入口守卫不许动")
})

test("mutateSource：手法 2 在没有块级 if 时改内联条件式", function () {
  const src = [
    "def f(a, b):",
    '    return f"{a} if a == b else 别的"',
    "",
  ].join("\n")
  const m = mutateSource(src, "if")
  assert.strictEqual(m.ok, true)
  assert.ok(m.code.indexOf("if False else") !== -1, m.code)
})

test("mutateSource：手法 3 把第一条 return 改成 return None", function () {
  const src = ["def f(x):", "    return x + 1", "", "def g(y):", "    return y", ""].join("\n")
  const m = mutateSource(src, "return")
  assert.strictEqual(m.ok, true)
  assert.ok(m.code.indexOf("return None") !== -1, m.code)
  assert.ok(m.code.indexOf("return y") !== -1, "只改第一条")
})

test("mutateSource：找不到可改点时 ok=false 且给中文理由", function () {
  const m = mutateSource('print("hi")\n', "assert")
  assert.strictEqual(m.ok, false)
  assert.ok(m.reason.indexOf("assert") !== -1, m.reason)
})

test("断言通过 + 变异被捕获 → verified，并把 loader C 单元文件写成 verified", { skip: SKIP }, async function () {
  const repo = makeRepo(DEMO_MUTABLE, true)
  const result = await run(repo, cardUnit())
  assert.strictEqual(result.status, "verified", result.reason)
  assert.strictEqual(result.mutated, true)
  assert.strictEqual(result.technique, "if")
  const file = path.join(repo.unitsDir, "database", "demo.md")
  assert.ok(fs.existsSync(file), "单元文件要被写出来")
  const text = fs.readFileSync(file, "utf8")
  assert.ok(text.indexOf("status: verified") !== -1, text)
  assert.ok(text.indexOf("id: db:demo") !== -1, text)
  assert.ok(text.indexOf("script: units/database/demo.assert.py") !== -1, text)
})

test("断言脚本没通过 → rejected", { skip: SKIP }, async function () {
  const repo = makeRepo(DEMO_BROKEN, true)
  const result = await run(repo, cardUnit())
  assert.strictEqual(result.status, "rejected", result.reason)
  assert.ok(result.reason.indexOf("断言脚本没有通过") !== -1, result.reason)
})

test("断言恒真（改坏了也看不出来）→ rejected：变异测试未捕获，断言无效", { skip: SKIP }, async function () {
  const repo = makeRepo(DEMO_VACUOUS, true)
  const result = await run(repo, cardUnit())
  assert.strictEqual(result.status, "rejected", result.reason)
  assert.ok(result.reason.indexOf("变异测试未捕获，断言无效") !== -1, result.reason)
})

test("找不到可变异点 → rejected", { skip: SKIP }, async function () {
  const repo = makeRepo(DEMO_NO_POINT, true)
  const result = await run(repo, cardUnit())
  assert.strictEqual(result.status, "rejected", result.reason)
  assert.ok(result.reason.indexOf("找不到可变异点，无法验证断言有效性") !== -1, result.reason)
})

test("缺 numpy → skipped，不算通过也不算打回", { skip: SKIP }, async function () {
  const repo = makeRepo(DEMO_NUMPY, true)
  const result = await run(repo, cardUnit(), { numpyAvailable: false })
  assert.strictEqual(result.status, "skipped", result.reason)
  assert.ok(result.reason.indexOf("环境缺失") !== -1, result.reason)
  assert.ok(result.reason.indexOf("numpy") !== -1, result.reason)
})

test("算法题 → skipped，理由是「算法题由 P5 校验」", { skip: SKIP }, async function () {
  const repo = makeRepo(DEMO_MUTABLE, true)
  const unit = cardUnit({ id: "algo:lc-00007", track: "algorithm", kind: "problem" })
  const result = await run(repo, unit)
  assert.strictEqual(result.status, "skipped")
  assert.strictEqual(result.reason, "算法题由 P5 校验")
})

test("断言脚本不存在 → skipped", { skip: SKIP }, async function () {
  const repo = makeRepo(DEMO_MUTABLE, false)
  const result = await run(repo, cardUnit())
  assert.strictEqual(result.status, "skipped")
  assert.ok(result.reason.indexOf("断言脚本还不存在") !== -1, result.reason)
})

test("跑完之后 learning 仓里不多出任何文件，demo 原文也没被动过", { skip: SKIP }, async function () {
  const repo = makeRepo(DEMO_MUTABLE, true)
  const before = fs.readFileSync(path.join(repo.root, "scaffold", "grokking", "demo", "main.py"), "utf8")
  const result = await run(repo, cardUnit())
  assert.strictEqual(result.status, "verified", result.reason)
  const after = fs.readFileSync(path.join(repo.root, "scaffold", "grokking", "demo", "main.py"), "utf8")
  assert.strictEqual(after, before, "learning 仓里的 demo 一个字节都不许动")
  const strays = execFileSync(
    "git",
    ["-c", "core.quotepath=false", "status", "--porcelain", "-z", "--untracked-files=all"],
    { cwd: repo.root, encoding: "utf8" }
  )
  const paths = strays.split("\0").filter(function (s) { return s !== "" })
  paths.forEach(function (p) {
    assert.strictEqual(/\.db$/i.test(p), false, "learning 仓里不许多出 .db 文件：" + p)
  })
  assert.deepStrictEqual(paths, ["?? units/database/demo.md"], "只该多出单元文件：" + paths.join("、"))
})

test("verifyBatch 汇总三种结论的计数", { skip: SKIP }, async function () {
  const repo = makeRepo(DEMO_MUTABLE, true)
  const batch = await verifyBatch({
    units: [
      cardUnit({ id: "db:a" }),
      cardUnit({ id: "db:b", kind: "problem" }),
      cardUnit({ id: "db:c", entry: { demo: "scaffold/grokking/demo/main.py", reference: "", graded: "script", script: "units/database/nope.assert.py", language: "python" } }),
    ],
    learningRoot: repo.root,
    unitsDir: repo.unitsDir,
    repoRoot: repo.root,
    stateFile: repo.stateFile,
    numpyAvailable: true,
  })
  assert.strictEqual(batch.counts.total, 3)
  assert.strictEqual(batch.counts.verified, 1)
  assert.strictEqual(batch.counts.skipped, 2)
  assert.strictEqual(batch.counts.rejected, 0)
})
