"use strict"
/**
 * verify.js —— 执行器反向校验 + 变异测试（契约 §10）
 *
 * 一句话：**不用相信 LLM，用执行器验它。**
 *
 * 卡类单元（kind=card）的校验动作：
 *   1. 把 demo 与断言脚本都复制进沙箱（judge/sandbox.js，G18）；
 *   2. 真跑一遍断言脚本 —— 必须 exit 0；
 *   3. **变异测试**：把沙箱里的 demo 副本改坏，重跑断言脚本 —— 它**必须失败**。
 *      exit 0 只证明「没崩」，一个 assert True 也能过；变异测试才证明断言真的在盯着被测逻辑。
 *   4. 结论写进 loader C 的单元文件（learning/units/<track>/<stem>.md）：
 *      draft → verified 或 draft → rejected。loader B 自己只读，永远不写盘（契约 §1.5）。
 *
 * 结论词是「供给结论」，不是 judge 的 verdict——verdict 五类（pass/wrong_answer/…）冻结不变（G20）。
 */

const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { createSandbox, runCommand, resolveToolchain, copyIntoSandbox } = require("../../judge/sandbox.js")
const {
  renderUnitFile,
  patchUnitFile,
  writeFileChecked,
  unitFilePath,
  stemOf,
} = require("./unit-file.js")

/** 供给结论（不是 verdict；verdict 五类冻结，G20） */
const SUPPLY_STATUSES = ["verified", "rejected", "skipped"]

/** 三种变异手法（契约 §10 的原文顺序） */
const MUTATION_TECHNIQUES = ["assert", "if", "return"]

/** 手法的中文名 */
const TECHNIQUE_LABELS = {
  assert: "把第一条 assert 改成 assert False",
  if: "把第一条 if 的条件改成 if False",
  return: "把第一条 return 改成 return None",
}

/** 供给侧跑的是整个 demo（不是判题器的单个用例），时限比 G19 的 2000ms 宽 */
const DEFAULT_VERIFY_TIMEOUT_MS = 20000

/**
 * 把一行拆成「代码部分 + 行尾注释」。
 * @param {string} text
 * @returns {{code: string, comment: string}}
 */
function splitComment(text) {
  const at = String(text).indexOf("#")
  if (at === -1) return { code: String(text), comment: "" }
  return { code: String(text).slice(0, at), comment: String(text).slice(at).trim() }
}

/**
 * 变异一份 demo 源码。**只对沙箱副本调用**，绝不动 learning 仓里的文件（G18）。
 * @param {string} source
 * @param {string} technique "assert" | "if" | "return"
 * @returns {{ok: boolean, code: string, technique: string, reason: string}}
 */
