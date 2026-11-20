"use strict"
/**
 * attempt-log.js —— 把一次判题结果记进 attempts 表
 *
 * 为什么单独一个文件：P2 有**两条**链路会判题——单元页的
 * `POST /api/units/:id/judge` 与会话的 `POST /api/session/submit`。
 * 两边都记流水，口径必须只有一份，否则「单元页判的算不算练习」这类问题
 * 会在两个地方得出不同答案。
 *
 * 表被谁读：`/api/health`（attempts 总数）、`/api/progress`（今日 solved）、
 * `server/insights.js`（P3 诊断的知识点统计与每日曲线）。
 * 也就是说**只写不读不会有问题，只读不写会让人以为练过 0 次**——
 * 这正是 P2 落地时抓到的缺陷：当时全仓只有旧路由 `/api/attempt` 会写这张表。
 * 旧路由下线后，写这张表的**就是上面那两条链路**（单元页判题 / 会话提交），
 * 口径仍然只有这一份。
 */

const { normalizeUnitId } = require("../core/unit/unit.js")

/**
 * 从单元 id 推出旧口径的 `problem_id`。
 *
 * 为什么要写旧口径：`listAttemptsByProblem` 还在（契约 §4.1「旧名保留一个版本」），
 * 老测试与老数据都按无前缀的题号查。算法轨的旧 id 就是 stem（`lc-00007`），
 * 其它轨没有旧 id，写完整 id（含冒号，不会被 P1 那次的 `unit_id` 回填 SQL 误伤）。
 * @param {object} unit
 * @returns {string}
 */
function legacyProblemId(unit) {
  const id = String((unit && unit.id) || "")
  const track = String((unit && unit.track) || "")
  if (track !== "algorithm") return id
  return id.indexOf(":") === -1 ? id : id.slice(id.indexOf(":") + 1)
}

/**
 * 记一条作答流水。
 *
 * 只在**真判过**之后调用：`attempts.verdict` 是 NOT NULL，没判过就没有 verdict 可写，
 * 那种「只记为完成」的路径不该造假数据。
 *
 * @param {object} store 必须实现 insertAttempt
 * @param {{unit: object, result: object, code?: string, language?: string, mode?: string}} input
 *   result 是 judgeUnit 的返回值（含 verdict / passedCases / totalCases / durationMs）
 * @returns {number} 流水 id；store 不可用时返回 0（不抛错，判题本身不该因此失败）
 */
function recordUnitAttempt(store, input) {
  const src = input || {}
  const unit = src.unit || {}
  const result = src.result || {}
  if (!store || typeof store.insertAttempt !== "function") return 0
  if (!unit.id) return 0

  const track = String(unit.track || "algorithm")
  const entry = unit.entry || {}
  // 卡类判题跑的是 Python 断言脚本，用户没提交代码；这种情形 language 记 python。
  const language =
    String(src.language || entry.language || (unit.kind === "card" ? "python" : "cpp")).toLowerCase()
  const total = Number(result.totalCases || 0)
  const passed = Number(result.passedCases || 0)

  return store.insertAttempt({
    problem_id: legacyProblemId(unit),
    unit_id: normalizeUnitId(String(unit.id), track) || String(unit.id),
    track: track,
    ts: new Date().toISOString(),
    language: language,
    code: String(src.code || ""),
    verdict: String(result.verdict || ""),
    passed_cases: passed,
    total_cases: total,
    duration_ms: Number(result.durationMs || 0),
    mode: String(src.mode || "solve"),
    // 卡类的断言条数就是 passed/total；题目类的用例条数与断言条数是两件事，
    // 契约 §4.1 的两列留给卡类用，题目类写 0 保持旧口径。
    assert_total: unit.kind === "card" ? total : 0,
    assert_passed: unit.kind === "card" ? passed : 0,
  })
}

module.exports = {
  legacyProblemId: legacyProblemId,
  recordUnitAttempt: recordUnitAttempt,
}
