"use strict"
/**
 * supply-cli.test.js —— 供给流水线 CLI（契约 §10）
 *
 * 全程不联网（G12）：LLM 客户端是假客户端，人工抽检是假 ask。
 * learning 仓是临时目录里的真 git 仓（要查脏），跑完**不提交**（G13）。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const {
  runCli,
  parseArgs,
  COMMANDS,
  EXIT_OK,
  EXIT_USAGE,
  EXIT_LLM_OFF,
} = require("../tools/supply.js")

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

/** loader C 的单元文件（契约 §3.3 的 frontmatter 清单） */
/**
 * 造一份单元文件内容。
 *
 * status 可换：`review` 只对 verified 单元开放（补齐设计 §6.2 / R1 Task 5），
 * 所以抽检相关的用例要传 `status: "verified"`；
 * verify 相关的用例仍用默认的 draft（它们要验「draft → verified」的改判）。
 * @param {{status?: string, explain?: string}} [opts]
 * @returns {string}
 */
function buildUnitFile(opts) {
  const o = opts || {}
  return [
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
    "status: " + String(o.status || "draft"),
    "reviewed: false",
    'reviewed_at: ""',
    "subtracks: []",
    "---",
    "",
    "## 讲解",
    "",
    String(o.explain || "演示卡的讲解正文。"),
    "",
    "## 关联",
    "",
    "- 轮次：R37 Grokking 数据库",
    "",
  ].join("\n")
}

const UNIT_FILE = buildUnitFile()

/** 造一个假的 learning 仓（真的 git 仓，好查脏） */
function makeRepo(opts) {
  const o = opts || {}
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "supply-cli-repo-"))
  execFileSync("git", ["init"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["config", "user.email", "t@t"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["config", "user.name", "t"], { cwd: root, stdio: "ignore" })
  const cardDir = path.join(root, "scaffold", "grokking", "demo")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "main.py"), o.demo || DEMO_MUTABLE, "utf8")
  const unitsDir = path.join(root, "units")
  fs.mkdirSync(path.join(unitsDir, "database"), { recursive: true })
  if (o.withScript !== false) {
    fs.writeFileSync(path.join(unitsDir, "database", "demo.assert.py"), SCRIPT_TEXT, "utf8")
  }
  fs.writeFileSync(
    path.join(unitsDir, "database", "demo.md"),
    // 抽档案要 verified；其它案（generate / verify）保持 draft
    o.status ? buildUnitFile({ status: o.status }) : UNIT_FILE,
    "utf8"
  )
  execFileSync("git", ["add", "-A"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["commit", "-m", "init"], { cwd: root, stdio: "ignore" })
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), "supply-cli-state-"))
  return {
    root: root,
    unitsDir: unitsDir,
    stateFile: path.join(stateDir, "self-written.json"),
  }
}

/** 假 LLM 客户端：第一次回断言脚本，第二次回讲解正文 */
function fakeClient() {
  const calls = []
  let n = 0
  return {
    name: "fake",
    model: "fake-model",
    calls: calls,
    complete: async function (req) {
      calls.push(req)
      n += 1
      if (n === 1) return JSON.stringify({ assert_script: SCRIPT_TEXT, topics: ["事务"] })
      return "# 讲解\n\n这是假模型写的讲解正文。\n"
    },
  }
}

