"use strict"
/**
 * acceptance.js —— 算法轨交付验收（把计划 Task 10 的 12 条自查表变成一次命令）
 *
 * 设计取舍：
 *   - 能在文件层面查的，自己查（复用 audit.js）；
 *   - 需要真编译的（第 3 条），默认不跑，只有显式 `verify: true` 才跑——
 *     全量 738 道的复核要 40–70 分钟，不该混进一条随手跑的验收命令；
 *   - 需要人看或要看 git 远端的（第 11 条「全量测试」，第 12 条「已推送」），
 *     标成 manual 并写清「怎么查」，不假装机器已经确认。
 *
 * 行的三种状态：
 *   ok     —— 机器查过，符合期望
 *   bad    —— 机器查过，不符合期望
 *   manual —— 需要人确认或要跑外部命令，本命令只把查法打出来
 *
 * 整体 `ok` 的判据：**一条 bad 都没有**。manual 不算失败——
 * 但会一直挂着，提醒你还有没确认过的东西。
 */

const fs = require("node:fs")
const path = require("node:path")

const fill = require("./algorithm-fill.js")
const batch = require("./algorithm-batch.js")
const audit = require("./audit.js")

/** 12 条验收项（编号与计划 Task 10 Step 10 的自查表逐条对应） */
const ROWS = [
  { no: 1, name: "单元文件批量产出", how: "units/algorithm 下 .md 的个数" },
  { no: 2, name: "三件套成对", how: ".reference.cpp 与 .negative.cpp 各与 .md 同数" },
  { no: 3, name: "每份都经过真实编译执行", how: "node tools/supply.js verify --track algorithm" },
  { no: 4, name: "负例被拒", how: "verify 输出里没有「负例竟然全部通过」" },
  { no: 5, name: "覆盖报告缺口降下来", how: "report --track algorithm 的「还欠」" },
  { no: 6, name: "plan 的缺口降下来", how: "plan --track algorithm 的「待补」" },
  { no: 7, name: "存量题库零改动", how: "git status --porcelain -- bank/" },
  { no: 8, name: "单元三件套同目录同 stem", how: "没有孤儿文件" },
  { no: 9, name: "负例内容不进 md", how: "审计的 negative-leak / reference-drift" },
  { no: 10, name: "密钥没进产物", how: "审计的 secret 检查" },
  { no: 11, name: "全量测试全绿", how: "node tests/run-all.js" },
  { no: 12, name: "learning 仓已推且工作树干净", how: "git log --oneline -1 / git status -sb" },
]

/** 真跑一次 git status（porcelain -z 按 NUL 切分） */
function runGitStatus(root, extraArgs) {
  const args = ["-c", "core.quotepath=false", "status", "--porcelain", "-z", "--untracked-files=all"]
  const res = require("node:child_process").spawnSync("git", args.concat(extraArgs || []), {
    cwd: root,
    encoding: "utf8",
    windowsHide: true,
    maxBuffer: 8 * 1024 * 1024,
  })
  if (res.error) return { ok: false, entries: [], reason: String(res.error.message || res.error) }
  const entries = String(res.stdout || "")
    .split("\0")
    .filter(function (s) { return s !== "" })
  return { ok: res.status === 0, entries: entries, reason: "" }
}

/** porcelain 的一行取路径 */
function entryPath(entry) {
  const text = String(entry || "")
  return text.length < 4 ? text : text.slice(3)
}

/**
 * 跑一遍验收。
 *
 * @param {{
 *   learningRoot: string, localBank?: string|null, unitsDir?: string,
 *   verify?: boolean, toolchain?: object,
 *   gitRunner?: Function,
 *   plan?: object|null  —— 传进来就复用，不重复扫盘
 * }} params
 * @returns {Promise<{ok: boolean, rows: Array, counts: object, notes: string[]}>}
 */
