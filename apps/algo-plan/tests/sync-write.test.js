"use strict"
/**
 * sync-write.test.js —— 写回 learning 仓：查脏 / 勾选写回 / 错题订正
 *
 * 全部在 fs.mkdtemp 出来的**临时副本**上跑，绝不碰真实的 learning 仓。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const rounds = require("../core/sync/rounds.js")
const fx = require("./fixtures-m4.js")

/** 造一个干净的临时 learning 仓（带 git 仓库与一个轮次文件） */
function tempLearning() {
  return fx.tempLearning()
}

/** 把仓库弄脏 */
function dirty(root, name) {
  fx.dirty(root, name)
}

const PROBLEM = fx.PROBLEM

test("工作区干净时 assertCleanWorktree 返回 ok", function () {
  const root = tempLearning()
  const result = rounds.assertCleanWorktree(root)
  assert.strictEqual(result.ok, true)
  assert.deepStrictEqual(result.dirtyFiles, [])
})

test("工作区脏时 assertCleanWorktree 指出被动过的文件", function () {
  const root = tempLearning()
  dirty(root, path.join("progress", "r33-progress.md"))
  const result = rounds.assertCleanWorktree(root)
  assert.strictEqual(result.ok, false)
  assert.ok(result.dirtyFiles.length > 0)
  assert.ok(
    result.dirtyFiles.some(function (f) { return f.indexOf("r33-progress.md") !== -1 }),
    result.dirtyFiles.join("|")
  )
})

test("不是 git 仓库时返回 ok=false 而不是抛错", function () {
  const dir = require("./helpers/tmp.js").tmpDir("algo-plan-nogit-")
  const result = rounds.assertCleanWorktree(dir)
  assert.strictEqual(result.ok, false)
  assert.ok(result.reason.length > 0, "应给出中文原因")
})

test("勾选写回只改目标行，其它行逐字节不变", function () {
  const root = tempLearning()
  const file = path.join(root, "progress", "r33-progress.md")
  const before = fs.readFileSync(file, "utf8").split("\n")

  const result = rounds.updateRoundItem(path.join(root, "progress"), "r33", "arrays", {
    status: "done",
    doneDate: "2026-11-11",
  })

  const after = fs.readFileSync(file, "utf8").split("\n")
  assert.strictEqual(after.length, before.length, "行数不该变")
  let changed = 0
  for (let i = 0; i < before.length; i += 1) {
    if (before[i] !== after[i]) {
      changed += 1
      assert.ok(before[i].indexOf("arrays") !== -1, "只有目标行允许变：" + before[i])
      assert.ok(after[i].indexOf("arrays") !== -1)
      assert.ok(after[i].indexOf("done") !== -1, "目标行应写入 done：" + after[i])
    }
  }
  assert.strictEqual(changed, 1, "只应改一行")
  assert.ok(result.diff.indexOf("arrays") !== -1, "diff 里应能看到目标行")
  assert.ok(result.diff.indexOf("--- a/") === 0, "diff 应有统一的头部")
})

test("勾选写回在脏工作区时拒绝并指出被动过的文件", function () {
  const root = tempLearning()
  dirty(root, path.join("notes", "随手改的.md"))
  assert.throws(
    function () {
      rounds.updateRoundItem(path.join(root, "progress"), "r33", "arrays", { status: "done" })
    },
    function (err) {
      assert.strictEqual(err.status, 409)
      assert.ok(err.dirtyFiles.length > 0)
      assert.ok(err.message.indexOf("不干净") !== -1, err.message)
      return true
    }
  )
})

test("条目不存在时返回 404 语义的错误", function () {
  const root = tempLearning()
  assert.throws(
    function () {
      rounds.updateRoundItem(path.join(root, "progress"), "r33", "不存在的条目", { status: "done" })
    },
    function (err) {
      return err.status === 404
    }
  )
})