function mutateSource(source, technique) {
  const src = String(source === null || source === undefined ? "" : source)
  const eol = src.indexOf("\r\n") !== -1 ? "\r\n" : "\n"
  const lines = src.split(/\r\n|\n/)

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i]

    if (technique === "assert") {
      const m = /^(\s*)assert\s+(.*)$/.exec(line)
      if (!m) continue
      const parts = splitComment(m[2])
      if (parts.code.trim() === "") continue
      lines[i] = m[1] + "assert False" + (parts.comment ? " " + parts.comment : "")
      return { ok: true, code: lines.join(eol), technique: technique, reason: "" }
    }

    if (technique === "if") {
      // 2a. 块级 if ...:
      const m = /^(\s*)if\s+(.*)$/.exec(line)
      if (m) {
        const raw = m[2].trim()
        if (raw.endsWith(":")) {
          const parts = splitComment(raw.slice(0, -1))
          const cond = parts.code.trim()
          const isMainGuard =
            cond === '__name__ == "__main__"' || cond === "__name__ == '__main__'"
          if (cond !== "" && !isMainGuard) {
            lines[i] =
              m[1] + "if False:" + (parts.comment ? " " + parts.comment : "")
            return { ok: true, code: lines.join(eol), technique: technique, reason: "" }
          }
        }
        continue
      }
      // 2b. 内联条件式 A if 条件 else B
      const m2 = /^(\s*)(.*?)\s+if\s+(.+?)\s+else\s+(.*)$/.exec(line)
      if (m2) {
        const cond = m2[3].trim()
        if (cond !== "") {
          lines[i] =
            m2[1] +
            m2[2] +
            " if False else " +
            m2[4]
          return { ok: true, code: lines.join(eol), technique: technique, reason: "" }
        }
      }
      continue
    }

    if (technique === "return") {
      const m = /^(\s*)return\s+(.*)$/.exec(line)
      if (!m) continue
      const parts = splitComment(m[2])
      const value = parts.code.trim()
      if (value === "" || value === "None") continue
      lines[i] = m[1] + "return None" + (parts.comment ? " " + parts.comment : "")
      return { ok: true, code: lines.join(eol), technique: technique, reason: "" }
    }
  }

  const reasons = {
    assert: "demo 里没有 assert 语句",
    if: "demo 里没有可改的条件分支（入口守卫不算）",
    return: "demo 里没有可改的 return 语句",
  }
  return {
    ok: false,
    code: "",
    technique: technique,
    reason: reasons[technique] || "没有这个变异手法",
  }
}

/**
 * 探测本机有没有 numpy。
 * @param {{cmd: string, args: string[]}} python
 * @param {number} timeoutMs
 * @returns {Promise<boolean>}
 */
async function probeNumpy(python, timeoutMs) {
  const run = await runCommand(
    python.cmd,
    (python.args || []).concat(["-c", "import numpy"]),
    { cwd: os.tmpdir(), timeoutMs: timeoutMs }
  )
  return run.code === 0
}

/**
 * 把 stdout / stderr 摘成一句能给人看的理由。
 * @param {string} stdout
 * @param {string} stderr
 * @returns {string}
 */
function summarize(stdout, stderr) {
  const out = String(stdout || "").trim()
  const err = String(stderr || "").trim()
  const text = out !== "" ? out : err
  if (text === "") return "（没有输出）"
  const lines = text.split(/\r?\n/).filter(function (line) {
    return line.trim() !== ""
  })
  const picked = lines.slice(0, 3).join(" / ")
  return picked.length > 200 ? picked.slice(0, 200) + "…" : picked
}

/**
 * 把裁决写进 loader C 的单元文件（契约 §1.5）。
 * @param {object} unit
 * @param {string} status "verified" | "rejected"
 * @param {object} p verifyUnit 的入参
 * @returns {{ok: boolean, reason: string, diff: string}}
 */
function writeVerdict(unit, status, p) {
  const dir = p.unitsDir || null
  const root = p.repoRoot || p.learningRoot || null
  if (!dir || !root) {
    return { ok: false, reason: "还没连上 learning 仓，写回不可用", diff: "" }
  }
  const file = unitFilePath(dir, unit)
  const opts = { repoRoot: root, stateFile: p.stateFile || null }
  if (fs.existsSync(file)) {
    return patchUnitFile(file, { status: status }, opts)
  }
  const entry = unit.entry || {}
  const fields = {
    id: unit.id,
    track: unit.track,
    kind: "card",
    title: unit.title || "",
    difficulty: unit.difficulty || "medium",
    topics: Array.isArray(unit.topics) ? unit.topics : [],
    source: unit.source || "grokking",
    language: entry.language || "python",
    demo: entry.demo || "",
    reference: entry.reference || "",
    graded: "script",
    script: entry.script || "",
    status: status,
    reviewed: false,
    reviewed_at: "",
    subtracks: Array.isArray(unit.subtracks) ? unit.subtracks : [],
  }
  const content = renderUnitFile(fields, {
    explain: String(unit.statement || ""),
    related: [],
  })
  return writeFileChecked(file, content, opts)
}