async function runAcceptance(params) {
  const p = params || {}
  const learningRoot = String(p.learningRoot || "")
  const unitsDir = String(p.unitsDir || path.join(learningRoot, "units", "algorithm"))
  const notes = []

  const scan = fill.scanBank({ learningRoot: learningRoot, localBank: p.localBank || null })
  const unitStatuses = fill.readUnitStatuses(unitsDir)
  const covered = batch.buildCoverageReport({ items: scan.items, store: p.store, units: unitStatuses })
  const planned = batch.planBatch({ items: scan.items, store: p.store, limit: null, units: unitStatuses })
  const audited = audit.auditAlgorithmUnits({ unitsDir: unitsDir, learningRoot: learningRoot })

  const gitRunner = typeof p.gitRunner === "function" ? p.gitRunner : runGitStatus
  const bankStatus = gitRunner(learningRoot, ["--", "bank/"])
  const treeStatus = gitRunner(learningRoot, [])

  const counts = {
    expected: covered.debt + covered.filled + covered.bankReady - covered.bankReady,
    md: audited.counts.md,
    reference: audited.counts.reference,
    negative: audited.counts.negative,
    verified: audited.counts.verified,
    debt: covered.debt,
    planned: planned.planned.length,
    problems: audited.problems.length,
  }

  const rows = []

  function push(no, status, actual, detail) {
    const row = ROWS.find(function (r) { return r.no === no })
    rows.push({
      no: no,
      name: row ? row.name : "（未知项）",
      how: row ? row.how : "",
      status: status,
      actual: actual,
      detail: detail || "",
    })
  }

  // —— 1. 单元文件产出 ——
  const expectedUnits = covered.debt + covered.filled
  push(
    1,
    counts.md === expectedUnits ? "ok" : "bad",
    counts.md + " / 期望 " + expectedUnits,
    counts.md === 0 ? "还没跑批量补题" : ""
  )

  // —— 2. 三件套成对 ——
  const tripletOk = counts.reference === counts.md && counts.negative === counts.md
  push(
    2,
    tripletOk ? "ok" : "bad",
    "md " + counts.md + " / reference " + counts.reference + " / negative " + counts.negative,
    tripletOk ? "" : "三件套数量对不上，见审计明细"
  )

  // —— 3. 真实编译执行 ——
  if (!p.verify) {
    push(3, "manual", "未跑", "要真编译：node tools/supply.js verify --track algorithm（全量约 40–70 分钟）")
  } else {
    const { verifyAlgorithmUnits } = require("./algorithm-verify.js")
    const summary = await verifyAlgorithmUnits({
      unitsDir: unitsDir,
      learningRoot: learningRoot,
      toolchain: p.toolchain,
      write: true,
    })
    const pass = summary.verified === counts.md && summary.rejected === 0
    push(
      3,
      pass ? "ok" : "bad",
      "通过 " + summary.verified + " / 被拒 " + summary.rejected + " / 共 " + summary.checked,
      pass ? "" : "有单元没通过，见 verify 输出"
    )
    // —— 4. 负例被拒（就是 verify 的 rejected 里有没有「负例竟然全部通过」）——
    const weak = summary.results.filter(function (r) {
      return String(r.reason || "").indexOf("负例竟然全部通过") !== -1
    })
    push(
      4,
      weak.length === 0 ? "ok" : "bad",
      weak.length === 0 ? "0 条" : weak.length + " 条：" + weak.slice(0, 3).map(function (r) { return r.unit }).join("、"),
      ""
    )
  }
  if (!p.verify) {
    push(4, "manual", "未跑", "同上，第 3 条跑完才知道")
  }

  // —— 5. 覆盖报告还欠 ——
  push(
    5,
    covered.debt === 0 ? "ok" : "bad",
    "还欠 " + covered.debt,
    covered.debt === 0 ? "" : "还有题没补完"
  )

  // —— 6. plan 待补 ——
  push(
    6,
    planned.planned.length === 0 ? "ok" : "bad",
    "待补 " + planned.planned.length,
    planned.planned.length === 0
      ? ""
      : "头几道：" + planned.planned.slice(0, 5).map(function (i) { return i.stem }).join("、")
  )

  // —— 7. 存量题库零改动 ——
  if (!bankStatus.ok) {
    push(7, "manual", "查不了", bankStatus.reason)
  } else {
    push(
      7,
      bankStatus.entries.length === 0 ? "ok" : "bad",
      bankStatus.entries.length === 0 ? "0 处改动" : bankStatus.entries.length + " 处改动",
      bankStatus.entries.length === 0 ? "" : bankStatus.entries.slice(0, 3).map(entryPath).join("、")
    )
  }

  // —— 8/9/10. 审计三类问题 ——
  const byKind = function (kind) {
    return audited.problems.filter(function (item) { return item.kind === kind })
  }
  const orphans = byKind("orphan-file").concat(byKind("missing-file"))
  push(8, orphans.length === 0 ? "ok" : "bad", orphans.length + " 处", orphans.length ? orphans[0].detail : "")
  const leaks = byKind("negative-leak").concat(byKind("reference-drift")).concat(byKind("reference-missing"))
  push(9, leaks.length === 0 ? "ok" : "bad", leaks.length + " 处", leaks.length ? leaks[0].detail : "")
  const secrets = byKind("secret")
  push(10, secrets.length === 0 ? "ok" : "bad", secrets.length + " 处", secrets.length ? secrets[0].detail : "")

  // —— 11. 全量测试 ——
  push(11, "manual", "未跑", "node tests/run-all.js（本命令不替你跑测试）")

  // —— 12. learning 仓已推且干净 ——
  if (!treeStatus.ok) {
    push(12, "manual", "查不了", treeStatus.reason)
  } else {
    const allowed = treeStatus.entries.filter(function (entry) {
      return entryPath(entry).replace(/\\/g, "/").indexOf("units/algorithm/") === 0
    })
    const others = treeStatus.entries.filter(function (entry) {
      return allowed.indexOf(entry) === -1
    })
    push(
      12,
      others.length === 0 ? "ok" : "bad",
      others.length === 0
        ? "工作树干净" + (allowed.length ? "（" + allowed.length + " 处是本计划产物，待提交）" : "")
        : others.length + " 处不属于本计划的改动",
      others.length === 0 ? "" : others.slice(0, 3).map(entryPath).join("、")
    )
  }

  if (audited.notes.length > 0) audited.notes.forEach(function (text) { notes.push(text) })

  const ok = rows.filter(function (r) { return r.status === "bad" }).length === 0
  return { ok: ok, rows: rows, counts: counts, notes: notes }
}