test("轮次不存在时返回 404 语义的错误", function () {
  const root = tempLearning()
  assert.throws(
    function () {
      rounds.updateRoundItem(path.join(root, "progress"), "r99", "arrays", { status: "done" })
    },
    function (err) {
      return err.status === 404
    }
  )
})

test("表里没有「完成时间」列时写告警而不是新增列", function () {
  const root = tempLearning()
  fs.writeFileSync(path.join(root, "progress", "r30-progress.md"), fx.ROUND_FILE_NO_DONE_DATE, "utf8")
  execFileSync("git", ["add", "-A"], { cwd: root, stdio: ["ignore", "pipe", "pipe"] })
  execFileSync("git", ["commit", "-q", "-m", "补 r30"], { cwd: root, stdio: ["ignore", "pipe", "pipe"] })

  const result = rounds.updateRoundItem(path.join(root, "progress"), "r30", "basic_types", {
    status: "done",
    doneDate: "2026-11-11",
  })
  assert.ok(result.warnings.length > 0, "应有告警")
  assert.ok(result.warnings.some(function (w) { return w.indexOf("完成时间") !== -1 }), result.warnings.join("|"))
  const text = fs.readFileSync(path.join(root, "progress", "r30-progress.md"), "utf8")
  assert.ok(text.indexOf("| basic_types | 基础类型 | done |") !== -1, text)
  assert.ok(text.indexOf("完成时间") === -1, "不该新增列：" + text)
})

test("错题订正新建文件：路径、frontmatter 键序、正文结构", function () {
  const root = tempLearning()
  const result = rounds.writeCorrection(root, PROBLEM, "哈希表一次遍历就能出解，先查再插。")

  const expected = path.join(root, "notes", "practice", "lc-00001-订正.md")
  assert.strictEqual(result.file, expected)
  assert.ok(fs.existsSync(expected))
  assert.ok(result.diff.indexOf("--- a/") === 0, "新文件也要给 diff")

  const text = fs.readFileSync(expected, "utf8")
  const keys = text
    .split("---")[1]
    .trim()
    .split("\n")
    .map(function (line) { return line.split(":")[0] })
  assert.deepStrictEqual(keys, [
    "stack",
    "difficulty",
    "status",
    "links",
    "ref",
    "created",
    "updated",
    "tags",
    "unresolved",
    "problem",
  ])
  assert.ok(text.indexOf("stack: c") !== -1, "interview 轨道映射到 stack: c")
  assert.ok(text.indexOf("difficulty: medium") !== -1)
  assert.ok(text.indexOf("tags: [practice, correction]") !== -1)
  assert.ok(text.indexOf("unresolved: true") !== -1)
  assert.ok(text.indexOf("problem: lc-00001") !== -1)
  assert.ok(text.indexOf("## 订正 ") !== -1)
  assert.ok(text.indexOf("先查再插") !== -1)
  assert.ok(text.indexOf("bank/interview/lc-00001.md") !== -1)
})

test("错题订正对已存在的文件是追加，不覆盖原内容", function () {
  const root = tempLearning()
  rounds.writeCorrection(root, PROBLEM, "第一次订正")
  const file = path.join(root, "notes", "practice", "lc-00001-订正.md")
  rounds.writeCorrection(root, PROBLEM, "第二次订正")

  const text = fs.readFileSync(file, "utf8")
  assert.ok(text.indexOf("第一次订正") !== -1, "第一次的内容要保留")
  assert.ok(text.indexOf("第二次订正") !== -1, "第二次的内容要追加")
  assert.strictEqual(text.split("## 订正 ").length - 1, 2, "应有两段订正")
})

test("错题订正在脏工作区时拒绝写入", function () {
  const root = tempLearning()
  dirty(root, path.join("notes", "随手改的.md"))
  assert.throws(
    function () {
      rounds.writeCorrection(root, PROBLEM, "不该写进去")
    },
    function (err) {
      return err.status === 409
    }
  )
  assert.strictEqual(fs.existsSync(path.join(root, "notes", "practice", "lc-00001-订正.md")), false)
})

