"use strict"
/**
 * unit.js —— 学习单元（Unit）模型
 *
 * 三条轨共用一个 Unit。下游（调度器 / 判题器 / 掌握度 / LLM / UI）完全不感知来源轨道——
 * 这是整个 app 的核心抽象：跨轨统计、跨轨排程、跨轨复习因此是自然结果，而不是三套并行实现。
 *
 * 字段表见契约 §1.2。本文件是**唯一**定义 Unit 形状的地方。
 */

// 轨道 → id 前缀的映射只有 core/track/tracks.js 一份（G41），这里引用它，不另抄一份。
const { TRACK_CODES, trackOf } = require("../track/tracks.js")

/** 单元种类：problem = 要写答案；card = 读讲解 + 跑 demo + 过断言。 */
const KINDS = ["problem", "card"]

/** 内容可信度。 */
const STATUSES = ["todo", "draft", "verified", "rejected"]

/** 学习进度（旧 frontmatter 的 status 语义）。 */
const PROGRESSES = ["todo", "in-progress", "done"]

/** 难度。 */
const DIFFICULTIES = ["easy", "medium", "hard"]

/** 入口种类。 */
const GRADED = ["user", "script"]

/** 内容来源。 */
const SOURCES = [
  "leetcode",
  "nowcoder",
  "self",
  "grokking",
  "scaffold",
  "generated",
  "codeforces",
  "unknown",
]

/** 单元 id 的形状：<trackCode>:<stem>。 */
const ID_PATTERN = /^(algo|db|ai):[a-z0-9][a-z0-9._-]*$/

/**
 * 归一化一个字符串数组：去空、去首尾空白、去重、保序。
 * @param {string[]|string|null|undefined} list
 * @returns {string[]}
 */
function normalizeList(list) {
  const input = Array.isArray(list) ? list : list === undefined || list === null ? [] : [list]
  const out = []
  for (const item of input) {
    if (item === undefined || item === null) continue
    const text = String(item).trim()
    if (text === "") continue
    if (out.indexOf(text) !== -1) continue
    out.push(text)
  }
  return out
}

/**
 * 把任意来源的 id 规范成 <trackCode>:<stem>。
 * 已经带前缀的原样返回；不带前缀的按 track 补前缀。
 *
 * 轨道 → id 前缀的映射只有 core/track/tracks.js 一份（G41），这里引用它，不另抄一份。
 * 未知轨道返回空串——**不许静默兜底成 algo 前缀**：那样会造出前缀与轨道不符、
 * 永远 join 不上 SQLite 调度态的单元（G24）。
 *
 * 注意：规范化只发生在**内存层**——存量内容文件里的 id 一个字都不改（G33）。
 *
 * @param {string} raw 原始 id
 * @param {string} track 轨 code（algorithm / database / ai）
 * @returns {string}
 */
function normalizeUnitId(raw, track) {
  const text = String(raw === undefined || raw === null ? "" : raw).trim()
  if (text === "") return ""
  if (text.indexOf(":") !== -1) return text
  const t = trackOf(track)
  if (!t) return ""
  return t.idPrefix + ":" + text
}

/**
 * 把各种形态的真假值归一成布尔。
 * 只有 true 与字符串 "true" 算真——frontmatter 里的 `reviewed: false` 会被 mini-YAML
 * 解析成字符串 "false"，直接 Boolean() 会得到 true，所以必须显式处理。
 *
 * @param {*} value
 * @returns {boolean}
 */
function toBool(value) {
  if (value === true) return true
  if (typeof value === "string") return value.trim().toLowerCase() === "true"
  return false
}

/**
 * 校验一个单元对象。
 * @param {object} unit
 * @returns {string[]} 错误信息数组，空数组表示通过
 */
