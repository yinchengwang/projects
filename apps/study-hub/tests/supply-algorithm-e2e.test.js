"use strict"
/**
 * supply-algorithm-e2e.test.js —— 端到端：假 LLM → 三件套 → 真编译执行 → 状态写回
 *
 * 覆盖五条：
 *   1. draft → verified（参考解法全 AC、负例被拒）
 *   2. draft → rejected（负例骗过全部用例）
 *   3. 存量题库零改动（learning/bank/** 一个字节都不许动）
 *   4. 真实 git 查脏：有别人的未提交改动就停下
 *   5. 全程不联网（一次 fetch 都不许有）
 *
 * LLM 注入假客户端（G12）；编译执行是真的 g++（没装 g++ 的机器明确 skip）。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const crypto = require("node:crypto")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { spawnSync } = require("node:child_process")

const cli = require("../core/supply/algorithm-cli.js")
const { detectCompiler } = require("../judge/sandbox.js")

/** 没有 g++ 就整份 skip，并说清原因 */
const SKIP = detectCompiler() ? false : "本机没有 g++，无法做真实编译执行"

/** 没有 git 就 skip 查脏那条 */
const GIT_OK = spawnSync("git", ["--version"], { encoding: "utf8" }).status === 0

/** 正确的参考解法：把输入反转后输出 */
const REF_CODE = [
  "#include <iostream>",
  "#include <string>",
  "#include <algorithm>",
  "int main() {",
  "  std::string s;",
  "  if (!(std::cin >> s)) return 0;",
  "  std::reverse(s.begin(), s.end());",
  '  std::cout << s << "\\n";',
  "  return 0;",
  "}",
].join("\n")

/** 负例：原样输出（逻辑错了，但能编译能跑）；带一个独一无二的标记，便于断言它没进单元 md */
const NEG_CODE = [
  "#include <iostream>",
  "#include <string>",
  "int main() {",
  "  std::string s;",
  "  if (!(std::cin >> s)) return 0;",
  '  std::cout << "WRONG:" << s << "\\n";',
  "  return 0;",
  "}",
].join("\n")

/** 负例：结果其实对、只是写法不同（能骗过所有用例——正是要抓住的那种） */
const PASSING_NEG_CODE = [
  "#include <iostream>",
  "#include <string>",
  "int main() {",
  "  std::string s;",
  "  if (!(std::cin >> s)) return 0;",
  "  std::string out;",
  "  for (int i = (int)s.size() - 1; i >= 0; --i) out += s[i];",
  '  std::cout << out << "\\n";',
  "  return 0;",
  "}",
].join("\n")

/** 内存里的 settings 仓储（不碰真 SQLite） */
function fakeStore() {
  const data = {}
  return {
    data: data,
    getSetting: function (key, fallback) {
      return data[key] === undefined ? fallback : data[key]
    },
    setSetting: function (key, value) {
      data[key] = String(value)
    },
  }
}

/** 假客户端：按调用次数给不同的回复 */
function fakeClient(responder) {
  return {
    name: "fake",
    model: "fake-model",
    calls: 0,
    async complete(req) {
      this.calls += 1
      return responder(req, this.calls)
    },
  }
}

/** 从提示词里抠出题号 */
function stemOf(req) {
  const m = /题号：([^\n]+)/.exec(String((req && req.user) || ""))
  return m ? m[1].trim() : ""
}

/** 造一份四件套回复 */
function answerOf(negative) {
  return JSON.stringify({
    title: "反转字符串",
    statement: "给你一个字符串，把它反过来输出。",
    difficulty: "easy",
    topics: ["字符串"],
    cases: [{ input: "123", expect: "321" }],
    solution: REF_CODE,
    negative: negative,
    negativeNote: "负例把输入原样输出了",
  })
}

/** 跑一次 git */
function git(root, args) {
  return spawnSync("git", args, { cwd: root, encoding: "utf8", windowsHide: true })
}

/** 造一个真的 git 仓当 learning 仓 */
function makeGitRoot() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-e2e-"))
  git(root, ["init"])
  git(root, ["config", "user.email", "p5@example.com"])
  git(root, ["config", "user.name", "p5"])
  return root
}

