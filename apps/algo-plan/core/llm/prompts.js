"use strict"
/**
 * prompts.js —— 提示词构造与响应解析（**纯函数，不联网**）
 *
 * 设计文档 §6.4：提示词是纯函数，改动用 Node 单测钉住，不需要联网测。
 *
 * 纪律在这里就落一半（G24）：
 *   1. 不主动泄题 —— 只有「代码」档的 system 提示才放开给代码；
 *      其它档位一律禁止给代码；并且 explain / hint 分支**根本不读**
 *      context.solution，参考解法进不了提示词。
 *   2. 产出全部可追溯 —— 提示词里带上题号与档位，方便回查。
 *
 * 纯函数边界：不联网、不读环境变量、不读文件、不取当前时间。
 */

const { normalizeTopics, DIFFICULTIES } = require("../bank/problem.js")

/** 讲解的档位：先思路，后代码 */
const EXPLAIN_LEVELS = ["思路", "代码"]

/** 求助的档位：方向 → 思路 → 伪码 → 代码，逐级解锁 */
const HINT_LEVELS = ["方向", "思路", "伪码", "代码"]

/** 全部提示词种类 */
const KINDS = ["explain", "hint", "grade-idea", "generate", "fill"]

/** 禁止给代码时，system 提示里必须出现的句子（测试钉住它） */
const NO_CODE_RULE = "绝不给出任何可编译的完整代码"

/** 允许给代码时，system 提示里必须出现的句子（测试钉住它） */
const ALLOW_CODE_RULE = "本次允许给出完整代码"

/** 各档位对模型的具体要求 */
const LEVEL_BRIEF = {
  方向: "只点一个大方向（往哪类思路想、用什么数据结构），不要展开推导，不要给伪代码。",
  思路: "讲清楚完整的解题思路与关键推导，但不要写任何代码，连伪代码也不要写。",
  伪码: "给出算法伪代码或步骤分解，可以用接近自然语言描述流程，但不要写可编译的完整实现。",
  代码: "给出可直接提交的完整代码：自带 main 函数，从 stdin 读入、从 stdout 输出，不要写 class Solution 包装。",
}

const SYSTEM_EXPLAIN = [
  "你是一位算法辅导老师，要给正在做题的人讲清「为什么这么想」。",
  "纪律：" + NO_CODE_RULE + "、函数实现或伪代码，只讲到思路层面。",
  "除非用户明确要求「代码」档，否则你只能讲思路。",
  "输出用简体中文，分点讲清，不要复述题面。",
].join("\n")

const SYSTEM_EXPLAIN_CODE = [
  "你是一位算法辅导老师，要给正在做题的人讲清「为什么这么想」。",
  ALLOW_CODE_RULE + "：用户明确选了「代码」档。",
  "判题契约：stdin 读入、stdout 输出，程序自带 main 函数，不要写 class Solution 包装。",
  "输出用简体中文，先讲思路，再给完整代码。",
].join("\n")

const SYSTEM_HINT = [
  "你是一位算法辅导老师。用户已经卡住了，在向你要提示。",
  "纪律：" + NO_CODE_RULE + "、函数实现或伪代码。",
  "除非用户明确要求「代码」档，否则你只能给到本档允许的内容。",
  "不要重复题面，不要重新讲一遍题目。",
  "输出用简体中文，简短直接。",
].join("\n")

const SYSTEM_HINT_PSEUDO = [
  "你是一位算法辅导老师。用户已经卡住了，在向你要提示。",
  "纪律：" + NO_CODE_RULE + "。本档允许给伪代码与步骤分解。",
  "不要重复题面，不要重新讲一遍题目。",
  "输出用简体中文，简短直接。",
].join("\n")

const SYSTEM_HINT_CODE = [
  "你是一位算法辅导老师。用户已经卡住了，在向你要提示。",
  ALLOW_CODE_RULE + "：用户明确选了「代码」档。",
  "判题契约：stdin 读入、stdout 输出，程序自带 main 函数，不要写 class Solution 包装。",
  "输出用简体中文，先讲思路，再给完整代码。",
].join("\n")

