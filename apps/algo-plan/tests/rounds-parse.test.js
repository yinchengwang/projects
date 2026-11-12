"use strict"
/**
 * rounds-parse.test.js —— 轮次解析：7 种真实表头 + 缺列 + 坏行
 *
 * 夹具里的表头与数据行是从 learning/progress/ 的 9 个真实轮次文件里**原样摘出来的**，
 * 不许改成理想格式——真实文件就是乱的。勘察确认有 7 种表头变体：
 *   r25「ID | 卡片 | 主题 | 状态」
 *   r26「# | 卡片 ID | 主题 | 状态」
 *   r30「卡 ID | 名称 | 状态」
 *   r32「时间戳 | 卡 ID | commit SHA | scaffold 路径 | NOTES 路径 | 审核人」（**整张表没有「状态」列**）
 *   r33「序号 | ID | 名称 | 状态 | 完成时间」
 *   r35「序号 | ID | 名称 | 状态 | 完成时间 | commit SHA」
 *   r36/r37/r38「序号 | ID | 名称 | 语言 | 状态 | 完成时间 | commit SHA」
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const rounds = require("../core/sync/rounds.js")
const fx = require("./fixtures-m4.js")

const REAL_TABLES = fx.REAL_TABLES

/** 造一个临时 progress 目录 */
function tempProgress(files) {
  return fx.tempProgress(files)
}

/** 造一个只含清单表的轮次文件 */
function fileWithTable(header, rows) {
  return fx.fileWithTable(header, rows)
}

test("7 种真实表头都能按列名关键词解析出 id / 标题 / 状态", function () {
  REAL_TABLES.forEach(function (spec) {
    const dir = tempProgress({ "r99-progress.md": fileWithTable(spec.header, [spec.row]) })
    const parsed = rounds.parseRounds(dir)
    assert.strictEqual(parsed.rounds.length, 1, spec.name + "：应解析出 1 个轮次")
    const items = parsed.rounds[0].items
    assert.strictEqual(items.length, 1, spec.name + "：应解析出 1 个条目")
    const item = items[0]
    assert.strictEqual(item.id, spec.expect.id, spec.name + "：id 解析错了")
    assert.strictEqual(item.title, spec.expect.title, spec.name + "：标题解析错了")
    assert.strictEqual(item.status, spec.expect.status, spec.name + "：状态解析错了")
    assert.strictEqual(item.doneDate, spec.expect.doneDate, spec.name + "：完成时间解析错了")
    assert.strictEqual(item.commitSha, spec.expect.commitSha, spec.name + "：commit SHA 解析错了")
  })
})

test("缺列的行照常解析，缺的字段填空值", function () {
  const dir = tempProgress({
    "r99-progress.md": fileWithTable("| 卡片 ID | 名称 |", ["| dp_basic | 动态规划基础 |"]),
  })
  const parsed = rounds.parseRounds(dir)
  const item = parsed.rounds[0].items[0]
  assert.strictEqual(item.id, "dp_basic")
  assert.strictEqual(item.title, "动态规划基础")
  assert.strictEqual(item.status, "todo", "没有状态列 → 视为未完成")
  assert.strictEqual(item.doneDate, "")
  assert.strictEqual(item.commitSha, "")
})

test("坏行被跳过并计入告警，其它行不受影响", function () {
  const dir = tempProgress({
    "r99-progress.md": fileWithTable("| 卡片 ID | 名称 | 状态 |", [
      "| dp_basic | 动态规划基础 | done |",
      "| 这行坏了 |",
      "| graph_shortest | 图上最短路 | todo |",
    ]),
  })
  const parsed = rounds.parseRounds(dir)
  assert.strictEqual(parsed.rounds[0].items.length, 2, "坏行应被跳过，其余两行保留")
  assert.ok(parsed.warnings.length > 0, "坏行应产生告警")
  assert.ok(parsed.warnings.some(function (w) { return w.indexOf("跳过") !== -1 }), parsed.warnings.join("|"))
})