/** 全部提交 */
function commitAll(root) {
  git(root, ["add", "-A"])
  git(root, ["commit", "-m", "init"])
}

/** 往 learning 仓的 bank/ 里放一份题库文件 */
function bankFile(root, rel, opts) {
  const o = opts || {}
  const abs = path.join(root, "bank", rel)
  fs.mkdirSync(path.dirname(abs), { recursive: true })
  const lines = []
  lines.push("---")
  lines.push("id: " + o.id)
  lines.push("source: " + (o.source || "leetcode"))
  lines.push("source_id: " + (o.sourceId || "7"))
  lines.push("title: " + (o.title || "反转字符串"))
  lines.push("difficulty: " + (o.difficulty || "easy"))
  lines.push("topics: [字符串]")
  lines.push("tracks: [" + (o.subtrack || "fundamentals") + "]")
  lines.push("status: todo")
  lines.push("needs_fill: true")
  lines.push("---")
  lines.push("")
  lines.push("## 题面")
  lines.push("")
  if (o.statement) lines.push(o.statement)
  lines.push("")
  lines.push("## 测试用例")
  lines.push("")
  lines.push("| input | expect |")
  lines.push("|---|---|")
  if (o.cases) {
    o.cases.forEach(function (c) {
      lines.push("| " + c.input + " | " + c.expect + " |")
    })
  }
  lines.push("")
  lines.push("## 参考解法")
  lines.push("")
  fs.writeFileSync(abs, lines.join("\n"), "utf8")
  return abs
}

/** 目录内容的 sha256 清单 */
function digestTree(dir) {
  const out = {}
  function walk(d) {
    if (!fs.existsSync(d)) return
    for (const name of fs.readdirSync(d)) {
      const p = path.join(d, name)
      if (fs.statSync(p).isDirectory()) walk(p)
      else out[path.relative(dir, p).split(path.sep).join("/")] = crypto
        .createHash("sha256")
        .update(fs.readFileSync(p))
        .digest("hex")
    }
  }
  walk(dir)
  return out
}

/** 跑一次算法轨 generate */
function runGenerate(root, client, store, extraArgs) {
  const localBank = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-e2e-local-"))
  return cli
    .runAlgorithmCommand({
      argv: ["generate", "--track", "algorithm", "--batch"].concat(extraArgs || []),
      learningRoot: root,
      localBank: localBank,
      client: client,
      store: store,
      env: {},
    })
    .finally(function () {
      fs.rmSync(localBank, { recursive: true, force: true })
    })
}