/**
 * 校验一个单元：真跑一遍断言脚本，再做变异测试。
 * @param {{
 *   unit: object,
 *   learningRoot?: string|null,
 *   unitsDir?: string|null,
 *   repoRoot?: string|null,
 *   stateFile?: string|null,
 *   pythonOverride?: string|null,
 *   numpyAvailable?: boolean|null,
 *   timeoutMs?: number
 * }} params
 * @returns {Promise<{
 *   id: string, status: string, reason: string, mutated: boolean, technique: string,
 *   stdout: string, stderr: string, exitCode: number|null, durationMs: number,
 *   wrote: boolean, diff: string
 * }>}
 */
async function verifyUnit(params) {
  const p = params || {}
  const unit = p.unit || {}
  // P5 追加：算法轨分支 开始
  if (String(unit.kind || "") === "problem" && String(unit.track || "") === "algorithm") {
    const { verifyAlgorithmUnit } = require("./algorithm-verify.js")
    const unitPath = path.join(String(p.learningRoot || ""), String(unit.path || "").replace(/^units[\\/]/, "units/"))
    const algorithm = await verifyAlgorithmUnit({ unitPath: unitPath, learningRoot: p.learningRoot })
    return Object.assign({ id: String(unit.id || "") }, algorithm)
  }
  // P5 追加：算法轨分支 结束
  const timeoutMs = Number(p.timeoutMs) > 0 ? Number(p.timeoutMs) : DEFAULT_VERIFY_TIMEOUT_MS
  const result = {
    id: String(unit.id || ""),
    status: "skipped",
    reason: "",
    mutated: false,
    technique: "",
    stdout: "",
    stderr: "",
    exitCode: null,
    durationMs: 0,
    wrote: false,
    diff: "",
  }

  /** 记一个结论并（对 verified / rejected）写回单元文件 */
  function settle(status, reason) {
    result.status = status
    result.reason = reason
    if (status === "skipped") return result
    const written = writeVerdict(unit, status, p)
    result.wrote = written.ok
    result.diff = written.diff
    if (!written.ok) {
      result.reason = reason + "；但单元文件没写回：" + written.reason
    }
    return result
  }

  if (String(unit.kind || "") !== "card") {
    return settle("skipped", "算法题由 P5 校验")
  }
  const entry = unit.entry || {}
  const scriptRel = String(entry.script || "")
  const demoRel = String(entry.demo || "")
  if (scriptRel === "") return settle("skipped", "这个单元没有断言脚本路径")
  if (demoRel === "") return settle("skipped", "这个单元没有可运行的 demo")

  const root = p.learningRoot || p.repoRoot || null
  if (!root) return settle("skipped", "还没连上 learning 仓，校验不可用")

  const scriptAbs = path.join(root, scriptRel.split("/").join(path.sep))
  const demoAbs = path.join(root, demoRel.split("/").join(path.sep))
  if (!fs.existsSync(scriptAbs)) {
    return settle("skipped", "断言脚本还不存在：" + scriptRel)
  }
  if (!fs.existsSync(demoAbs)) {
    return settle("skipped", "demo 源码还不存在：" + demoRel)
  }

  const toolchain = resolveToolchain({ pythonOverride: p.pythonOverride || null })
  const python = toolchain.python
  if (!python) {
    return settle("skipped", "本机找不到可用的 Python 解释器，这个单元先跳过")
  }

  const demoSource = fs.readFileSync(demoAbs, "utf8")
  const needsNumpy = /^\s*(import\s+numpy|from\s+numpy\s+import)/m.test(demoSource)
  if (needsNumpy) {
    const hasNumpy =
      p.numpyAvailable === undefined || p.numpyAvailable === null
        ? await probeNumpy(python, timeoutMs)
        : Boolean(p.numpyAvailable)
    if (!hasNumpy) {
      return settle(
        "skipped",
        "环境缺失：本机没有装 numpy，这张卡先跳过（不算通过也不算打回）"
      )
    }
  }

  const sandbox = createSandbox()
  try {
    const demoInSandbox = copyIntoSandbox(sandbox, demoAbs, "main.py")
    const scriptInSandbox = copyIntoSandbox(
      sandbox,
      scriptAbs,
      stemOf(unit) + ".assert.py"
    )
    const env = Object.assign({}, process.env, { STUDY_HUB_DEMO: demoInSandbox })
    const args = (python.args || []).concat([scriptInSandbox])

    const first = await runCommand(python.cmd, args, {
      cwd: sandbox.dir,
      env: env,
      timeoutMs: timeoutMs,
    })
    result.stdout = first.stdout
    result.stderr = first.stderr
    result.exitCode = first.code
    result.durationMs = first.durationMs

    if (first.timedOut) {
      return settle("rejected", "断言脚本超时（超过 " + timeoutMs + " 毫秒）")
    }
    if (first.code !== 0) {
      return settle("rejected", "断言脚本没有通过：" + summarize(first.stdout, first.stderr))
    }

    let mutant = null
    for (const technique of MUTATION_TECHNIQUES) {
      const m = mutateSource(demoSource, technique)
      if (m.ok) {
        mutant = m
        break
      }
    }
    if (!mutant) {
      return settle("rejected", "找不到可变异点，无法验证断言有效性")
    }
    result.mutated = true
    result.technique = mutant.technique

    // 只改沙箱里的副本，learning 仓里的 demo 一个字节都不动（G18）
    fs.writeFileSync(demoInSandbox, mutant.code, "utf8")
    const second = await runCommand(python.cmd, args, {
      cwd: sandbox.dir,
      env: env,
      timeoutMs: timeoutMs,
    })
    if (second.timedOut) {
      return settle("rejected", "变异测试未捕获，断言无效（变异后脚本超时）")
    }
    if (second.code === 0) {
      return settle("rejected", "变异测试未捕获，断言无效")
    }
    return settle(
      "verified",
      "断言脚本通过，且变异测试证明断言有效（" + TECHNIQUE_LABELS[mutant.technique] + "）"
    )
  } finally {
    sandbox.cleanup()
  }
}