test("没有 ID 的行被跳过并计入告警（条目靠 ID 定位）", function () {
  const dir = tempProgress({
    "r99-progress.md": fileWithTable("| 卡片 ID | 名称 | 状态 |", [
      "|  | 没有 ID 的行 | done |",
      "| ok_item | 正常行 | done |",
    ]),
  })
  const parsed = rounds.parseRounds(dir)
  assert.strictEqual(parsed.rounds[0].items.length, 1)
  assert.strictEqual(parsed.rounds[0].items[0].id, "ok_item")
  assert.ok(parsed.warnings.length > 0)
})

test("轮次 ID 从文件名取，标题从一级标题取", function () {
  const dir = tempProgress({
    "r33-progress.md": [
      "# R33 Grokking 算法篇 — 学习进度",
      "",
      "## 基本信息",
      "",
      "| 字段 | 值 |",
      "|------|-----|",
      "| 开始时间 | 2026-07-12 |",
      "| 目标 | Grokking 算法篇 16 张卡片 |",
      "| 状态 | TBD-deferred |",
      "",
      "| 序号 | ID | 名称 | 状态 | 完成时间 |",
      "|------|-----|------|------|----------|",
      "| 1 | big_o | 大 O 分析 | done | 2026-07-12 |",
      "| 2 | arrays | 数组技巧 | done | 2026-07-12 |",
    ].join("\n"),
  })
  const parsed = rounds.parseRounds(dir)
  const round = parsed.rounds[0]
  assert.strictEqual(round.id, "r33")
  assert.strictEqual(round.title, "R33 Grokking 算法篇 — 学习进度")
  assert.strictEqual(round.goal, "Grokking 算法篇 16 张卡片")
  assert.strictEqual(round.startedAt, "2026-07-12")
  assert.strictEqual(round.items.length, 2)
})

test("基本信息里的「状态」是 TBD-deferred 时轮次算 active", function () {
  const dir = tempProgress({
    "r33-progress.md": [
      "# R33 Grokking 算法篇 — 学习进度",
      "",
      "| 字段 | 值 |",
      "|------|-----|",
      "| 状态 | TBD-deferred |",
      "",
      "| 序号 | ID | 名称 | 状态 |",
      "|------|-----|------|------|",
      "| 1 | big_o | 大 O 分析 | done |",
    ].join("\n"),
  })
  const parsed = rounds.parseRounds(dir)
  assert.strictEqual(parsed.rounds[0].status, "active")
})

test("没有状态字段且条目全部 done 时轮次算 done", function () {
  const dir = tempProgress({
    "r25-progress.md": [
      "# R25 Linux 网络基础 — 学习进度",
      "",
      "## 基本信息",
      "",
      "- **开始日期**: 2026-07-12",
      "- **主题**: Linux 网络基础",
      "- **卡数**: 10 张",
      "",
      "| ID | 卡片 | 主题 | 状态 |",
      "|----|------|------|------|",
      "| 1 | epoll | epoll 事件驱动 | done |",
      "| 2 | zero_copy | 零拷贝技术 | done |",
    ].join("\n"),
  })
  const parsed = rounds.parseRounds(dir)
  const round = parsed.rounds[0]
  assert.strictEqual(round.status, "done")
  assert.strictEqual(round.goal, "Linux 网络基础", "无序列表形式的「主题」也要认")
  assert.strictEqual(round.startedAt, "2026-07-12")
})

test("没有清单表的轮次返回空条目并给告警，不整体报错", function () {
  const dir = tempProgress({
    "r99-progress.md": ["# R99 只有标题", "", "## 基本信息", "", "- **主题**: 空轮次"].join("\n"),
  })
  const parsed = rounds.parseRounds(dir)
  assert.strictEqual(parsed.rounds.length, 1)
  assert.deepStrictEqual(parsed.rounds[0].items, [])
  assert.ok(parsed.warnings.length > 0)
})