const SYSTEM_GRADE = [
  "你是一位算法老师，负责批改用户的解题思路。",
  "你只判断思路对不对，不给完整代码，不重写解法。",
  "输出必须是且只是一个 JSON 对象，字段为 verdict 与 comment，不要加解释性前后缀，不要用代码块围栏。",
  "输出用简体中文。",
].join("\n")

const SYSTEM_GENERATE = [
  "你是一位算法出题老师，要出一道能独立完成、能自动判题的算法题。",
  "输出必须是且只是一个 JSON 对象，字段为 title、statement、difficulty、topics、cases、solution、solutionLang。",
  "不要加解释性前后缀，不要用代码块围栏。",
].join("\n")

const SYSTEM_FILL = [
  "你是一位算法老师，要根据已有的参考解法反推出一道题的题面与测试用例。",
  "输出必须是且只是一个 JSON 对象，字段为 statement 与 cases。",
  "不要加解释性前后缀，不要用代码块围栏。",
].join("\n")

/** 知识点拼成一行；空则写「未标注」 */
function topicText(topics) {
  const list = Array.isArray(topics) ? normalizeTopics(topics) : []
  return list.length ? list.join("、") : "未标注"
}

/** 题面、标题、难度这些公共部分 */
function baseUser(ctx) {
  const lines = []
  if (ctx.title) lines.push("题目：" + ctx.title)
  if (ctx.difficulty) lines.push("难度：" + ctx.difficulty)
  if (Array.isArray(ctx.topics) && ctx.topics.length) lines.push("知识点：" + topicText(ctx.topics))
  if (ctx.statement) lines.push("题面：\n" + String(ctx.statement).trim())
  return lines.join("\n")
}

/**
 * 构造提示词。
 * @param {"explain"|"hint"|"grade-idea"|"generate"|"fill"} kind
 * @param {object} context
 *   explain / hint：{title, difficulty, topics, statement, level}
 *   grade-idea   ：{title, statement, idea}
 *   generate     ：{track, topics, difficulty}
 *   fill         ：{title, difficulty, topics, statement, cases, solution, solutionLang}
 * @returns {{system: string, user: string}}
 */