/**
 * 批量校验。
 * @param {{units: object[], learningRoot?: string|null, unitsDir?: string|null,
 *          repoRoot?: string|null, stateFile?: string|null,
 *          numpyAvailable?: boolean|null, timeoutMs?: number}} params
 * @returns {Promise<{results: object[], counts: {total, verified, rejected, skipped}, diffs: string[]}>}
 */
async function verifyBatch(params) {
  const p = params || {}
  const units = Array.isArray(p.units) ? p.units : []
  const results = []
  for (const unit of units) {
    results.push(await verifyUnit(Object.assign({}, p, { unit: unit })))
  }
  const counts = { total: results.length, verified: 0, rejected: 0, skipped: 0 }
  results.forEach(function (item) {
    if (item.status === "verified") counts.verified += 1
    else if (item.status === "rejected") counts.rejected += 1
    else counts.skipped += 1
  })
  const diffs = results
    .map(function (item) {
      return item.diff
    })
    .filter(function (diff) {
      return diff !== ""
    })
  return { results: results, counts: counts, diffs: diffs }
}

module.exports = {
  SUPPLY_STATUSES: SUPPLY_STATUSES,
  MUTATION_TECHNIQUES: MUTATION_TECHNIQUES,
  TECHNIQUE_LABELS: TECHNIQUE_LABELS,
  DEFAULT_VERIFY_TIMEOUT_MS: DEFAULT_VERIFY_TIMEOUT_MS,
  mutateSource: mutateSource,
  probeNumpy: probeNumpy,
  summarize: summarize,
  writeVerdict: writeVerdict,
  verifyUnit: verifyUnit,
  verifyBatch: verifyBatch,
}
