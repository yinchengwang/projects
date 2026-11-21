"use strict"
/**
 * algorithm-verify.js —— 算法轨校验侧：真实编译执行
 *
 * 复用**消费侧同一套**执行器（judge/runner.js 的 judge() + judge/sandbox.js 的沙箱），
 * 不许用字符串比对冒充执行（P5-7）。
 *
 * 判定顺序（契约 §10.1 冻结）：
 *   1. 参考解法 × 全部用例 → verdict 必须是 pass
 *   2. 负例 × 同一批用例     → verdict 必须在 {wrong_answer, runtime_error, timeout} 里
 *   3. 负例 pass           → 不算被拒（用例太弱，挡不住错解）
 *   4. 负例 compile_error  → 不算被拒（负例本身没写对）
 *
 * 单元级结论词只有 verified / rejected；pass / wrong_answer / timeout /
 * compile_error / runtime_error 是**单次执行**的结论（G20 的五类，不增不改）。
 *
 * 本模块**只读** learning 仓；唯一的写动作是把单元 md 的 status 改一行（G15 / P5-10）。
 */

const fs = require("node:fs")
const path = require("node:path")

const { judge } = require("../../judge/runner.js")
const { resolveToolchain } = require("../../judge/sandbox.js")
const { parseFrontmatter, splitSections, parseCases } = require("../bank/parse.js")
const { canonicalCode, patchStatusLine } = require("./algorithm-fill.js")

/** 负例「被拒」的三种 verdict（契约 §10.1 冻结） */
const NEGATIVE_REJECTED_VERDICTS = ["wrong_answer", "runtime_error", "timeout"]

/** 单元文件的小节名 */
const SECTION_CASES = "测试用例"
const SECTION_SOLUTION = "参考解法"

/**
 * 读单元文件：frontmatter + 正文小节。
 * @param {string} unitPath
 * @returns {{fm: object, body: string, sections: object, text: string}}
 */
function readUnitFile(unitPath) {
  const text = fs.readFileSync(unitPath, "utf8")
  const src = text.replace(/\r\n/g, "\n")
  const fmMatch = /^---\n([\s\S]*?)\n---\n?/.exec(src)
  if (!fmMatch) throw new Error("单元文件缺少 frontmatter：" + path.basename(unitPath))
  const fm = parseFrontmatter(fmMatch[1])
  const body = src.slice(fmMatch[0].length)
  return { fm: fm, body: body, sections: splitSections(body), text: text }
}

/**
 * 从正文里抠出 `## 参考解法` 的代码块（归一后）。
 * @param {object} sections
 * @returns {string}
 */
function solutionBlockOf(sections) {
  const raw = String((sections && sections[SECTION_SOLUTION]) || "")
  const fence = /```([A-Za-z0-9+#_-]*)\n([\s\S]*?)```/.exec(raw)
  return fence ? canonicalCode(fence[2]) : ""
}

/** 把 judge() 的返回值压成要记进账的形状 */
function summarize(result) {
  const r = result || {}
  return {
    verdict: String(r.verdict || ""),
    passedCases: Number(r.passedCases || 0),
    totalCases: Number(r.totalCases || 0),
    message: String(r.message || ""),
    compilerOutput: String(r.compilerOutput || ""),
    failedCase: r.failedCase || null,
  }
}

/**
 * 校验一个算法单元。
 *
 * @param {{unitPath: string, learningRoot: string, toolchain?: object, timeoutMs?: number}} params
 * @returns {Promise<{ok: boolean, status: string, reason: string, checks: Array, reference: object|null, negative: object|null}>}
 */