function buildPrompt(kind, context) {
  const ctx = context || {}

  if (kind === "explain") {
    const level = EXPLAIN_LEVELS.indexOf(ctx.level) !== -1 ? ctx.level : "思路"
    return {
      system: level === "代码" ? SYSTEM_EXPLAIN_CODE : SYSTEM_EXPLAIN,
      user: [
        baseUser(ctx),
        "",
        "请按「" + level + "」档讲解本题。",
        LEVEL_BRIEF[level],
        "再提醒一次纪律：" + (level === "代码" ? ALLOW_CODE_RULE : NO_CODE_RULE) + "。",
      ].join("\n"),
    }
  }

  if (kind === "hint") {
    const level = HINT_LEVELS.indexOf(ctx.level) !== -1 ? ctx.level : "方向"
    const system = level === "代码"
      ? SYSTEM_HINT_CODE
      : (level === "伪码" ? SYSTEM_HINT_PSEUDO : SYSTEM_HINT)
    return {
      system: system,
      user: [
        baseUser(ctx),
        "",
        "请按「" + level + "」档给提示。用户已经卡住了，这一档只解决一个层次的问题。",
        LEVEL_BRIEF[level],
        "纪律：" + (level === "代码" ? ALLOW_CODE_RULE : NO_CODE_RULE) + "。",
      ].join("\n"),
    }
  }

  if (kind === "grade-idea") {
    const idea = String(ctx.idea || "").trim()
    return {
      system: SYSTEM_GRADE,
      user: [
        baseUser(ctx),
        "",
        "用户的解题思路：",
        idea || "（用户没有写思路）",
        "",
        "请判断这个思路是否可行：能不能得到正确答案，复杂度是否可接受，有没有漏掉边界情况。",
        "只输出一个 JSON 对象，不要任何多余文字，也不要用代码块围栏：",
        '{"verdict":"correct|partial|wrong","comment":"中文点评，一到三句"}',
        "verdict 只能取 correct、partial、wrong 三个值之一。",
      ].join("\n"),
    }
  }

  if (kind === "generate") {
    const track = String(ctx.track || "").trim() || "未指定"
    const difficulty = DIFFICULTIES.indexOf(String(ctx.difficulty)) !== -1
      ? String(ctx.difficulty)
      : "medium"
    return {
      system: SYSTEM_GENERATE,
      user: [
        "请出一道新的算法练习题。",
        "",
        "所属轨道：" + track,
        "知识点：" + topicText(ctx.topics),
        "难度：" + difficulty,
        "",
        "要求：",
        "1. 题面必须自包含：说清输入、输出、约束，不能依赖上下文。",
        "2. 判题契约：用户程序从 stdin 读入、从 stdout 输出，程序自带 main 函数。",
        "3. 至少给 3 个测试用例，其中至少 1 个是边界情况。",
        "4. 给一份 C++ 参考解法（cpp），是带 main 函数的完整程序。",
        "5. 题目要有明确答案，不能出无解或多解的题。",
        "",
        "只输出一个 JSON 对象，不要任何多余文字，也不要用代码块围栏：",
        '{"title":"题目名","statement":"完整题面","difficulty":"easy|medium|hard","topics":["知识点"],"cases":[{"input":"...","expect":"..."}],"solution":"完整 cpp 程序","solutionLang":"cpp"}',
      ].join("\n"),
    }
  }

  if (kind === "fill") {
    const cases = Array.isArray(ctx.cases) ? ctx.cases : []
    const lang = String(ctx.solutionLang || "cpp")
    const 用例行 = cases.length
      ? cases.map(function (c) { return "- " + c.input + " → " + c.expect }).join("\n")
      : "（缺失，需要你补）"
    return {
      system: SYSTEM_FILL,
      user: [
        "下面是一道题的原题解，题面与测试用例可能有缺失，请反推并补全。",
        "",
        "标题：" + String(ctx.title || "未命名"),
        "难度：" + String(ctx.difficulty || "未标注"),
        "知识点：" + topicText(ctx.topics),
        "当前题面：",
        String(ctx.statement || "").trim() || "（缺失，需要你补）",
        "",
        "当前测试用例（input → expect）：",
        用例行,
        "",
        "参考解法（" + lang + "）：",
        "```" + lang,
        String(ctx.solution || "").trim() || "（缺失）",
        "```",
        "",
        "要求：",
        "1. 补出完整题面：说清输入、输出、约束。",
        "2. 补出至少 3 个测试用例，期望输出必须与参考解法一致。",
        "3. 不要修改参考解法。",
        "4. 题面若已存在就原样保留，只补缺失的部分。",
        "",
        "只输出一个 JSON 对象，不要任何多余文字，也不要用代码块围栏：",
        '{"statement":"完整题面","cases":[{"input":"...","expect":"..."}]}',
      ].join("\n"),
    }
  }

  return { system: "", user: "" }
}

/** 从可能带围栏、前后缀的文本里抠出 JSON 对象；抠不出返回 null */
function extractJson(text) {
  const raw = String(text === null || text === undefined ? "" : text)
  const fenced = /```(?:json)?\s*([\s\S]*?)```/i.exec(raw)
  const body = fenced ? fenced[1] : raw
  const start = body.indexOf("{")
  const end = body.lastIndexOf("}")
  if (start === -1 || end === -1 || end <= start) return null
  try {
    return JSON.parse(body.slice(start, end + 1))
  } catch (err) {
    return null
  }
}

/** 把模型给的各种说法归一成四种判定之一 */
function normalizeVerdict(value) {
  const text = String(value === null || value === undefined ? "" : value).trim().toLowerCase()
  if (text === "correct" || text === "正确" || text === "对" || text === "可行") return "correct"
  if (text === "wrong" || text === "错误" || text === "不对" || text === "不正确" || text === "不可行") return "wrong"
  if (text === "partial" || text === "部分正确" || text === "部分对" || text === "部分可行") return "partial"
  return "unknown"
}

/** 用例列表归一：丢掉空壳，tol 非法就置 null */
function normalizeCaseList(list) {
  const out = []
  if (!Array.isArray(list)) return out
  list.forEach(function (item) {
    if (!item || typeof item !== "object") return
    const input = String(item.input === undefined || item.input === null ? "" : item.input).trim()
    const expect = String(item.expect === undefined || item.expect === null ? "" : item.expect).trim()
    if (input === "" && expect === "") return
    let tol = null
    if (item.tol !== undefined && item.tol !== null && item.tol !== "") {
      const value = Number(item.tol)
      tol = Number.isFinite(value) ? value : null
    }
    out.push({ input: input, expect: expect, tol: tol })
  })
  return out
}