test("端到端：draft → verified（三件套落盘、真编译执行、status 改对）", { skip: SKIP }, async function () {
  const root = makeGitRoot()
  try {
    // 形态 A：题面与用例都空
    bankFile(root, "fundamentals/lc-00001.md", { id: "lc-00001" })
    // 形态 C：题面与用例齐、参考解法空
    bankFile(root, "interview/lc-00003.md", {
      id: "lc-00003",
      statement: "给你一个字符串，把它反过来输出。",
      cases: [{ input: "123", expect: "321" }],
    })
    commitAll(root)

    const client = fakeClient(function () {
      return answerOf(NEG_CODE)
    })
    const store = fakeStore()
    const res = await runGenerate(root, client, store, ["--session", "batch-e2e"])
    assert.equal(res.code, 0, res.output)
    assert.equal(client.calls, 2, "两题两次调用")

    const unitsDir = path.join(root, "units", "algorithm")
    const md = fs.readFileSync(path.join(unitsDir, "lc-00001.md"), "utf8")
    assert.ok(fs.existsSync(path.join(unitsDir, "lc-00001.reference.cpp")), "要有参考解法文件")
    assert.ok(fs.existsSync(path.join(unitsDir, "lc-00001.negative.cpp")), "要有负例文件")
    assert.ok(md.indexOf("status: verified") !== -1, md.slice(0, 600))

    // 形态 C：题面与用例原样保留
    const md3 = fs.readFileSync(path.join(unitsDir, "lc-00003.md"), "utf8")
    assert.ok(md3.indexOf("给你一个字符串，把它反过来输出。") !== -1, "题面要原样保留")
    assert.ok(md3.indexOf("| 123 | 321 |") !== -1, "用例要原样保留")
    assert.ok(md3.indexOf("status: verified") !== -1, md3.slice(0, 600))

    // 学习者看不到错解：负例的内容不进 md
    assert.ok(md.indexOf("WRONG:") === -1, "负例的内容不许进单元 md")

    // 记账：批次汇总落在 settings
    const run = JSON.parse(store.getSetting("supply.batch.run.batch-e2e", "{}"))
    assert.equal(run.verified, 2)
    assert.equal(run.rejected, 0)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("端到端：draft → rejected（负例骗过全部用例，理由说清用例太弱）", { skip: SKIP }, async function () {
  const root = makeGitRoot()
  try {
    bankFile(root, "fundamentals/lc-00002.md", { id: "lc-00002" })
    commitAll(root)

    const client = fakeClient(function () {
      return answerOf(PASSING_NEG_CODE)
    })
    const store = fakeStore()
    const res = await runGenerate(root, client, store, ["--session", "batch-e2e-2"])
    assert.equal(res.code, 0, res.output)

    const md = fs.readFileSync(path.join(root, "units", "algorithm", "lc-00002.md"), "utf8")
    assert.ok(md.indexOf("status: rejected") !== -1, md.slice(0, 600))

    const run = JSON.parse(store.getSetting("supply.batch.run.batch-e2e-2", "{}"))
    assert.equal(run.rejected, 1)
    assert.equal(run.verified, 0)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("存量题库一个字节都不许动（sha256 前后一致）", { skip: SKIP }, async function () {
  const root = makeGitRoot()
  try {
    bankFile(root, "fundamentals/lc-00001.md", { id: "lc-00001" })
    bankFile(root, "interview/lc-00003.md", {
      id: "lc-00003",
      statement: "给你一个字符串，把它反过来输出。",
      cases: [{ input: "123", expect: "321" }],
    })
    commitAll(root)
    const before = digestTree(path.join(root, "bank"))

    const client = fakeClient(function () {
      return answerOf(NEG_CODE)
    })
    const res = await runGenerate(root, client, fakeStore(), ["--session", "batch-e2e-3"])
    assert.equal(res.code, 0, res.output)

    const after = digestTree(path.join(root, "bank"))
    assert.deepEqual(after, before, "learning/bank/** 一个字节都不许改")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("真实 git：learning 仓有别人的未提交改动 → 立刻停下，一个 LLM 都不调", { skip: GIT_OK ? false : "本机没有 git" }, async function () {
  const root = makeGitRoot()
  try {
    bankFile(root, "fundamentals/lc-00001.md", { id: "lc-00001" })
    commitAll(root)
    // 别人在别处改了东西（不在 units/algorithm/ 下）
    fs.mkdirSync(path.join(root, "notes"), { recursive: true })
    fs.writeFileSync(path.join(root, "notes", "别人的笔记.md"), "别人的未提交改动\n", "utf8")

    const client = fakeClient(function () {
      return answerOf(NEG_CODE)
    })
    const res = await runGenerate(root, client, fakeStore(), ["--session", "batch-e2e-4"])
    assert.equal(client.calls, 0, "查脏没过就不许调 LLM")
    assert.ok(res.output.indexOf("未提交改动") !== -1, res.output)
    assert.equal(fs.existsSync(path.join(root, "units", "algorithm", "lc-00001.md")), false, "不许写盘")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("全程不联网：整条链路一次都没碰过 fetch", { skip: SKIP }, async function () {
  const root = makeGitRoot()
  const original = globalThis.fetch
  let fetchCalls = 0
  globalThis.fetch = function () {
    fetchCalls += 1
    throw new Error("测试里不许联网")
  }
  try {
    bankFile(root, "fundamentals/lc-00001.md", { id: "lc-00001" })
    commitAll(root)
    const client = fakeClient(function () {
      return answerOf(NEG_CODE)
    })
    const res = await runGenerate(root, client, fakeStore(), ["--session", "batch-e2e-5"])
    assert.equal(res.code, 0, res.output)
    assert.equal(fetchCalls, 0, "整条链路不许碰 fetch")
    assert.equal(client.calls, 1, "LLM 走的是注入的假客户端")
  } finally {
    globalThis.fetch = original
    fs.rmSync(root, { recursive: true, force: true })
  }
})