test("错题订正追加时沿用原文件的行尾符（CRLF 文件不写成混合行尾）", function () {
  const root = tempLearning()
  const file = path.join(root, "notes", "practice", "lc-00001-订正.md")
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, "# 手写的订正\r\n\r\n已有内容\r\n", "utf8")
  fx.commitAll(root, "先提交手写的那一篇")

  rounds.writeCorrection(root, PROBLEM, "追加的一段")

  const text = fs.readFileSync(file, "utf8")
  assert.ok(text.indexOf("已有内容") !== -1, "原内容要保留")
  assert.ok(text.indexOf("追加的一段") !== -1, "新内容要追加")
  assert.strictEqual(text.indexOf("\n\n"), -1, "不该混入裸 LF：" + JSON.stringify(text.slice(0, 40)))
})

test("diffLines：单行改动给出一删一增", function () {
  const diff = rounds.diffLines("a\nb\nc\n", "a\nB\nc\n")
  assert.strictEqual(diff.removed, 1)
  assert.strictEqual(diff.added, 1)
  assert.ok(diff.lines.indexOf("-b") !== -1, diff.lines.join("|"))
  assert.ok(diff.lines.indexOf("+B") !== -1, diff.lines.join("|"))
})

/* ==================== 计划之外的补充断言 ==================== */

test("写回绝不自动 commit：仓库里仍然只有初始化那一次提交", function () {
  const root = tempLearning()
  rounds.updateRoundItem(path.join(root, "progress"), "r33", "arrays", { status: "done" })
  const log = execFileSync("git", ["log", "--oneline"], { cwd: root, encoding: "utf8" }).trim().split("\n")
  assert.strictEqual(log.length, 1, "不该有第二个提交：" + log.join("|"))
  const status = execFileSync("git", ["status", "--porcelain"], { cwd: root, encoding: "utf8" })
  assert.ok(status.indexOf("r33-progress.md") !== -1, "改动应留在工作区等用户决定：" + status)
})

test("写回只动目标单元格：目标行的其它格与原来一模一样", function () {
  const root = tempLearning()
  const file = path.join(root, "progress", "r33-progress.md")
  const beforeLine = fs
    .readFileSync(file, "utf8")
    .split("\n")
    .filter(function (line) { return line.indexOf("arrays") !== -1 })[0]

  rounds.updateRoundItem(path.join(root, "progress"), "r33", "arrays", { status: "done" })

  const afterLine = fs
    .readFileSync(file, "utf8")
    .split("\n")
    .filter(function (line) { return line.indexOf("arrays") !== -1 })[0]
  assert.strictEqual(beforeLine.indexOf("| 2 |"), afterLine.indexOf("| 2 |"), "序号列不该变")
  assert.ok(afterLine.indexOf("| 数组技巧 |") !== -1, "名称列不该变：" + afterLine)
  assert.ok(afterLine.indexOf("| done |") !== -1, "状态列应被写：" + afterLine)
})

test("CRLF 文件写回后行尾符仍是 CRLF，其它行逐字节不变", function () {
  const root = fx.tempLearning({
    "progress/r33-progress.md": fx.ROUND_FILE.replace(/\n/g, "\r\n"),
  })
  const file = path.join(root, "progress", "r33-progress.md")
  const before = fs.readFileSync(file, "utf8")

  rounds.updateRoundItem(path.join(root, "progress"), "r33", "arrays", { status: "done" })

  const after = fs.readFileSync(file, "utf8")
  assert.ok(after.indexOf("\r\n") !== -1, "应保留 CRLF")
  assert.strictEqual(after.indexOf("\n\n"), -1, "不该混入裸 LF")
  const beforeLines = before.split("\r\n")
  const afterLines = after.split("\r\n")
  assert.strictEqual(beforeLines.length, afterLines.length, "行数不该变")
  let changed = 0
  for (let i = 0; i < beforeLines.length; i += 1) {
    if (beforeLines[i] !== afterLines[i]) {
      changed += 1
      assert.ok(beforeLines[i].indexOf("arrays") !== -1, "只有目标行允许变：" + beforeLines[i])
    }
  }
  assert.strictEqual(changed, 1, "只应改一行")
})