/** generate 的解析结果 → 题目骨架 */
function buildGenerated(obj) {
  const title = String(obj.title || "").trim()
  if (title === "") return { ok: false, reason: "生成结果缺标题" }
  const statement = String(obj.statement || "").trim()
  if (statement === "") return { ok: false, reason: "生成结果缺题面" }
  const cases = normalizeCaseList(obj.cases)
  if (cases.length === 0) return { ok: false, reason: "生成结果缺测试用例" }
  const solution = String(obj.solution || "").trim()
  if (solution === "") return { ok: false, reason: "生成结果缺参考解法" }
  const difficulty = DIFFICULTIES.indexOf(String(obj.difficulty)) !== -1
    ? String(obj.difficulty)
    : "medium"
  const topics = topicText(obj.topics) === "未标注" ? [] : normalizeTopics(obj.topics)
  return {
    ok: true,
    problem: {
      title: title,
      statement: statement,
      cases: cases,
      solution: solution,
      solutionLang: String(obj.solutionLang || "cpp").trim() || "cpp",
      difficulty: difficulty,
      topics: topics,
    },
  }
}

/** fill 的解析结果 → 补全补丁 */
function buildFillPatch(obj) {
  const statement = String(obj.statement || "").trim()
  if (statement === "") return { ok: false, reason: "补全结果缺题面" }
  const cases = normalizeCaseList(obj.cases)
  if (cases.length === 0) return { ok: false, reason: "补全结果缺测试用例" }
  const patch = { statement: statement, cases: cases }
  const solution = String(obj.solution || "").trim()
  if (solution !== "") {
    patch.solution = solution
    patch.solutionLang = String(obj.solutionLang || "cpp").trim() || "cpp"
  }
  return { ok: true, patch: patch }
}

/**
 * 解析模型回复。
 * @param {string} text
 * @param {"explain"|"hint"|"grade-idea"|"generate"|"fill"} kind
 * @returns {object} 成功时带 ok:true；失败时带 ok:false 与中文 reason
 */
function parseResponse(text, kind) {
  const raw = text === null || text === undefined ? "" : String(text).trim()

  if (kind === "explain" || kind === "hint") {
    if (raw === "") return { ok: false, kind: kind, reason: "模型没有返回内容" }
    return { ok: true, kind: kind, text: raw }
  }

  if (kind === "grade-idea") {
    const obj = extractJson(raw)
    if (!obj) {
      return { ok: false, kind: kind, reason: "模型返回的不是结构化结果，无法判定思路对错", raw: raw }
    }
    const comment = String(obj.comment || obj.remark || obj.explanation || "").trim()
    if (comment === "") {
      return { ok: false, kind: kind, reason: "模型没有给出点评", raw: raw }
    }
    return { ok: true, kind: kind, verdict: normalizeVerdict(obj.verdict), comment: comment }
  }

  if (kind === "generate") {
    const obj = extractJson(raw)
    if (!obj) {
      return { ok: false, kind: kind, reason: "模型返回的不是合法 JSON，无法生成题目", raw: raw }
    }
    const built = buildGenerated(obj)
    if (!built.ok) return { ok: false, kind: kind, reason: built.reason, raw: raw }
    return { ok: true, kind: kind, problem: built.problem }
  }

  if (kind === "fill") {
    const obj = extractJson(raw)
    if (!obj) {
      return { ok: false, kind: kind, reason: "模型返回的不是合法 JSON，无法补全题目", raw: raw }
    }
    const patch = buildFillPatch(obj)
    if (!patch.ok) return { ok: false, kind: kind, reason: patch.reason, raw: raw }
    return { ok: true, kind: kind, patch: patch.patch }
  }

  return { ok: false, kind: kind, reason: "未知的提示词种类：" + String(kind) }
}

module.exports = {
  buildPrompt: buildPrompt,
  parseResponse: parseResponse,
  EXPLAIN_LEVELS: EXPLAIN_LEVELS,
  HINT_LEVELS: HINT_LEVELS,
  KINDS: KINDS,
}
