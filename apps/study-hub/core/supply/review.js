"use strict"
/**
 * review.js —— 人工抽检的写回逻辑（CLI 的 review 子命令与 HTTP 接口共用同一份）
 *
 * 抽检 = 人看过一遍，给个结论。结论只有两种：
 *   - 通过（passed = true）→ frontmatter 记 reviewed: true / reviewed_at，status 不动
 *   - 打回（passed = false）→ status 改成 rejected，同样记 reviewed: true / reviewed_at
 *
 * 三条纪律：
 *   1. **写回只改目标行**（G15）：frontmatter 里只替换 reviewed / reviewed_at
 *      （打回时多一个 status）所在的那几行；抽检意见作为一条列表项追加到
 *      「## 关联」小节末尾；其余行逐字节不变（含 CRLF / LF）。
 *   2. **写回前查脏**（G14）：别人动过的文件一律阻塞，返回中文理由。
 *   3. **绝不自动提交**（G13）：本模块只产出 diff，不执行 git commit / git push。
 */

const fs = require("node:fs")

const {
  unitFilePath,
  applyFrontmatterPatch,
  appendSectionNote,
  writeFileChecked,
} = require("./unit-file.js")

/** 抽检意见追加到哪个小节 */
const REVIEW_SECTION = "关联"

/** 状态的中文名（写回时排优先级用） */
const STATUS_PRIORITY = {
  verified: 0,
  draft: 1,
  rejected: 2,
  todo: 3,
}

/**
 * 今天的日期（YYYY-MM-DD）。reviewed_at 用它。
 * @param {Date} [date]
 * @returns {string}
 */
function todayStamp(date) {
  const d = date instanceof Date ? date : new Date()
  const y = d.getFullYear()
  const m = String(d.getMonth() + 1).padStart(2, "0")
  const day = String(d.getDate()).padStart(2, "0")
  return y + "-" + m + "-" + day
}

/**
 * 挑出待抽检的单元：还没被人看过（reviewed 不为 true）的单元。
 * 排序：已验证优先，其次待校验，再次待补内容、已打回；同级按 id 升序。
 * @param {object[]} units
 * @param {{sample?: number, track?: string}} opts
 * @returns {object[]}
 */
function pickReviewTargets(units, opts) {
  const o = opts || {}
  const only = o.track ? String(o.track) : ""
  const list = (Array.isArray(units) ? units : [])
    .filter(function (unit) {
      if (only !== "" && String(unit.track || "") !== only) return false
      // 抽检只对**已达标**的单元开放：verified（执行器与变异测试都过了）且还没被人看过。
      // draft / todo / rejected 是「还没补齐」或「没过校验」，不是抽检对象。
      if (String(unit.status || "") !== "verified") return false
      return unit.reviewed !== true
    })
    .slice()
    .sort(function (a, b) {
      const pa = STATUS_PRIORITY[String(a.status || "todo")] === undefined ? 9 : STATUS_PRIORITY[String(a.status || "todo")]
      const pb = STATUS_PRIORITY[String(b.status || "todo")] === undefined ? 9 : STATUS_PRIORITY[String(b.status || "todo")]
      if (pa !== pb) return pa - pb
      const ia = String(a.id || "")
      const ib = String(b.id || "")
      return ia < ib ? -1 : ia > ib ? 1 : 0
    })
  const sample = Number(o.sample) > 0 ? Number(o.sample) : list.length
  return list.slice(0, sample)
}

/**
 * 写回抽检结论。
 * @param {{unit: object, passed: boolean, note?: string,
 *          unitsDir: string, repoRoot: string, stateFile?: string|null, today?: string}} opts
 * @returns {{ok: boolean, code: string, changed: boolean, reason: string, diff: string,
 *            unit: {id: string, status: string, reviewed: boolean, reviewed_at: string}|null}}
 */
function applyReview(opts) {
  const o = opts || {}
  const unit = o.unit || {}
  const id = String(unit.id || "")
  const passed = o.passed === true
  const note = String(o.note || "").trim()
  const unitsDir = o.unitsDir || ""
  const repoRoot = o.repoRoot || ""
  const today = String(o.today || todayStamp())

  function fail(code, reason) {
    return { ok: false, code: code, changed: false, reason: reason, diff: "", unit: null }
  }

  // 未达标的单元不配被抽检：人的一次点击不该覆盖执行器的裁决。
  // 这一条必须排在 unitsDir / repoRoot 检查**之前**——
  // 否则在非 git 的临时目录里，它会先被「读不到 git 状态」挡成 blocked，
  // 掩盖掉「这个单元本来就不该被抽检」这个真正的理由。
  if (String(unit.status || "") !== "verified") {
    return fail(
      "invalid",
      "这个单元还没通过校验（当前状态：" + String(unit.status || "todo") + "），不能抽检"
    )
  }

  if (unitsDir === "" || repoRoot === "") {
    return fail("invalid", "还没连上 learning 仓，写回不可用")
  }
  const file = unitFilePath(unitsDir, unit)
  if (!fs.existsSync(file)) {
    return fail("missing", "单元文件不存在：" + file)
  }

  const before = fs.readFileSync(file, "utf8")
  const patch = { reviewed: true, reviewed_at: today }
  if (!passed) patch.status = "rejected"

  let after = ""
  try {
    after = applyFrontmatterPatch(before, patch)
    if (note !== "") after = appendSectionNote(after, REVIEW_SECTION, "抽检意见：" + note)
  } catch (err) {
    return fail("invalid", String((err && err.message) || err))
  }

  const written = writeFileChecked(file, after, {
    repoRoot: repoRoot,
    stateFile: o.stateFile || null,
  })
  if (!written.ok) {
    return fail("blocked", written.reason)
  }

  const matched = /^status:[ \t]*(.+)$/m.exec(after)
  const status = matched
    ? String(matched[1]).trim()
    : passed
      ? String(unit.status || "verified")
      : "rejected"
  return {
    ok: true,
    code: "ok",
    changed: written.changed,
    reason: "",
    diff: written.diff,
    unit: { id: id, status: status, reviewed: true, reviewed_at: today },
  }
}

module.exports = {
  REVIEW_SECTION: REVIEW_SECTION,
  STATUS_PRIORITY: STATUS_PRIORITY,
  todayStamp: todayStamp,
  pickReviewTargets: pickReviewTargets,
  applyReview: applyReview,
}
