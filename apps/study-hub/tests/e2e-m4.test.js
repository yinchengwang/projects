"use strict"
/**
 * e2e-m4.test.js —— M4 闭环端到端
 *
 * 下一题 → 勾选写回 → 错题订正 → 复习评分，
 * 并验证写回落在 learning 仓的**工作区**、**app 没有自动提交**。
 *
 * 全程跑在临时仓库里（tests/fixtures-m4.js 的 tempLearning），绝不碰真实的 learning 仓。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const { createApp } = require("../server/app.js")
const { tempLearning } = require("./fixtures-m4.js")

/** 一道题（题库文件），放临时仓的 bank/interview/ 下 */
const PROBLEM_MD = [
  "---",
  "id: lc-00001",
  "source: leetcode",
  "title: 两数之和",
  "difficulty: easy",
  "topics: [哈希表, 数组]",
  "tracks: [interview, fundamentals]",
  "status: todo",
  "needs_fill: false",
  "created: 2026-11-11",
  "updated: 2026-11-11",
  "---",
  "## 题面",
  "",
  "给定整数数组与目标值。",
  "",
  "## 测试用例",
  "",
  "| input | expect |",
  "|---|---|",
  "| [2,7,11,15], 9 | [0,1] |",
  "",
  "## 参考解法",
  "",
  "```cpp",
  "int main() { return 0; }",
  "```",
  "",
].join("\n")

/**
 * 工作区状态（相对仓库根的路径，中文不转义）。
 * `-uall` 把未跟踪目录展开成一个个文件——不然新建的 notes/ 只显示成一行 "??"，
 * 看不见里面的订正笔记。
 */
function gitStatus(root) {
  return execFileSync(
    "git",
    ["-c", "core.quotepath=false", "status", "--porcelain", "-uall"],
    { cwd: root, encoding: "utf8" }
  )
}

/** 提交条数 */
function commitCount(root) {
  const out = execFileSync("git", ["rev-list", "--count", "HEAD"], {
    cwd: root,
    encoding: "utf8",
  })
  return Number(String(out).trim())
}

/** 起一次服务，跑完就关 */
async function withApp(fn) {
  const root = tempLearning({ "bank/interview/lc-00001.md": PROBLEM_MD })
  const app = createApp({ dbPath: ":memory:", learningRoot: root, localBank: null })
  const port = await app.listen(0)
  try {
    await fn("http://127.0.0.1:" + port, root)
  } finally {
    await app.close()
  }
}

test("闭环：下一题 → 勾选写回 → 错题订正 → 复习评分，且不自动提交", async function () {
  await withApp(async function (base, root) {
    const commitsBefore = commitCount(root)

    /* 1. 下一题 */
    const nextRes = await fetch(base + "/api/next")
    assert.strictEqual(nextRes.status, 200)
    const next = await nextRes.json()
    assert.ok(next.action, "应该给出下一个动作")
    assert.ok(next.action.reason.length > 0, "要给出中文理由")
    assert.ok(next.action.estimatedMinutes > 0, "要给出预计用时")

    /* 2. 勾选写回：只改目标行 */
    const patchRes = await fetch(base + "/api/rounds/r33/items/arrays", {
      method: "PATCH",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ status: "done", doneDate: "2026-11-12" }),
    })
    assert.strictEqual(patchRes.status, 200)
    const patched = await patchRes.json()
    assert.ok(patched.diff.indexOf("arrays") !== -1, patched.diff)

    const roundText = fs.readFileSync(path.join(root, "progress", "r33-progress.md"), "utf8")
    assert.ok(
      roundText.indexOf("| 2 | arrays | 数组技巧 | done | 2026-11-12 |") !== -1,
      roundText
    )
    assert.ok(roundText.indexOf("| 1 | big_o | 大 O 分析 | done | 2026-07-12 |") !== -1, "别的行不能动")
    assert.ok(roundText.indexOf("| 3 | linked_lists | 链表技巧 | todo |  |") !== -1, "别的行不能动")

    /* 3. 错题订正 */
    const corrRes = await fetch(base + "/api/correction", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ problemId: "lc-00001", text: "先查再插，别先插再查。" }),
    })
    assert.strictEqual(corrRes.status, 200)
    const corr = await corrRes.json()
    assert.ok(fs.existsSync(corr.file), "订正笔记要真的写出来")
    assert.ok(fs.readFileSync(corr.file, "utf8").indexOf("先查再插") !== -1)

    /* 4. 复习评分 */
    const gradeRes = await fetch(base + "/api/review/lc-00001/grade", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ quality: 3 }),
    })
    assert.strictEqual(gradeRes.status, 200)
    const graded = await gradeRes.json()
    assert.strictEqual(graded.problemId, "lc-00001")
    assert.strictEqual(graded.intervalDays, 1)

    /* 5. 写回落在工作区，app 没有自动提交 */
    const status = gitStatus(root)
    assert.ok(status.indexOf("r33-progress.md") !== -1, status)
    assert.ok(status.indexOf("订正") !== -1, status)
    assert.strictEqual(commitCount(root), commitsBefore, "app 不应该自动提交")
  })
})

test("learning 仓脏的时候写回被拒绝，仓库内容一个字节都不动", async function () {
  await withApp(async function (base, root) {
    const file = path.join(root, "progress", "r33-progress.md")
    const before = fs.readFileSync(file, "utf8")
    fs.writeFileSync(path.join(root, "别的改动.md"), "手工改的\n", "utf8")

    const res = await fetch(base + "/api/rounds/r33/items/arrays", {
      method: "PATCH",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ status: "done" }),
    })
    assert.strictEqual(res.status, 409)
    const body = await res.json()
    assert.ok(body.dirtyFiles.length > 0)
    assert.strictEqual(fs.readFileSync(file, "utf8"), before, "被拒绝时不能改动文件")
  })
})
