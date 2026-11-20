"use strict"
/**
 * supply-e2e.test.js —— 供给流水线端到端
 *
 * 四个场景：
 *   A. 一张 DB 卡 draft → verified（执行器原样跑通过 + 变异测试抓到改坏）
 *   B. 恒真断言 → draft → rejected（变异测试没抓到）
 *   C. LLM 关掉时 plan / verify / report 照常可用（G10）
 *   D. 跑完之后 learning 仓工作树一个文件都不多（G18）
 *
 * 全程不联网（G12）：LLM 客户端是假客户端。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const { runCli, EXIT_OK, EXIT_LLM_OFF } = require("../tools/supply.js")
const { resolveToolchain } = require("../judge/sandbox.js")

const TOOLCHAIN = resolveToolchain({})
const HAS_PYTHON = Boolean(TOOLCHAIN.python)
const SKIP = HAS_PYTHON ? false : "本机没有可用的 Python 解释器，跑不了真实执行"

/** 契约 §3.5 冻结的断言脚本模板（新版：subprocess + 临时目录 + 不写死 text=True）——
 *  与契约里的模板**逐字一致**，只把 <stem> 换成 demo、CHECKS 里的一条换成
 *  「算对了」。三条硬性约定都在里面：
 *    ① demo 用 subprocess 跑，不 import；
 *    ② 脚本自己把 demo 复制进 tempfile.mkdtemp() 造的临时目录再跑，跑完 rmtree；
 *    ③ 不写死 text=True——抓字节，再按 utf-8 → gbk → latin-1 依次试解码。 */
const SCRIPT_TEXT = [
  "#!/usr/bin/env python3",
  '"""demo 卡断言脚本 —— 校验 demo 的关键结论都出现在输出里。',
  "",
  "用法：",
  "  1) 直接跑：把本脚本与 demo 的 main.py 放在同一目录（沙箱副本）。",
  "  2) 指定 demo：环境变量 STUDY_HUB_DEMO 指向 demo 的绝对路径。",
  "全部断言通过 → exit 0；任一条不通过 → 打印失败原因并 exit 1。",
  "",
  "编码：demo 的 stdout 按**本机 locale** 编码（Windows 中文环境是 GBK，Linux 是 UTF-8），",
  "所以这里抓字节再按 utf-8 → gbk → latin-1 的顺序试解码，不写死编码。",
  '"""',
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
  "# (说明, 在 stdout 里必须出现的片段)",
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
  "def run_demo():",
  '    """把 demo 复制进临时目录再跑。',
  "",
  "    为什么要复制：有的 demo（mvcc / transaction）会在 __file__ 旁边写 .db 文件，而且不能重复跑。",
  "    复制进临时目录，既保证每次都是干净环境，也保证 .db 不落进 learning 仓。",
  '    """',
  "    if not os.path.exists(DEMO):",
  '        return None, "找不到 demo：" + DEMO',
  '    sandbox = tempfile.mkdtemp(prefix="study-hub-assert-")',
  "    try:",
  "        target = os.path.join(sandbox, os.path.basename(DEMO))",
  "        shutil.copy2(DEMO, target)",
  "        proc = subprocess.run(",
  "            [sys.executable, target],",
  "            capture_output=True,",
  "            timeout=180,",
  "        )",
  "        return proc, None",
  "    finally:",
  "        shutil.rmtree(sandbox, ignore_errors=True)",
  "",
  "",
  "def main():",
  "    proc, err = run_demo()",
  "    if err is not None:",
  "        print(err)",
  "        return 1",
  "    out = decode(proc.stdout)",
  "    failed = []",
  "    for name, needle in CHECKS:",
  "        if needle not in out:",
  '            failed.append(name + "（缺少：" + needle + "）")',
  "    if proc.returncode != 0:",
  '        failed.append("demo 退出码非 0：" + str(proc.returncode))',
  '        failed.append("demo 的 stderr 末尾：" + decode(proc.stderr)[-500:])',
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

/** 恒定输出：改坏了也看不出来 → 断言无效（有变异点，但变异后输出不变） */
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

/** loader C 的单元文件 */
const UNIT_FILE = [
  "---",
  "id: db:demo",
  "track: database",
  "kind: card",
  "title: 演示卡",
  "difficulty: medium",
  "topics: []",
  "source: generated",
  "language: python",
  "demo: scaffold/grokking/demo/main.py",
  'reference: ""',
  "graded: script",
  "script: units/database/demo.assert.py",
  "status: draft",
  "reviewed: false",
  'reviewed_at: ""',
  "subtracks: []",
  "---",
  "",
  "## 讲解",
  "",
  "演示卡的讲解正文。",
  "",
  "## 关联",
  "",
  "- 轮次：R37 Grokking 数据库",
  "",
].join("\n")

/** 造一个假的 learning 仓 */
function makeRepo(demoText) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "supply-e2e-repo-"))
  execFileSync("git", ["init"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["config", "user.email", "t@t"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["config", "user.name", "t"], { cwd: root, stdio: "ignore" })
  const cardDir = path.join(root, "scaffold", "grokking", "demo")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "main.py"), demoText, "utf8")
  const unitsDir = path.join(root, "units")
  fs.mkdirSync(path.join(unitsDir, "database"), { recursive: true })
  fs.writeFileSync(path.join(unitsDir, "database", "demo.assert.py"), SCRIPT_TEXT, "utf8")
  fs.writeFileSync(path.join(unitsDir, "database", "demo.md"), UNIT_FILE, "utf8")
  execFileSync("git", ["add", "-A"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["commit", "-m", "init"], { cwd: root, stdio: "ignore" })
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), "supply-e2e-state-"))
  return { root: root, unitsDir: unitsDir, stateFile: path.join(stateDir, "self-written.json") }
}

