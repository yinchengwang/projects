"use strict"
/**
 * audit.js —— 算法轨交付审计（P5 的验收自查表，机器能查的部分）
 *
 * 纯文件检查：不联网、不编译、不写盘。
 *
 * 查这七条（对应计划 Task 10 的验收自查表第 1/2/3/8/9/10 条）：
 *   1. 三件套成对：每个 <stem>.md 必须有同 stem 的 .reference.cpp 与 .negative.cpp
 *   2. status：交付状态的单元必须是 verified（draft / rejected 都要报出来）
 *   3. 负例不外泄：单元 md 里不许出现负例独有的代码行（P5-5）
 *   4. 双份一致：md 的「## 参考解法」代码块与 .reference.cpp 必须逐字节一致（P5-6）
 *   5. plan.json 不混进产物目录（它是清单，不是产物）
 *   6. 密钥没进产物（G9）
 *   7. 孤儿文件：有 cpp 但没对应 md 的，报出来
 */

const fs = require("node:fs")
const path = require("node:path")

const fill = require("./algorithm-fill.js")
const { findNegativeLeak } = require("./algorithm-verify.js")

/** 交付状态：单元经过真实校验并通过 */
const DELIVERED_STATUS = "verified"

/** 密钥特征（G9）：绝不进产物 */
const SECRET_PATTERNS = [
  { name: "ARK_API_KEY", re: /ARK_API_KEY\s*=/ },
  { name: "ANTHROPIC_API_KEY", re: /ANTHROPIC_API_KEY\s*=/ },
  { name: "sk- 前缀", re: /\bsk-[A-Za-z0-9]{8,}/ },
]

/**
 * 审计一个单元目录。
 * @param {{unitsDir: string, learningRoot?: string}} params
 * @returns {{ok: boolean, counts: object, problems: Array, notes: string[]}}
 */
function auditAlgorithmUnits(params) {
  const p = params || {}
  const unitsDir = String(p.unitsDir || "")
  const problems = []
  const notes = []
  const counts = { md: 0, reference: 0, negative: 0, verified: 0, rejected: 0, draft: 0 }

  function problem(kind, stem, detail) {
    problems.push({ kind: kind, stem: stem, detail: detail })
  }

  if (unitsDir === "" || !fs.existsSync(unitsDir)) {
    notes.push("单元目录还不存在：" + (unitsDir || "（空）") + "（还没跑过批量补题）")
    return { ok: true, counts: counts, problems: problems, notes: notes }
  }

  let names = []
  try {
    names = fs.readdirSync(unitsDir).sort()
  } catch (err) {
    notes.push("单元目录读不到：" + String((err && err.message) || err))
    return { ok: true, counts: counts, problems: problems, notes: notes }
  }

  names.forEach(function (name) {
    if (name === "plan.json") {
      problem("plan-json", "", "plan.json 混在产物目录里（它是清单，不是产物，提交时要剔掉）")
      return
    }
    if (!/\.md$/i.test(name)) return
    counts.md += 1
    const stem = name.replace(/\.md$/i, "")
    const mdPath = path.join(unitsDir, name)
    const md = fs.readFileSync(mdPath, "utf8")

    // —— 1. 三件套成对 ——
    const refPath = path.join(unitsDir, stem + ".reference.cpp")
    const negPath = path.join(unitsDir, stem + ".negative.cpp")
    if (!fs.existsSync(refPath)) problem("missing-file", stem, "缺参考解法文件 " + stem + ".reference.cpp")
    else counts.reference += 1
    if (!fs.existsSync(negPath)) problem("missing-file", stem, "缺负例文件 " + stem + ".negative.cpp")
    else counts.negative += 1

    // —— 2. status ——
    const statusMatch = /^status:[ \t]*(\S+)[ \t]*$/m.exec(md)
    const status = statusMatch ? statusMatch[1] : ""
    if (status === DELIVERED_STATUS) counts.verified += 1
    else if (status === "rejected") {
      counts.rejected += 1
      problem("status", stem, "status 是 rejected（没通过真实校验）")
    } else {
      counts.draft += 1
      problem("status", stem, "status 是 " + (status || "（空）") + "，不是 " + DELIVERED_STATUS)
    }

    // —— 3/4. 负例不外泄 + 双份一致（需要两份 cpp 都在）——
    if (fs.existsSync(refPath) && fs.existsSync(negPath)) {
      const ref = fs.readFileSync(refPath, "utf8")
      const neg = fs.readFileSync(negPath, "utf8")
      const leak = findNegativeLeak(md, neg, ref)
      if (leak !== "") problem("negative-leak", stem, "md 里混进了负例代码：" + leak)
      const block = solutionBlockOf(md)
      if (block === "") problem("reference-missing", stem, "md 的「## 参考解法」小节没有代码块")
      else if (block !== fill.canonicalCode(ref)) {
        problem("reference-drift", stem, "md 代码块与 " + stem + ".reference.cpp 不是逐字节一致")
      }
    }

    // —— 6. 密钥 ——
    SECRET_PATTERNS.forEach(function (item) {
      if (item.re.test(md)) problem("secret", stem, "单元 md 里出现了密钥特征：" + item.name)
    })
  })

  // —— 7. 孤儿 cpp ——
  names.forEach(function (name) {
    const m = /^(.+)\.(reference|negative)\.cpp$/i.exec(name)
    if (!m) return
    const stem = m[1]
    if (!fs.existsSync(path.join(unitsDir, stem + ".md"))) {
      problem("orphan-file", stem, "有 " + name + " 但缺 " + stem + ".md")
    }
  })

  return {
    ok: problems.length === 0,
    counts: counts,
    problems: problems,
    notes: notes,
  }
}