/** 假 ask：人一律点「通过」 */
function fakeAsk(answer) {
  const seen = []
  const fn = async function (unit) {
    seen.push(String(unit.id))
    return answer || { passed: true, note: "" }
  }
  fn.seen = seen
  return fn
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

/** git 里有多少个提交（用来证明 CLI 没提交） */
function commitCount(root) {
  const out = execFileSync("git", ["rev-list", "--count", "HEAD"], {
    cwd: root,
    encoding: "utf8",
  })
  return Number(String(out).trim())
}

test("CLI 的子命令只有五个", function () {
  assert.deepStrictEqual(COMMANDS, ["plan", "generate", "verify", "review", "report"])
})

test("parseArgs：认得五个子命令", function () {
  assert.strictEqual(parseArgs(["plan"]).command, "plan")
  assert.strictEqual(parseArgs(["report", "--track", "database"]).command, "report")
  assert.strictEqual(parseArgs(["report", "--track", "database"]).options.track, "database")
  assert.strictEqual(parseArgs(["verify", "--all"]).options.all, true)
})

test("parseArgs：不接受把密钥写在命令行上（G9）", function () {
  const parsed = parseArgs(["generate", "--api-key", "sk-xxx"])
  assert.strictEqual(parsed.error !== "", true)
  assert.ok(parsed.error.indexOf("密钥") !== -1, parsed.error)
})

test("parseArgs：不认识的子命令 / 不该给这个子命令的选项都要报错", function () {
  assert.ok(parseArgs(["nope"]).error.indexOf("子命令") !== -1)
  assert.ok(parseArgs(["verify", "--sample", "5"]).error.indexOf("不接受选项") !== -1)
  assert.ok(parseArgs([]).error.indexOf("子命令") !== -1)
})

test("plan：打出缺口清单，并写 learning/units/<track>/plan.json", async function () {
  const repo = makeRepo({ withScript: false })
  const res = await cli(["plan"], { learningRoot: repo.root, stateFile: repo.stateFile })
  assert.strictEqual(res.code, EXIT_OK, res.stderr)
  assert.ok(res.stdout.indexOf("供给缺口清单") !== -1, res.stdout)
  assert.ok(res.stdout.indexOf("缺断言脚本") !== -1, res.stdout)
  const planFile = path.join(repo.unitsDir, "database", "plan.json")
  assert.ok(fs.existsSync(planFile), "plan.json 要落盘")
  const plan = JSON.parse(fs.readFileSync(planFile, "utf8"))
  assert.strictEqual(plan.track, "database")
  assert.ok(plan.gaps.length > 0, JSON.stringify(plan))
})

test("plan：纯本地模式打印中文提示并正常退出（G17）", async function () {
  const res = await cli(["plan"], { learningRoot: null })
  assert.strictEqual(res.code, EXIT_OK, res.stderr)
  assert.ok(res.stderr.indexOf("learning") !== -1, res.stderr)
})

test("generate：LLM 关掉时退出码 2 + 中文提示（G10）", async function () {
  const repo = makeRepo({ withScript: false })
  const res = await cli(["generate", "--track", "database"], {
    learningRoot: repo.root,
    stateFile: repo.stateFile,
    client: { name: "null", model: "null", reason: "没有配置 API Key", complete: async function () { return "" } },
  })
  assert.strictEqual(res.code, EXIT_LLM_OFF, res.stderr)
  assert.ok(res.stderr.indexOf("LLM") !== -1, res.stderr)
  assert.ok(res.stderr.indexOf("plan / verify / review / report") !== -1, res.stderr)
})

test("generate：LLM 开着时生成断言脚本 + 讲解正文，状态是 draft", async function () {
  const repo = makeRepo({ withScript: false })
  const client = fakeClient()
  const res = await cli(["generate", "--track", "database"], {
    learningRoot: repo.root,
    stateFile: repo.stateFile,
    client: client,
  })
  assert.strictEqual(res.code, EXIT_OK, res.stderr)
  assert.strictEqual(client.calls.length, 2, "生成断言脚本 + 写讲解，各调一次 LLM")
  const scriptFile = path.join(repo.unitsDir, "database", "demo.assert.py")
  assert.ok(fs.existsSync(scriptFile), "断言脚本要落盘")
  assert.ok(fs.readFileSync(scriptFile, "utf8").indexOf("STUDY_HUB_DEMO") !== -1)
  const unitText = fs.readFileSync(path.join(repo.unitsDir, "database", "demo.md"), "utf8")
  assert.ok(unitText.indexOf("status: draft") !== -1, unitText)
  assert.ok(unitText.indexOf("假模型写的讲解正文") !== -1, unitText)
})

test("generate：没连上 learning 仓时退出码 1", async function () {
  const res = await cli(["generate"], { learningRoot: null })
  assert.strictEqual(res.code, EXIT_USAGE, res.stderr)
  assert.ok(res.stderr.indexOf("还没连上 learning 仓") !== -1, res.stderr)
})

test("verify：draft → verified，只打印 diff，不提交（G13）", async function () {
  const repo = makeRepo({ withScript: true })
  const before = commitCount(repo.root)
  const res = await cli(["verify", "--all"], { learningRoot: repo.root, stateFile: repo.stateFile })
  assert.strictEqual(res.code, EXIT_OK, res.stderr)
  assert.ok(res.stdout.indexOf("已验证") !== -1, res.stdout)
  const unitText = fs.readFileSync(path.join(repo.unitsDir, "database", "demo.md"), "utf8")
  assert.ok(unitText.indexOf("status: verified") !== -1, unitText)
  assert.strictEqual(commitCount(repo.root), before, "CLI 绝不许自己提交")
})

test("verify：变异抓不住 → 打回 rejected", async function () {
  const repo = makeRepo({ withScript: true, demo: DEMO_VACUOUS })
  const res = await cli(["verify", "--all"], { learningRoot: repo.root, stateFile: repo.stateFile })
  assert.strictEqual(res.code, EXIT_OK, res.stderr)
  assert.ok(res.stdout.indexOf("已打回") !== -1, res.stdout)
  assert.ok(res.stdout.indexOf("变异测试未捕获，断言无效") !== -1, res.stdout)
  const unitText = fs.readFileSync(path.join(repo.unitsDir, "database", "demo.md"), "utf8")
  assert.ok(unitText.indexOf("status: rejected") !== -1, unitText)
})

test("review：写回 reviewed: true 与 reviewed_at", async function () {
  const repo = makeRepo({ withScript: true, status: "verified" })
  const ask = fakeAsk({ passed: true, note: "讲得清楚" })
  const res = await cli(["review", "--sample", "5"], {
    learningRoot: repo.root,
    stateFile: repo.stateFile,
    ask: ask,
  })
  assert.strictEqual(res.code, EXIT_OK, res.stderr)
  assert.ok(ask.seen.length > 0, "要问人")
  const unitText = fs.readFileSync(path.join(repo.unitsDir, "database", "demo.md"), "utf8")
  assert.ok(unitText.indexOf("reviewed: true") !== -1, unitText)
  assert.ok(/reviewed_at: \d{4}-\d{2}-\d{2}/.test(unitText), unitText)
  assert.ok(unitText.indexOf("讲得清楚") !== -1, "抽检意见要落在「## 关联」小节里")
})

test("review：别人动过的文件一律阻塞写回（G14）", async function () {
  const repo = makeRepo({ withScript: true, status: "verified" })
  const file = path.join(repo.unitsDir, "database", "demo.md")
  fs.writeFileSync(file, fs.readFileSync(file, "utf8") + "\n别人加的一行\n", "utf8")
  const ask = fakeAsk({ passed: true, note: "" })
  const res = await cli(["review", "--sample", "5"], {
    learningRoot: repo.root,
    stateFile: repo.stateFile,
    ask: ask,
  })
  assert.strictEqual(res.code, EXIT_OK, res.stderr)
  assert.ok(res.stderr.indexOf("写回被阻塞") !== -1, res.stderr)
  assert.ok(fs.readFileSync(file, "utf8").indexOf("reviewed: true") === -1, "不该写进去")
})

test("report：打印每轨计数与欠账", async function () {
  const repo = makeRepo({ withScript: false })
  const res = await cli(["report"], { learningRoot: repo.root, stateFile: repo.stateFile })
  assert.strictEqual(res.code, EXIT_OK, res.stderr)
  assert.ok(res.stdout.indexOf("供给报告") !== -1, res.stdout)
  assert.ok(res.stdout.indexOf("数据库") !== -1, res.stdout)
  assert.ok(res.stdout.indexOf("欠账") !== -1, res.stdout)
})
