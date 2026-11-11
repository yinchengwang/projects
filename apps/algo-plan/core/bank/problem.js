"use strict"
/**
 * problem.js —— Problem 模型、稳定 ID、topic 归一化、字段校验
 *
 * 纯函数，无 IO。Problem 是 git 侧（learning/bank/*.md）与 SQLite 侧的唯一连接载体，
 * 一切靠稳定 id 互链（设计文档 §3.4）。
 */

const crypto = require("node:crypto")

/** 四条学习轨道 */
const TRACKS = ["interview", "fundamentals", "advanced", "engineering"]

/** 题目来源 */
const SOURCES = ["leetcode", "nowcoder", "jz", "self", "llm"]

/** 来源 → ID 前缀 */
const SOURCE_PREFIX = {
  leetcode: "lc",
  nowcoder: "nc",
  jz: "jz",
  self: "self",
  llm: "llm",
}

/** 难度 */
const DIFFICULTIES = ["easy", "medium", "hard"]

/** 题目状态 */
const STATUSES = ["todo", "in-progress", "done"]

/**
 * 归一化单个 topic：去首尾空白、内部连续空白压成一个空格。
 * topic 的 key 就是标签文本本身（设计文档 §3.4），所以拼写必须统一。
 */
function normalizeTopic(text) {
  if (text === null || text === undefined) return ""
  return String(text).trim().replace(/\s+/g, " ")
}

/**
 * 归一化 topic 列表：去空、去重、保序。
 * @param {string[]|string} list
 * @returns {string[]}
 */
function normalizeTopics(list) {
  const input = Array.isArray(list) ? list : list ? [list] : []
  const seen = Object.create(null)
  const out = []
  input.forEach(function (item) {
    const topic = normalizeTopic(item)
    if (topic === "" || seen[topic]) return
    seen[topic] = true
    out.push(topic)
  })
  return out
}

/**
 * 归一化 track 列表：只保留合法 track，去重保序。
 * @param {string[]|string} list
 * @returns {string[]}
 */
function normalizeTracks(list) {
  const input = Array.isArray(list) ? list : list ? [list] : []
  const seen = Object.create(null)
  const out = []
  input.forEach(function (item) {
    const track = normalizeTopic(item)
    if (track === "" || TRACKS.indexOf(track) === -1 || seen[track]) return
    seen[track] = true
    out.push(track)
  })
  return out
}

/** 今天的日期（YYYY-MM-DD，本地时区） */
function today() {
  const now = new Date()
  const y = now.getFullYear()
  const m = String(now.getMonth() + 1).padStart(2, "0")
  const d = String(now.getDate()).padStart(2, "0")
  return y + "-" + m + "-" + d
}

/**
 * 生成稳定 ID：`<前缀>-<5 位数字>`，例如 lc-00001。
 * @param {string} source 来源，见 SOURCES
 * @param {number|string} number 题号
 * @returns {string}
 */
function makeId(source, number) {
  const prefix = SOURCE_PREFIX[source]
  if (!prefix) throw new Error("未知题目来源：" + source)
  const digits = String(Math.trunc(Number(number)))
  if (!/^\d+$/.test(digits)) throw new Error("题号必须是数字：" + number)
  return prefix + "-" + (digits.length >= 5 ? digits : "00000".slice(digits.length) + digits)
}

/**
 * LLM 现造题的 ID：内容哈希，稳定且可复现。
 * @param {string} seed 题面或参考解法
 * @returns {string}
 */
function makeLlmId(seed) {
  const hash = crypto.createHash("sha1").update(String(seed)).digest("hex")
  return "llm-" + hash.slice(0, 10)
}

/**
 * ID 是否合法。
 * @param {string} id
 * @returns {boolean}
 */
function isValidId(id) {
  return /^(lc|nc|jz|self|llm)-[0-9a-z]{5,12}$/.test(String(id || ""))
}

/**
 * 校验题目字段。
 * @param {object} problem
 * @returns {{ok: boolean, errors: string[]}}
 */
function validateProblem(problem) {
  const errors = []
  if (!problem || typeof problem !== "object") {
    return { ok: false, errors: ["题目不是对象"] }
  }
  if (!isValidId(problem.id)) errors.push("id 非法：" + problem.id)
  if (SOURCES.indexOf(problem.source) === -1) errors.push("source 非法：" + problem.source)
  if (!problem.title) errors.push("title 为空")
  if (DIFFICULTIES.indexOf(problem.difficulty) === -1) {
    errors.push("difficulty 非法：" + problem.difficulty)
  }
  if (!Array.isArray(problem.topics) || problem.topics.length === 0) {
    errors.push("topics 不能为空")
  }
  if (!Array.isArray(problem.tracks) || problem.tracks.length === 0) {
    errors.push("tracks 不能为空")
  } else {
    problem.tracks.forEach(function (track) {
      if (TRACKS.indexOf(track) === -1) errors.push("track 非法：" + track)
    })
  }
  if (STATUSES.indexOf(problem.status) === -1) errors.push("status 非法：" + problem.status)
  return { ok: errors.length === 0, errors: errors }
}

/**
 * 按字段建一道题：填默认值 + 归一化 + 校验（不合法直接抛错）。
 * @param {object} input
 * @returns {object} Problem
 */
function createProblem(input) {
  const src = input || {}
  const problem = {
    id: src.id,
    source: src.source,
    title: src.title ? String(src.title).trim() : "",
    difficulty: src.difficulty || "medium",
    topics: normalizeTopics(src.topics),
    tracks: normalizeTracks(src.tracks),
    status: src.status || "todo",
    needsFill: src.needsFill === true,
    created: src.created || today(),
    updated: src.updated || today(),
    statement: src.statement ? String(src.statement).trim() : "",
    cases: Array.isArray(src.cases) ? src.cases : [],
    solution: src.solution ? String(src.solution).replace(/\n$/, "") : "",
    solutionLang: src.solutionLang || "",
    extraSections: Array.isArray(src.extraSections) ? src.extraSections : [],
  }

  const result = validateProblem(problem)
  if (!result.ok) {
    throw new Error("题目字段非法：" + result.errors.join("；"))
  }
  return problem
}

/**
 * 题目摘要（列表页用，不含题面、用例、解法）。
 * @param {object} problem
 * @returns {object}
 */
function toSummary(problem) {
  return {
    id: problem.id,
    title: problem.title,
    source: problem.source,
    difficulty: problem.difficulty,
    topics: problem.topics,
    tracks: problem.tracks,
    status: problem.status,
    needsFill: problem.needsFill,
  }
}

module.exports = {
  TRACKS: TRACKS,
  SOURCES: SOURCES,
  SOURCE_PREFIX: SOURCE_PREFIX,
  DIFFICULTIES: DIFFICULTIES,
  STATUSES: STATUSES,
  normalizeTopic: normalizeTopic,
  normalizeTopics: normalizeTopics,
  normalizeTracks: normalizeTracks,
  today: today,
  makeId: makeId,
  makeLlmId: makeLlmId,
  isValidId: isValidId,
  validateProblem: validateProblem,
  createProblem: createProblem,
  toSummary: toSummary,
}