test("轮次目录不存在时返回空列表 + 告警，不抛错", function () {
  const parsed = rounds.parseRounds(path.join(require("node:os").tmpdir(), "algo-plan-不存在-" + Date.now()))
  assert.deepStrictEqual(parsed.rounds, [])
  assert.ok(parsed.warnings.length > 0)
})

test("多个轮次按文件名排序返回", function () {
  const dir = tempProgress({
    "r38-progress.md": fileWithTable("| 卡片 ID | 名称 | 状态 |", ["| a | 甲 | done |"]),
    "r33-progress.md": fileWithTable("| 卡片 ID | 名称 | 状态 |", ["| b | 乙 | done |"]),
  })
  const parsed = rounds.parseRounds(dir)
  assert.deepStrictEqual(parsed.rounds.map(function (r) { return r.id }), ["r33", "r38"])
})

test("非 rNN 开头的文件不当轮次解析", function () {
  const dir = tempProgress({
    "README.md": "# 说明\n\n不是轮次。\n",
    "notes.md": "# 别的\n\n| 卡片 ID | 名称 | 状态 |\n|---|---|---|\n| x | 甲 | done |\n",
    "r33-progress.md": fileWithTable("| 卡片 ID | 名称 | 状态 |", ["| b | 乙 | done |"]),
  })
  const parsed = rounds.parseRounds(dir)
  assert.deepStrictEqual(parsed.rounds.map(function (r) { return r.id }), ["r33"])
})

test("CRLF 行尾的轮次文件照常解析（真实 learning 仓就是 CRLF）", function () {
  const dir = tempProgress({
    "r33-progress.md": fx.ROUND_FILE.replace(/\n/g, "\r\n"),
  })
  const parsed = rounds.parseRounds(dir)
  assert.strictEqual(parsed.rounds.length, 1)
  assert.strictEqual(parsed.rounds[0].items.length, 3)
  assert.strictEqual(parsed.rounds[0].items[1].id, "arrays")
  assert.strictEqual(parsed.rounds[0].items[1].status, "todo")
})

test("「完成日期」也算完成时间列（设计文档 §8 提到的变体）", function () {
  const dir = tempProgress({
    "r99-progress.md": fileWithTable("| 序号 | ID | 名称 | 状态 | 完成日期 |", [
      "| 1 | big_o | 大 O 分析 | done | 2026-07-12 |",
    ]),
  })
  const parsed = rounds.parseRounds(dir)
  assert.strictEqual(parsed.rounds[0].items[0].doneDate, "2026-07-12")
})

test("parseRoundText 直接吃文本也能解析（写回逻辑复用它定位目标行）", function () {
  const parsed = rounds.parseRoundText("/tmp/r33-progress.md", fx.ROUND_FILE)
  assert.strictEqual(parsed.round.id, "r33")
  assert.strictEqual(parsed.round.items.length, 3)
  assert.deepStrictEqual(
    parsed.round.items.map(function (item) { return item.id }),
    ["big_o", "arrays", "linked_lists"]
  )
})

test("解析 learning 仓里真实轮次文件的目录（只读，不写任何东西）", function (t) {
  const realRoot = process.env.ALGO_PLAN_LEARNING_ROOT
  if (!realRoot) {
    t.skip("未设置 ALGO_PLAN_LEARNING_ROOT，跳过真实仓只读校验")
    return
  }
  const dir = path.join(realRoot, "progress")
  if (!fs.existsSync(dir)) {
    t.skip("真实仓里没有 progress 目录，跳过")
    return
  }
  const parsed = rounds.parseRounds(dir)
  assert.ok(parsed.rounds.length > 0, "真实仓里至少应解析出一个轮次")
  parsed.rounds.forEach(function (round) {
    assert.ok(round.id, "轮次应有 ID：" + round.file)
    round.items.forEach(function (item) {
      assert.ok(item.id, "条目应有 ID：" + round.file)
    })
  })
})