/**
 * 验收结果 → 中文表格。
 * @param {object} result
 * @returns {string}
 */
function renderAcceptance(result) {
  const r = result || {}
  const rows = Array.isArray(r.rows) ? r.rows : []
  const counts = r.counts || {}
  const lines = []
  lines.push("算法轨交付验收（12 条）")
  lines.push(
    "  单元 " + (counts.md || 0) + " / 参考解法 " + (counts.reference || 0) +
    " / 负例 " + (counts.negative || 0) + "；还欠 " + (counts.debt || 0) +
    "；待补 " + (counts.planned || 0)
  )
  lines.push("")
  rows.forEach(function (row) {
    const mark = row.status === "ok" ? "✅" : row.status === "bad" ? "❌" : "…"
    lines.push(
      "  " + mark + " " + String(row.no).padStart(2) + ". " + row.name +
      " —— " + row.actual
    )
    if (row.detail) lines.push("       " + row.detail)
    if (row.status === "manual") lines.push("       怎么查：" + row.how)
  })
  lines.push("")
  const bad = rows.filter(function (row) { return row.status === "bad" }).length
  const manual = rows.filter(function (row) { return row.status === "manual" }).length
  lines.push(
    bad === 0
      ? "没有不合格项" + (manual ? "；还有 " + manual + " 条需要人确认" : "") + "。"
      : "有 " + bad + " 条不合格" + (manual ? "，另有 " + manual + " 条需要人确认" : "") + "。"
  )
  return lines.join("\n")
}

module.exports = {
  ROWS: ROWS,
  runAcceptance: runAcceptance,
  renderAcceptance: renderAcceptance,
}