async function verifyAlgorithmUnit(params) {
  const p = params || {}
  const checks = []
  const unitPath = String(p.unitPath || "")
  const learningRoot = String(p.learningRoot || "")

  /** 结构层面的不合格：直接判 rejected */
  function reject(reason, name, detail) {
    checks.push({ name: name, ok: false, detail: detail || reason })
    return {
      ok: false,
      status: "rejected",
      reason: reason,
      checks: checks,
      reference: null,
      negative: null,
    }
  }

  if (!unitPath) return reject("没给单元文件路径", "单元文件", "没给单元文件路径")

  let parsed
  try {
    parsed = readUnitFile(unitPath)
  } catch (err) {
    return reject(err.message, "单元文件", err.message)
  }

  const fm = parsed.fm
  const referenceRel = String(fm.reference || "")
  const negativeRel = String(fm.negative || "")

  if (!referenceRel) return reject("单元没有 reference 路径，判不了", "参考解法路径", "frontmatter 里没有 reference")
  if (!negativeRel) return reject("单元没有负例文件路径，不合格", "负例路径", "frontmatter 里没有 negative")

  const referenceAbs = path.resolve(learningRoot, referenceRel)
  const negativeAbs = path.resolve(learningRoot, negativeRel)
  if (!fs.existsSync(referenceAbs)) {
    return reject("参考解法文件不存在：" + referenceRel, "参考解法文件", referenceRel)
  }
  if (!fs.existsSync(negativeAbs)) {
    return reject("负例文件不存在：" + negativeRel, "负例文件", negativeRel)
  }

  const referenceCode = fs.readFileSync(referenceAbs, "utf8")
  const negativeCode = fs.readFileSync(negativeAbs, "utf8")

  if (!/\bmain\s*\(/.test(referenceCode)) {
    return reject("参考解法没有 main 函数，不是完整的 stdin→stdout 程序", "参考解法结构", "找不到 main(")
  }
  if (!/\bmain\s*\(/.test(negativeCode)) {
    return reject("负例没有 main 函数", "负例结构", "找不到 main(")
  }
  if (canonicalCode(referenceCode) === canonicalCode(negativeCode)) {
    return reject("负例与参考解法完全相同，等于没测", "负例与解法", "两份代码逐字节相同")
  }

  const block = solutionBlockOf(parsed.sections)
  if (block === "") {
    return reject("单元文件的「## 参考解法」小节是空的", "参考解法代码块", "小节里没有代码块")
  }
  if (block !== canonicalCode(referenceCode)) {
    return reject("参考解法文件与单元文件里的代码块不一致", "参考解法双份一致", "md 代码块与 .reference.cpp 不一致")
  }

  const cases = parseCases(parsed.sections[SECTION_CASES] || "")
  if (cases.length === 0) {
    return reject("单元没有测试用例，判不了", "测试用例", "「## 测试用例」小节里没有数据行")
  }

  const toolchain = p.toolchain || resolveToolchain({})

  // —— 1) 参考解法 × 全部用例 ——
  const refResult = await judge({
    problem: { cases: cases },
    code: referenceCode,
    language: "cpp",
    timeoutMs: p.timeoutMs,
    toolchain: toolchain,
  })
  checks.push({
    name: "参考解法全用例通过",
    ok: refResult.verdict === "pass",
    detail: refResult.verdict + "：" + refResult.message,
  })
  if (refResult.verdict !== "pass") {
    return {
      ok: false,
      status: "rejected",
      reason: "参考解法没有全部通过（" + refResult.verdict + "）：" + refResult.message,
      checks: checks,
      reference: summarize(refResult),
      negative: null,
    }
  }

  // —— 2) 负例 × 同一批用例 ——
  const negResult = await judge({
    problem: { cases: cases },
    code: negativeCode,
    language: "cpp",
    timeoutMs: p.timeoutMs,
    toolchain: toolchain,
  })
  checks.push({
    name: "负例被拒绝",
    ok: NEGATIVE_REJECTED_VERDICTS.indexOf(negResult.verdict) !== -1,
    detail: negResult.verdict + "：" + negResult.message,
  })
  if (negResult.verdict === "pass") {
    return {
      ok: false,
      status: "rejected",
      reason: "负例竟然全部通过，说明测试用例太弱，挡不住错解",
      checks: checks,
      reference: summarize(refResult),
      negative: summarize(negResult),
    }
  }
  if (negResult.verdict === "compile_error") {
    return {
      ok: false,
      status: "rejected",
      reason: "负例编译失败，负例本身无效",
      checks: checks,
      reference: summarize(refResult),
      negative: summarize(negResult),
    }
  }
  if (NEGATIVE_REJECTED_VERDICTS.indexOf(negResult.verdict) === -1) {
    return {
      ok: false,
      status: "rejected",
      reason: "负例的结论无法判定：" + negResult.verdict,
      checks: checks,
      reference: summarize(refResult),
      negative: summarize(negResult),
    }
  }

  return {
    ok: true,
    status: "verified",
    reason: "负例被拒绝（" + negResult.verdict + "）",
    checks: checks,
    reference: summarize(refResult),
    negative: summarize(negResult),
  }
}

/**
 * 批量校验 units/algorithm/ 下的全部单元，并把 status 写回单元 md 的那一行。
 *
 * @param {{unitsDir: string, learningRoot: string, toolchain?: object, timeoutMs?: number, write?: boolean}} params
 * @returns {Promise<{checked: number, verified: number, rejected: number, results: Array}>}
 */
async function verifyAlgorithmUnits(params) {
  const o = params || {}
  const unitsDir = String(o.unitsDir || "")
  const summary = { checked: 0, verified: 0, rejected: 0, results: [] }
  let names = []
  try {
    names = fs
      .readdirSync(unitsDir)
      .filter(function (name) {
        return /\.md$/i.test(name)
      })
      .sort()
  } catch (err) {
    return summary
  }
  for (let i = 0; i < names.length; i += 1) {
    const unitPath = path.join(unitsDir, names[i])
    const res = await verifyAlgorithmUnit({
      unitPath: unitPath,
      learningRoot: String(o.learningRoot || ""),
      toolchain: o.toolchain,
      timeoutMs: o.timeoutMs,
    })
    summary.checked += 1
    if (res.status === "verified") summary.verified += 1
    else summary.rejected += 1
    summary.results.push({
      unit: "algo:" + names[i].replace(/\.md$/i, ""),
      status: res.status,
      reason: res.reason,
    })
    if (o.write !== false) {
      const text = fs.readFileSync(unitPath, "utf8")
      fs.writeFileSync(unitPath, patchStatusLine(text, res.status), "utf8")
    }
  }
  return summary
}

module.exports = {
  NEGATIVE_REJECTED_VERDICTS: NEGATIVE_REJECTED_VERDICTS,
  readUnitFile: readUnitFile,
  solutionBlockOf: solutionBlockOf,
  verifyAlgorithmUnit: verifyAlgorithmUnit,
  verifyAlgorithmUnits: verifyAlgorithmUnits,
}