/** learning 仓工作树里现在多了哪些文件（NUL 切分，G14 的同一套口径） */
function strayPaths(root) {
  const out = execFileSync(
    "git",
    ["-c", "core.quotepath=false", "status", "--porcelain", "-z", "--untracked-files=all"],
    { cwd: root, encoding: "utf8" }
  )
  return out.split("\0").filter(function (s) { return s !== "" })
}

/** 跑一次 CLI */
function cli(argv, ctx) {
  const base = {
    env: {},
    learningRoot: null,
    localBank: null,
    dbPath: ":memory:",
    stateFile: null,
    client: null,
    ask: null,
  }
  return runCli(argv, Object.assign(base, ctx || {}))
}

test("场景 A：DB 卡 draft → verified（断言通过 + 变异被捕获）", { skip: SKIP }, async function () {
  const repo = makeRepo(DEMO_MUTABLE)
  const res = await cli(["verify", "--unit", "db:demo"], {
    learningRoot: repo.root,
    stateFile: repo.stateFile,
  })
  assert.strictEqual(res.code, EXIT_OK, res.stderr)
  assert.ok(res.stdout.indexOf("已验证") !== -1, res.stdout)
  const text = fs.readFileSync(path.join(repo.unitsDir, "database", "demo.md"), "utf8")
  assert.ok(text.indexOf("status: verified") !== -1, text)
})

test("场景 B：恒真断言 → rejected（变异测试未捕获，断言无效）", { skip: SKIP }, async function () {
  const repo = makeRepo(DEMO_VACUOUS)
  const res = await cli(["verify", "--unit", "db:demo"], {
    learningRoot: repo.root,
    stateFile: repo.stateFile,
  })
  assert.strictEqual(res.code, EXIT_OK, res.stderr)
  assert.ok(res.stdout.indexOf("已打回") !== -1, res.stdout)
  assert.ok(res.stdout.indexOf("变异测试未捕获，断言无效") !== -1, res.stdout)
  const text = fs.readFileSync(path.join(repo.unitsDir, "database", "demo.md"), "utf8")
  assert.ok(text.indexOf("status: rejected") !== -1, text)
})

test("场景 C：LLM 关掉时 plan / verify / report 照常可用（G10）", async function () {
  const repo = makeRepo(DEMO_MUTABLE)
  const off = {
    name: "null",
    model: "null",
    reason: "没有配置 API Key",
    complete: async function () { return "" },
  }
  const plan = await cli(["plan", "--track", "database"], {
    learningRoot: repo.root,
    stateFile: repo.stateFile,
    client: off,
  })
  assert.strictEqual(plan.code, EXIT_OK, plan.stderr)
  assert.ok(plan.stdout.indexOf("供给缺口清单") !== -1, plan.stdout)

  const report = await cli(["report"], {
    learningRoot: repo.root,
    stateFile: repo.stateFile,
    client: off,
  })
  assert.strictEqual(report.code, EXIT_OK, report.stderr)
  assert.ok(report.stdout.indexOf("供给报告") !== -1, report.stdout)

  const verify = await cli(["verify", "--all"], {
    learningRoot: repo.root,
    stateFile: repo.stateFile,
    client: off,
  })
  assert.strictEqual(verify.code, EXIT_OK, verify.stderr)

  const generate = await cli(["generate", "--track", "database"], {
    learningRoot: repo.root,
    stateFile: repo.stateFile,
    client: off,
  })
  assert.strictEqual(generate.code, EXIT_LLM_OFF, generate.stderr)
})

test("场景 D：跑完之后 learning 仓工作树一个文件都不多（G18）", { skip: SKIP }, async function () {
  const repo = makeRepo(DEMO_MUTABLE)
  const demoFile = path.join(repo.root, "scaffold", "grokking", "demo", "main.py")
  const before = fs.readFileSync(demoFile, "utf8")
  const res = await cli(["verify", "--all"], {
    learningRoot: repo.root,
    stateFile: repo.stateFile,
  })
  assert.strictEqual(res.code, EXIT_OK, res.stderr)
  assert.strictEqual(fs.readFileSync(demoFile, "utf8"), before, "learning 仓里的 demo 一个字节都不许动")

  const strays = strayPaths(repo.root)
  strays.forEach(function (item) {
    assert.strictEqual(/\.db$/i.test(item), false, "learning 仓里不许多出 .db 文件：" + item)
  })
  assert.deepStrictEqual(strays, [" M units/database/demo.md"], "只该多出单元文件这一处改动：" + strays.join("、"))
})