function validateUnit(unit) {
  const errors = []
  if (!unit || typeof unit !== "object") return ["单元不是对象"]
  if (!ID_PATTERN.test(String(unit.id || ""))) {
    errors.push("id 非法：" + String(unit.id || ""))
  }
  if (TRACK_CODES.indexOf(unit.track) === -1) {
    errors.push("track 非法：" + String(unit.track || ""))
  } else {
    // G41：id 前缀必须与轨道一致。前缀对不上的单元永远 join 不上 SQLite 里的调度态，
    // 还不许落库——否则它会安静地待在那里，join 一次也 join 不上。
    const expected = trackOf(unit.track).idPrefix
    const prefix = String(unit.id || "").split(":")[0]
    if (prefix !== expected) {
      errors.push(
        "id 前缀与 track 不一致：" + String(unit.id || "") + "（" + unit.track + " 轨应是 " + expected + " 前缀）"
      )
    }
  }
  if (KINDS.indexOf(unit.kind) === -1) {
    errors.push("kind 非法：" + String(unit.kind || ""))
  }
  if (!String(unit.title || "").trim()) {
    errors.push("title 不能为空")
  }
  // 契约 §1.2：path 必填，空串视为缺失（G42）。没有 path 的单元下游找不到文件。
  if (!String(unit.path || "").trim()) {
    errors.push("path 不能为空")
  }
  if (!unit.entry || typeof unit.entry !== "object") {
    errors.push("entry 缺失")
  } else {
    if (GRADED.indexOf(unit.entry.graded) === -1) {
      errors.push("entry.graded 非法：" + String(unit.entry.graded || ""))
    }
    if (unit.entry.graded === "script" && !String(unit.entry.script || "").trim()) {
      errors.push("entry.graded 是 script 但 entry.script 为空")
    }
  }
  if (!Array.isArray(unit.asserts)) {
    errors.push("asserts 必须是数组")
  }
  if (!Array.isArray(unit.topics)) {
    errors.push("topics 必须是数组")
  }
  if (DIFFICULTIES.indexOf(unit.difficulty) === -1) {
    errors.push("difficulty 非法：" + String(unit.difficulty || ""))
  }
  if (SOURCES.indexOf(unit.source) === -1) {
    errors.push("source 非法：" + String(unit.source || ""))
  }
  if (STATUSES.indexOf(unit.status) === -1) {
    errors.push("status 非法：" + String(unit.status || ""))
  }
  if (PROGRESSES.indexOf(unit.progress) === -1) {
    errors.push("progress 非法：" + String(unit.progress || ""))
  }
  if (typeof unit.reviewed !== "boolean") {
    errors.push("reviewed 必须是布尔值")
  }
  return errors
}

/**
 * 按字段造一个单元：填默认值 + 校验（不合法直接抛错）。
 *
 * @param {object} input
 * @returns {object} Unit
 */
function createUnit(input) {
  const src = input || {}
  const entry = src.entry || {}
  const created = {
    id: String(src.id === undefined || src.id === null ? "" : src.id).trim(),
    track: String(src.track === undefined || src.track === null ? "" : src.track).trim(),
    kind: String(src.kind === undefined || src.kind === null ? "" : src.kind).trim(),
    title: String(src.title === undefined || src.title === null ? "" : src.title).trim(),
    statement: String(src.statement === undefined || src.statement === null ? "" : src.statement),
    entry: {
      demo: String(entry.demo === undefined || entry.demo === null ? "" : entry.demo),
      reference: String(entry.reference === undefined || entry.reference === null ? "" : entry.reference),
      graded: String(entry.graded === undefined || entry.graded === null ? "" : entry.graded),
      script: String(entry.script === undefined || entry.script === null ? "" : entry.script),
      language: String(entry.language === undefined || entry.language === null ? "" : entry.language),
    },
    asserts: Array.isArray(src.asserts) ? src.asserts : [],
    topics: normalizeList(src.topics),
    difficulty: String(src.difficulty === undefined || src.difficulty === null ? "medium" : src.difficulty),
    source: String(src.source === undefined || src.source === null ? "unknown" : src.source),
    status: String(src.status === undefined || src.status === null ? "todo" : src.status),
    progress: String(src.progress === undefined || src.progress === null ? "todo" : src.progress),
    reviewed: toBool(src.reviewed),
    reviewed_at: String(src.reviewed_at === undefined || src.reviewed_at === null ? "" : src.reviewed_at),
    subtracks: normalizeList(src.subtracks),
    related: String(src.related === undefined || src.related === null ? "" : src.related),
    path: String(src.path === undefined || src.path === null ? "" : src.path),
  }
  const errors = validateUnit(created)
  if (errors.length > 0) {
    throw new Error("单元字段非法：" + errors.join("；"))
  }
  return created
}

/**
 * 列表用的摘要（不含 statement / asserts / path / entry）。
 * @param {object} unit
 * @returns {object}
 */
function toUnitSummary(unit) {
  return {
    id: unit.id,
    track: unit.track,
    kind: unit.kind,
    title: unit.title,
    status: unit.status,
    difficulty: unit.difficulty,
    topics: Array.isArray(unit.topics) ? unit.topics.slice() : [],
    subtracks: Array.isArray(unit.subtracks) ? unit.subtracks.slice() : [],
  }
}

module.exports = {
  KINDS: KINDS,
  STATUSES: STATUSES,
  PROGRESSES: PROGRESSES,
  DIFFICULTIES: DIFFICULTIES,
  GRADED: GRADED,
  SOURCES: SOURCES,
  ID_PATTERN: ID_PATTERN,
  normalizeList: normalizeList,
  normalizeUnitId: normalizeUnitId,
  toBool: toBool,
  validateUnit: validateUnit,
  createUnit: createUnit,
  toUnitSummary: toUnitSummary,
}