test("连着写两处不会被 app 自己的未提交改动挡住（自己写的脏不算脏）", function () {
  const root = tempLearning()
  const first = rounds.updateRoundItem(path.join(root, "progress"), "r33", "arrays", { status: "done" })
  assert.ok(first.diff.indexOf("arrays") !== -1)

  const second = rounds.updateRoundItem(path.join(root, "progress"), "r33", "linked_lists", {
    status: "done",
    doneDate: "2026-11-11",
  })
  assert.ok(second.diff.indexOf("linked_lists") !== -1, "第二次写回也该成功")
  const text = fs.readFileSync(path.join(root, "progress", "r33-progress.md"), "utf8")
  assert.ok(text.indexOf("| 2 | arrays | 数组技巧 | done |") !== -1, text)
  assert.ok(text.indexOf("| 3 | linked_lists | 链表技巧 | done | 2026-11-11 |") !== -1, text)
})

test("别人动过的文件即使和目标无关也拒绝写回（409 + 文件清单）", function () {
  const root = tempLearning()
  dirty(root, path.join("notes", "随手改的.md"))
  let caught = null
  try {
    rounds.updateRoundItem(path.join(root, "progress"), "r33", "arrays", { status: "done" })
  } catch (err) {
    caught = err
  }
  assert.ok(caught, "应当抛错")
  assert.strictEqual(caught.status, 409)
  assert.ok(caught.dirtyFiles.some(function (f) { return f.indexOf("随手改的.md") !== -1 }), caught.dirtyFiles.join("|"))
})

test("查脏按 NUL 切分：带空格与中文的路径都能原样取出（G20）", function () {
  const root = tempLearning()
  dirty(root, path.join("notes", "带 空格 的笔记.md"))
  dirty(root, path.join("notes", "中文笔记.md"))
  const result = rounds.assertCleanWorktree(root)
  assert.strictEqual(result.ok, false)
  assert.ok(
    result.dirtyFiles.some(function (f) { return f === "notes/带 空格 的笔记.md" }),
    JSON.stringify(result.dirtyFiles)
  )
  assert.ok(
    result.dirtyFiles.some(function (f) { return f === "notes/中文笔记.md" }),
    JSON.stringify(result.dirtyFiles)
  )
})

test("重命名记录的两段路径都能查出来（-z 模式下旧路径是独立一段）", function () {
  const root = tempLearning()
  execFileSync("git", ["mv", "progress/r33-progress.md", "progress/r33-改名.md"], {
    cwd: root,
    stdio: ["ignore", "pipe", "pipe"],
  })
  const result = rounds.assertCleanWorktree(root)
  assert.strictEqual(result.ok, false)
  assert.ok(
    result.dirtyFiles.some(function (f) { return f === "progress/r33-改名.md" }),
    JSON.stringify(result.dirtyFiles)
  )
  assert.ok(
    result.dirtyFiles.some(function (f) { return f === "progress/r33-progress.md" }),
    JSON.stringify(result.dirtyFiles)
  )
})

test("stackForProblem：按 track 映射到笔记 schema 的 stack 枚举", function () {
  assert.strictEqual(rounds.stackForProblem({ tracks: ["interview"] }), "c")
  assert.strictEqual(rounds.stackForProblem({ tracks: ["fundamentals"] }), "ds")
  assert.strictEqual(rounds.stackForProblem({ tracks: ["advanced"] }), "ds")
  assert.strictEqual(rounds.stackForProblem({ tracks: ["engineering"] }), "db")
  assert.strictEqual(rounds.stackForProblem({ tracks: [] }), "c", "没有 track 时兜底")
})