/** 从单元 md 里抠出 `## 参考解法` 的代码块（归一后） */
function solutionBlockOf(md) {
  const text = String(md || "").replace(/\r\n/g, "\n")
  const sections = splitSectionsOf(text)
  const raw = String(sections["参考解法"] || "")
  const fence = /```([A-Za-z0-9+#_-]*)\n([\s\S]*?)```/.exec(raw)
  return fence ? fill.canonicalCode(fence[2]) : ""
}

/** 只切「## 小节」，不依赖 bank/parse（审计要在最少的依赖下跑得起来） */
function splitSectionsOf(text) {
  const out = {}
  const lines = String(text || "").replace(/\r\n/g, "\n").split("\n")
  let current = ""
  let buf = []
  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i]
    const m = /^##[ \t]+(.+?)[ \t]*$/.exec(line)
    if (m) {
      if (current !== "") out[current] = buf.join("\n")
      current = m[1].trim()
      buf = []
      continue
    }
    if (current !== "") buf.push(line)
  }
  if (current !== "") out[current] = buf.join("\n")
  return out
}

/**
 * 审计结果 → 中文报告。
 * @param {object} result
 * @returns {string}
 */
function renderAuditReport(result) {
  const r = result || {}
  const counts = r.counts || {}
  const problems = Array.isArray(r.problems) ? r.problems : []
  const notes = Array.isArray(r.notes) ? r.notes : []
  const lines = []
  lines.push("算法轨交付审计")
  lines.push(
    "  md " + (counts.md || 0) +
    " 个 / 参考解法 " + (counts.reference || 0) +
    " 个 / 负例 " + (counts.negative || 0) + " 个"
  )
  lines.push(
    "  状态：verified " + (counts.verified || 0) +
    "，rejected " + (counts.rejected || 0) +
    "，其它 " + (counts.draft || 0)
  )
  if (problems.length === 0) {
    lines.push("  没有发现问题。")
  } else {
    lines.push("  发现 " + problems.length + " 处问题：")
    problems.slice(0, 20).forEach(function (item) {
      lines.push("    " + (item.stem ? item.stem + "  " : "") + item.detail)
    })
    if (problems.length > 20) lines.push("    ……还有 " + (problems.length - 20) + " 处没列出来")
  }
  notes.forEach(function (text) {
    lines.push("  提醒：" + text)
  })
  return lines.join("\n")
}

module.exports = {
  DELIVERED_STATUS: DELIVERED_STATUS,
  SECRET_PATTERNS: SECRET_PATTERNS,
  auditAlgorithmUnits: auditAlgorithmUnits,
  renderAuditReport: renderAuditReport,
}
