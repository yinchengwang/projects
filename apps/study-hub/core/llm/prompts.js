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

const SYSTEM_GENERATE_SKELETON = [
  "你是一位算法出题老师，要给一道已有的题补齐内容，一次性产出四件套：题面、测试用例、参考解法、负例。",
  "输出必须是且只是一个 JSON 对象，字段为 title、statement、difficulty、topics、cases、solution、negative、negativeNote。",
  "solution 是正确的 C++ 参考解法；negative 是故意写错的 C++ 解法（能编译、能运行，但算错）。",
  "不要加解释性前后缀，不要用代码块围栏。",
].join("\n")

/**
 * 各形态需要模型给出哪几件。
 * 形态 B 的题面、形态 C 的题面与用例都来自题库，**原样保留**，不让模型重写。
 */
const SKELETON_NEEDS = {
  A: ["statement", "cases", "solution", "negative"],
  B: ["cases", "solution", "negative"],
  C: ["solution", "negative"],
}

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
    // P5：算法轨批量补题的四件套分支（只在 ctx.mode === "skeleton" 时走）
    if (ctx.mode === "skeleton") return buildSkeletonPrompt(ctx)
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
      // —— P5 追加：负例与负例说明。老路径没有这两个字段，缺省是空串。——
      negative: String(obj.negative === undefined || obj.negative === null ? "" : obj.negative).trim(),
      negativeNote: String(obj.negativeNote === undefined || obj.negativeNote === null ? "" : obj.negativeNote).trim(),
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
 * 四件套的提示词（算法轨批量补题用）。
 *
 * 形态决定「哪几件要模型给」：
 *   A 题面与用例都缺 → 题面、用例、解法、负例都要
 *   B 只缺用例      → 题面原样保留，要用例、解法、负例
 *   C 只缺解法      → 题面与用例原样保留，要解法、负例
 *
 * @param {object} ctx {mode, shape, stem, title, difficulty, topics, url, statement, cases}
 * @returns {{system: string, user: string}}
 */
function buildSkeletonPrompt(ctx) {
  const c = ctx || {}
  const shape = String(c.shape || "A")
  const need = SKELETON_NEEDS[shape] || SKELETON_NEEDS.A
  const cases = Array.isArray(c.cases) ? c.cases : []
  const caseLines = cases.length
    ? cases.map(function (item) {
        return "- " + String(item.input || "") + " → " + String(item.expect || "")
      }).join("\n")
    : "（缺失，需要你补）"
  const statement = String(c.statement || "").trim() || "（缺失，需要你补）"

  const rules = []
  rules.push("1. 判题契约：程序从 stdin 读入、从 stdout 输出，自带 main 函数，不要写 class Solution 之类的包装。")
  if (need.indexOf("statement") !== -1) {
    rules.push("2. 题面缺失，必须新写：说清输入、输出、约束，不能依赖上下文。")
  } else {
    rules.push("2. 题面已经存在，原样保留，不要改写、不要润色。")
  }
  if (need.indexOf("cases") !== -1) {
    rules.push("3. 至少给 3 个测试用例，其中至少 1 个是边界情况；expect 必须与 solution 的实际输出逐字一致。")
  } else {
    rules.push("3. 测试用例已经存在，原样保留；solution 必须能通过上面列出的全部用例，expect 就是程序的输出。")
  }
  rules.push("4. solution 是正确的 C++ 参考解法：完整程序、自带 main 函数，能通过上面全部测试用例。")
  rules.push("5. negative 是故意写错的 C++ 解法：完整程序、自带 main 函数、能编译通过，但逻辑上算错——")
  rules.push("   在至少一个测试用例上给出与 expect 不同的输出。")
  rules.push("6. negative 不能与 solution 完全相同，也不能只是把 solution 换个变量名。")
  rules.push("7. negativeNote 用一句话说明负例错在哪（只给人看，不要泄露正确答案）。")

  const fields = []
  if (need.indexOf("statement") !== -1) fields.push("title、statement")
  if (need.indexOf("cases") !== -1) fields.push("cases")
  fields.push("solution、negative、negativeNote")

  const lines = []
  lines.push("请为下面这道题补齐内容。")
  lines.push("")
  lines.push("题号：" + String(c.stem || "未命名"))
  lines.push("标题：" + String(c.title || "未命名"))
  lines.push("难度：" + String(c.difficulty || "medium"))
  lines.push("知识点：" + topicText(c.topics))
  if (c.url) lines.push("原题链接：" + String(c.url))
  lines.push("")
  lines.push("当前题面：")
  lines.push(statement)
  lines.push("")
  lines.push("当前测试用例（input → expect）：")
  lines.push(caseLines)
  lines.push("")
  lines.push("要求：")
  lines.push(rules.join("\n"))
  lines.push("")
  lines.push("这次只需要你给出：" + fields.join("、") + "。")
  lines.push("")
  lines.push("只输出一个 JSON 对象，不要任何多余文字，也不要用代码块围栏：")
  lines.push(
    '{"title":"题目名","statement":"完整题面","difficulty":"easy|medium|hard","topics":["知识点"],"cases":[{"input":"...","expect":"..."}],"solution":"完整 cpp 程序","negative":"故意写错的完整 cpp 程序","negativeNote":"一句话说明负例错在哪"}'
  )

  return {
    system: SYSTEM_GENERATE_SKELETON,
    user: lines.join("\n"),
  }
}

/**
 * 四件套的解析结果 → 骨架。
 * 哪些件是必填由 need 决定（形态 B/C 的题面与用例来自题库，不让模型给）。
 * @param {object} obj
 * @param {string[]} need
 * @returns {object}
 */
function buildSkeleton(obj, need) {
  const want = Array.isArray(need) && need.length ? need : SKELETON_NEEDS.A
  const out = {}
  if (want.indexOf("statement") !== -1) {
    const statement = String(obj.statement || "").trim()
    if (statement === "") return { ok: false, reason: "生成结果缺题面" }
    out.statement = statement
  }
  if (want.indexOf("cases") !== -1) {
    const cases = normalizeCaseList(obj.cases)
    if (cases.length === 0) return { ok: false, reason: "生成结果缺测试用例" }
    out.cases = cases
  }
  if (want.indexOf("solution") !== -1) {
    const solution = String(obj.solution || "").trim()
    if (solution === "") return { ok: false, reason: "生成结果缺参考解法" }
    out.solution = solution
  }
  if (want.indexOf("negative") !== -1) {
    const negative = String(obj.negative || "").trim()
    if (negative === "") return { ok: false, reason: "生成结果缺负例" }
    out.negative = negative
  }
  out.title = String(obj.title || "").trim()
  out.difficulty = DIFFICULTIES.indexOf(String(obj.difficulty)) !== -1 ? String(obj.difficulty) : "medium"
  out.topics = topicText(obj.topics) === "未标注" ? [] : normalizeTopics(obj.topics)
  out.solutionLang = String(obj.solutionLang || "cpp").trim() || "cpp"
  out.negativeNote = String(obj.negativeNote === undefined || obj.negativeNote === null ? "" : obj.negativeNote).trim()
  return { ok: true, problem: out }
}

/**
 * 解析模型回复。
 * @param {string} text
 * @param {"explain"|"hint"|"grade-idea"|"generate"|"fill"} kind
 * @returns {object} 成功时带 ok:true；失败时带 ok:false 与中文 reason
 */
/**
 * 解析模型回复。
 * @param {string} text
 * @param {"explain"|"hint"|"grade-idea"|"generate"|"fill"} kind
 * @param {object} [opts] 可选；generate 的 skeleton 模式用 {mode:"skeleton", need:[...]}
 * @returns {object} 成功时带 ok:true；失败时带 ok:false 与中文 reason
 */
function parseResponse(text, kind, opts) {
  const context = opts || {}
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
    if (context.mode === "skeleton") {
      const builtSkeleton = buildSkeleton(obj, context.need)
      if (!builtSkeleton.ok) return { ok: false, kind: kind, reason: builtSkeleton.reason, raw: raw }
      return { ok: true, kind: kind, problem: builtSkeleton.problem }
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

/* ---------------- P4：按轨的模板表（契约 §7.2） ---------------- */

/** 供给场景（离线批处理用）：生成题面+用例+解法 / 生成断言脚本 / 生成讲解正文 / 深挖概念 */
const SUPPLY_SCENARIOS = ["generate-unit", "generate-assert", "write-explain", "deepen"]

/**
 * 按轨的模板表 —— **纯数据**。
 *
 * 纪律（契约 §7.2）：**轨道模板是数据，不是代码**。
 * 这里**没有** `if (track === "db")` 这样的硬分支：组装时按 unit.track 取模板 + 拼单元上下文。
 * 新增一条轨只要往这张表里加一条，不用改任何逻辑。
 *
 * 每个模板条目三个字段：
 *   response —— 期望的响应形状："json"（解析成 JSON 对象）或 "text"（纯文本）
 *   system   —— 系统提示
 *   user     —— 用户提示
 * {{占位符}} 由 renderTemplate 填。
 */
const TRACK_TEMPLATES = {
  algorithm: {
    label: "算法",
    defaultQuestion: "这道题的解法在什么规模下会超时？换一种解法能好在哪？",
    scenarios: {
      "generate-unit": {
        response: "json",
        system: [
          "你是一位算法出题老师，要出一道能独立完成、能自动判题的算法题。",
          "输出必须是且只是一个 JSON 对象，字段为 title、statement、difficulty、topics、cases、solution、solutionLang。",
          "不要加解释性前后缀，不要用代码块围栏。",
        ].join("\n"),
        user: [
          "请出一道新的算法练习题，落到轨道「{{trackLabel}}」。",
          "",
          "已有的题面（可能是空的）：",
          "{{statement}}",
          "",
          "要求：",
          "1. 题面必须自包含：说清输入、输出、约束，不能依赖上下文。",
          "2. 判题契约：用户程序从 stdin 读入、从 stdout 输出，程序自带 main 函数。",
          "3. 至少给 3 个测试用例，其中至少 1 个是边界情况。",
          "   测试用例要**覆盖边界**情况（空输入、单个元素、极值），不能只有一条 happy path；",
          "   用例的期望输出要**具体**（精确的 stdout 字符串），不要写「大概对」这种描述。",
          "4. 给一份 Python 参考解法，是带 main 函数的完整程序。",
          "5. 题目要有明确答案，不能出无解或多解的题。",
          "",
          "只输出一个 JSON 对象，不要任何多余文字，也不要用代码块围栏：",
          '{"title":"题目名","statement":"完整题面","difficulty":"easy|medium|hard","topics":["知识点"],"cases":[{"input":"...","expect":"..."}],"solution":"完整 python 程序","solutionLang":"python"}',
        ].join("\n"),
      },
      "write-explain": {
        response: "text",
        system: [
          "你是一位算法辅导老师，要给正在做题的人讲清「为什么这么想」。",
          "输出用简体中文，分点讲清，不要复述题面。",
        ].join("\n"),
        user: [
          "请给下面这道题写一份讲解正文。",
          "",
          "题目：{{title}}",
          "难度：{{difficulty}}",
          "知识点：{{topics}}",
          "",
          "题面：",
          "{{statement}}",
          "",
          "要求：",
          "1. 讲清三件事：这题的难点在哪、为什么想到这个思路、复杂度是多少。",
          "2. 用简体中文 Markdown，分小节写，不要复述题面。",
          "3. 篇幅 600~1200 字。",
          "4. 直接输出正文，不要加开场白。",
        ].join("\n"),
      },
      deepen: {
        response: "text",
        system: [
          "你是一位资深算法工程师，要就一个具体问题把一道题往下挖一层。",
          "输出用简体中文 Markdown，直接给正文。",
        ].join("\n"),
        user: [
          "请围绕下面这个问题，把「{{title}}」这道题深挖一层。",
          "",
          "问题：{{question}}",
          "",
          "题面：",
          "{{statement}}",
          "",
          "要求：",
          "1. 回答问题：为什么这样设计、换个设计会怎样、边界与失效场景。",
          "2. 给出复杂度与常数因子的实际影响。",
          "3. 600 字以内。",
        ].join("\n"),
      },
    },
  },
  database: {
    label: "数据库",
    defaultQuestion: "这个机制在什么情况下会失效？工程上要付出什么代价？",
    scenarios: {
      "generate-assert": {
        response: "json",
        system: [
          "你是一位数据库方向的测试工程师，要给一张教学 demo 写断言脚本。",
          "纪律：只依据被测代码的真实行为写断言，不许编造输出，不许写恒真的断言。",
          "输出必须是且只是一个 JSON 对象，字段为 assert_script、topics。",
          "不要加解释性前后缀，不要用代码块围栏。",
        ].join("\n"),
        user: [
          "请给下面这张数据库卡片写一份断言脚本。",
          "",
          "卡片：{{title}}",
          "知识点：{{topics}}",
          "",
          "被测 demo 源码（main.py）：",
          "```python",
          "{{demo}}",
          "```",
          "",
          "原始说明（README）：",
          "{{readme}}",
          "",
          "原始笔记（NOTES）：",
          "{{notes}}",
          "",
          "断言脚本的契约（照统一模板写，不许自创形状）：",
          "1. 用 `subprocess` 跑 demo，**不要** `import main`——demo 会在自己旁边写 .db 文件，用 subprocess 跑才不会污染仓库。",
          "2. **脚本自己把 demo 复制进临时目录再跑**，然后在临时目录里执行副本，跑完删掉临时目录。",
          "   这几个 demo **不能重复跑**（第二次会因为表已存在直接报错），就地执行必然失败，所以必须复制：",
          "   ```python",
          "   sandbox = tempfile.mkdtemp(prefix=\"study-hub-assert-\")",
          "   try:",
          "       target = os.path.join(sandbox, os.path.basename(DEMO))",
          "       shutil.copy2(DEMO, target)",
          "       proc = subprocess.run([sys.executable, target], capture_output=True, timeout=180)",
          "   finally:",
          "       shutil.rmtree(sandbox, ignore_errors=True)",
          "   ```",
          "3. **不许写死 `text=True`**。demo 的 stdout 按**本机 locale** 编码（Windows 中文环境是 GBK，Linux 是 UTF-8），",
          "   所以只写 `capture_output=True` 抓字节，再自己按 utf-8 → gbk → latin-1 依次试解码：",
          "   ```python",
          "   def decode(raw):",
          "       for enc in (\"utf-8\", \"gbk\"):",
          "           try:",
          "               return raw.decode(enc)",
          "           except UnicodeDecodeError:",
          "               continue",
          "       return raw.decode(\"utf-8\", errors=\"replace\")",
          "   ```",
          "4. demo 路径取环境变量 `STUDY_HUB_DEMO`，缺省是「与本脚本同目录的 main.py」，这一行照抄：",
          "   DEMO = os.environ.get(\"STUDY_HUB_DEMO\") or os.path.join(os.path.dirname(os.path.abspath(__file__)), \"main.py\")",
          "5. 跑完 demo 后，检查它的 stdout 里有没有出现关键结论（形如 \"[[OK]] ...\" 的标记或具体字符串）。",
          "6. 断言失败时打印中文原因并 exit 1；全部通过时 exit 0。",
          "7. 至少 3 条断言，并且至少有一条在实现被改坏时会失败（不许写恒真断言）。",
          "8. 脚本必须自包含：不许读写 demo 目录以外的文件。",
          "9. 断言脚本必须**覆盖 demo 输出的每个主要结论**（每一段、每一个关键数字、每一条结论行），",
          "   不能只检查「跑完了」这种笼统信号；要**具体**检查 demo 该段输出的内容（具体字符串或具体数值）。",
          "   脚本必须能在「被测逻辑被改坏」时失败：只要 demo 里某个结论变了、被改成一个错值，",
          "   脚本就必须 exit 1——否则这个脚本等于没断言。",
          "",
          "只输出一个 JSON 对象，不要任何多余文字，也不要用代码块围栏：",
          '{"assert_script":"完整 python 脚本","topics":["知识点"]}',
        ].join("\n"),
      },
      "write-explain": {
        response: "text",
        system: [
          "你是一位数据库方向的技术写作者，要给一张教学卡写一份能自学的讲解正文。",
          "输出用简体中文 Markdown，直接给正文。",
        ].join("\n"),
        user: [
          "请给下面这张数据库卡片写一份讲解正文。",
          "",
          "卡片：{{title}}",
          "知识点：{{topics}}",
          "",
          "被测 demo 源码（main.py）：",
          "```python",
          "{{demo}}",
          "```",
          "",
          "原始说明（README）：",
          "{{readme}}",
          "",
          "原始笔记（NOTES）：",
          "{{notes}}",
          "",
          "要求：",
          "1. 分小节讲：这个概念解决什么问题、demo 里怎么实现、关键数据结构与不变量、常见坑。",
          "2. 必须与 demo 的真实行为一致，不许写 demo 里没有的东西。",
          "3. 篇幅 600~1200 字。",
          "4. 直接输出正文，不要加开场白。",
        ].join("\n"),
      },
      deepen: {
        response: "text",
        system: [
          "你是一位资深数据库工程师，要就一个具体问题把一个概念往下挖一层。",
          "输出用简体中文 Markdown，直接给正文。",
        ].join("\n"),
        user: [
          "请围绕下面这个问题，把「{{title}}」这张卡深挖一层。",
          "",
          "问题：{{question}}",
          "",
          "被测 demo 源码（main.py）：",
          "```python",
          "{{demo}}",
          "```",
          "",
          "原始笔记（NOTES）：",
          "{{notes}}",
          "",
          "要求：",
          "1. 回答问题：为什么这样设计、换个设计会怎样、边界与失效场景、与相邻概念的联动。",
          "2. 必须与 demo 的真实行为一致。",
          "3. 600 字以内。",
        ].join("\n"),
      },
    },
  },
  ai: {
    label: "AI",
    defaultQuestion: "这个方法在什么数据分布下会失效？超参数该怎么定？",
    scenarios: {
      "generate-assert": {
        response: "json",
        system: [
          "你是一位机器学习方向的测试工程师，要给一张手写实现的教学 demo 写断言脚本。",
          "纪律：只依据被测代码的真实行为写断言，不许编造输出，不许写恒真的断言。",
          "输出必须是且只是一个 JSON 对象，字段为 assert_script、topics。",
          "不要加解释性前后缀，不要用代码块围栏。",
        ].join("\n"),
        user: [
          "请给下面这张 AI 卡片写一份断言脚本。",
          "",
          "卡片：{{title}}",
          "知识点：{{topics}}",
          "",
          "被测 demo 源码（main.py）：",
          "```python",
          "{{demo}}",
          "```",
          "",
          "原始说明（README）：",
          "{{readme}}",
          "",
          "原始笔记（NOTES）：",
          "{{notes}}",
          "",
          "断言脚本的契约（照统一模板写，不许自创形状）：",
          "1. 用 `subprocess` 跑 demo，**不要** `import main`——demo 会在自己旁边写文件，用 subprocess 跑才不会污染仓库。",
          "2. **脚本自己把 demo 复制进临时目录再跑**，然后在临时目录里执行副本，跑完删掉临时目录。",
          "   这几个 demo **不能重复跑**（第二次会因为表已存在直接报错），就地执行必然失败，所以必须复制：",
          "   ```python",
          "   sandbox = tempfile.mkdtemp(prefix=\"study-hub-assert-\")",
          "   try:",
          "       target = os.path.join(sandbox, os.path.basename(DEMO))",
          "       shutil.copy2(DEMO, target)",
          "       proc = subprocess.run([sys.executable, target], capture_output=True, timeout=180)",
          "   finally:",
          "       shutil.rmtree(sandbox, ignore_errors=True)",
          "   ```",
          "3. **不许写死 `text=True`**。demo 的 stdout 按**本机 locale** 编码（Windows 中文环境是 GBK，Linux 是 UTF-8），",
          "   所以只写 `capture_output=True` 抓字节，再自己按 utf-8 → gbk → latin-1 依次试解码：",
          "   ```python",
          "   def decode(raw):",
          "       for enc in (\"utf-8\", \"gbk\"):",
          "           try:",
          "               return raw.decode(enc)",
          "           except UnicodeDecodeError:",
          "               continue",
          "       return raw.decode(\"utf-8\", errors=\"replace\")",
          "   ```",
          "4. demo 路径取环境变量 `STUDY_HUB_DEMO`，缺省是「与本脚本同目录的 main.py」，这一行照抄：",
          "   DEMO = os.environ.get(\"STUDY_HUB_DEMO\") or os.path.join(os.path.dirname(os.path.abspath(__file__)), \"main.py\")",
          "5. 这张卡的 demo 依赖 numpy。脚本自己 import numpy 失败时，用 try/except 捕获，打印一句中文「环境缺失：没有装 numpy，这张卡先跳过」并 exit 1，不许静默跳过。",
          "6. 跑完 demo 后，断言真实可验证的性质：维度对不对、损失有没有下降、预测落在合理区间、参数有没有真的更新等。",
          "7. 数值断言一律用容差比较（例如 abs(a - b) < 1e-6），不许写严格相等。",
          "8. 断言失败时打印中文原因并 exit 1；全部通过时 exit 0。",
          "9. 至少 3 条断言，并且至少有一条在实现被改坏时会失败（不许写恒真断言）。",
          "10. 断言脚本必须**覆盖 demo 输出的每个主要结论**（每一段、每一个关键数字、每一条结论行），",
          "    不能只检查「跑完了」这种笼统信号；要**具体**检查 demo 该段输出的内容（具体字符串或具体数值）。",
          "    脚本必须能在「被测逻辑被改坏」时失败：只要 demo 里某个结论变了、被改成一个错值，",
          "    脚本就必须 exit 1——否则这个脚本等于没断言。",
          "",
          "只输出一个 JSON 对象，不要任何多余文字，也不要用代码块围栏：",
          '{"assert_script":"完整 python 脚本","topics":["知识点"]}',
        ].join("\n"),
      },
      "write-explain": {
        response: "text",
        system: [
          "你是一位机器学习方向的技术写作者，要给一张教学卡写一份能自学的讲解正文。",
          "输出用简体中文 Markdown，直接给正文。",
        ].join("\n"),
        user: [
          "请给下面这张 AI 卡片写一份讲解正文。",
          "",
          "卡片：{{title}}",
          "知识点：{{topics}}",
          "",
          "被测 demo 源码（main.py）：",
          "```python",
          "{{demo}}",
          "```",
          "",
          "原始说明（README）：",
          "{{readme}}",
          "",
          "原始笔记（NOTES）：",
          "{{notes}}",
          "",
          "要求：",
          "1. 分小节讲：这个算法解决什么问题、demo 里怎么实现、每一步的张量形状与含义、常见坑。",
          "2. 必须与 demo 的真实行为一致，不许写 demo 里没有的东西。",
          "3. 篇幅 600~1200 字。",
          "4. 直接输出正文，不要加开场白。",
        ].join("\n"),
      },
      deepen: {
        response: "text",
        system: [
          "你是一位资深机器学习工程师，要就一个具体问题把一个概念往下挖一层。",
          "输出用简体中文 Markdown，直接给正文。",
        ].join("\n"),
        user: [
          "请围绕下面这个问题，把「{{title}}」这张卡深挖一层。",
          "",
          "问题：{{question}}",
          "",
          "被测 demo 源码（main.py）：",
          "```python",
          "{{demo}}",
          "```",
          "",
          "原始笔记（NOTES）：",
          "{{notes}}",
          "",
          "要求：",
          "1. 回答问题：这个方法的假设是什么、在什么数据分布下会失效、超参数怎么定。",
          "2. 必须与 demo 的真实行为一致。",
          "3. 600 字以内。",
        ].join("\n"),
      },
    },
  },
}

/**
 * 填模板：把 {{key}} 换成 context 里的值；context 里没有的换成空串。
 * @param {string} tpl
 * @param {object} context
 * @returns {string}
 */
function renderTemplate(tpl, context) {
  const ctx = context || {}
  return String(tpl === null || tpl === undefined ? "" : tpl).replace(
    /\{\{\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*\}\}/g,
    function (whole, key) {
      const value = ctx[key]
      if (value === undefined || value === null) return ""
      return String(value)
    }
  )
}

/**
 * 这个轨有没有这个场景的模板。
 * @param {string} track
 * @param {string} scenario
 * @returns {boolean}
 */
function hasSupplyTemplate(track, scenario) {
  const table = TRACK_TEMPLATES[track]
  if (!table) return false
  return Boolean(table.scenarios[scenario])
}

/**
 * 组装供给场景的提示词。
 * @param {string} scenario generate-unit | generate-assert | write-explain | deepen
 * @param {string} track algorithm | database | ai
 * @param {object} context 单元上下文（title / difficulty / topics / statement / demo / readme / notes / question）
 * @returns {{system: string, user: string, response: "json"|"text"}}
 */
function buildSupplyPrompt(scenario, track, context) {
  const table = TRACK_TEMPLATES[track]
  if (!table) {
    throw new Error("没有这条轨的模板：" + String(track || "（空）"))
  }
  const tpl = table.scenarios[scenario]
  if (!tpl) {
    throw new Error(
      "轨道「" + table.label + "」没有场景「" + String(scenario) + "」的模板"
    )
  }
  const ctx = Object.assign({}, context || {}, {
    track: String(track),
    trackLabel: table.label,
  })
  if (!ctx.question) ctx.question = table.defaultQuestion
  return {
    system: renderTemplate(tpl.system, ctx),
    user: renderTemplate(tpl.user, ctx),
    response: tpl.response,
  }
}

/**
 * 解析供给场景的模型回复。
 * @param {string} text
 * @param {string} scenario
 * @returns {object} 成功时带 ok:true；失败时带 ok:false 与中文 reason
 */
function parseSupplyResponse(text, scenario) {
  const raw = String(text === null || text === undefined ? "" : text).trim()
  if (raw === "") {
    return { ok: false, kind: scenario, reason: "模型没有返回内容" }
  }

  if (scenario === "generate-unit") {
    const obj = extractJson(raw)
    if (!obj) {
      return { ok: false, kind: scenario, reason: "模型返回的不是合法 JSON，无法生成单元", raw: raw }
    }
    const title = String(obj.title || "").trim()
    if (title === "") return { ok: false, kind: scenario, reason: "生成结果缺标题" }
    const statement = String(obj.statement || "").trim()
    if (statement === "") return { ok: false, kind: scenario, reason: "生成结果缺题面" }
    const cases = normalizeCaseList(obj.cases)
    if (cases.length === 0) return { ok: false, kind: scenario, reason: "生成结果缺测试用例" }
    const solution = String(obj.solution || "").trim()
    if (solution === "") return { ok: false, kind: scenario, reason: "生成结果缺参考解法" }
    return {
      ok: true,
      kind: scenario,
      unit: {
        title: title,
        statement: statement,
        cases: cases,
        solution: solution,
        solutionLang: String(obj.solutionLang || "python").trim() || "python",
        difficulty:
          DIFFICULTIES.indexOf(String(obj.difficulty)) !== -1
            ? String(obj.difficulty)
            : "medium",
        topics: topicText(obj.topics) === "未标注" ? [] : normalizeTopics(obj.topics),
      },
    }
  }

  if (scenario === "generate-assert") {
    const obj = extractJson(raw)
    if (!obj) {
      return { ok: false, kind: scenario, reason: "模型返回的不是合法 JSON，无法生成断言脚本", raw: raw }
    }
    const script = String(obj.assert_script || obj.script || "").trim()
    if (script === "") {
      return { ok: false, kind: scenario, reason: "生成结果缺断言脚本" }
    }
    if (script.indexOf("STUDY_HUB_DEMO") === -1) {
      return {
        ok: false,
        kind: scenario,
        reason: "断言脚本必须用环境变量 STUDY_HUB_DEMO 找 demo（统一模板要求，缺省值是与脚本同目录的 main.py）",
      }
    }
    if (script.indexOf("subprocess") === -1) {
      return {
        ok: false,
        kind: scenario,
        reason: "断言脚本必须用 subprocess 跑 demo（不许 import main，demo 会在自己旁边写文件）",
      }
    }
    if (script.indexOf("exit") === -1) {
      return {
        ok: false,
        kind: scenario,
        reason: "断言脚本必须用退出码表示成败（exit 0 通过，非零退出码表示失败）",
      }
    }
    if (script.indexOf("tempfile") === -1 || script.indexOf("mkdtemp") === -1) {
      return {
        ok: false,
        kind: scenario,
        reason:
          "断言脚本必须自己把 demo 复制进临时目录再跑（tempfile.mkdtemp）——这几个 demo 不能重复跑，第二次会因为表已存在直接报错",
      }
    }
    if (/text\s*=\s*True/.test(script)) {
      return {
        ok: false,
        kind: scenario,
        reason:
          "断言脚本不许写死 text=True——demo 的 stdout 按本机 locale 编码（Windows 中文环境是 GBK），要抓字节后按 utf-8 → gbk → latin-1 依次试解码",
      }
    }
    return {
      ok: true,
      kind: scenario,
      script: script,
      topics: topicText(obj.topics) === "未标注" ? [] : normalizeTopics(obj.topics),
    }
  }

  if (scenario === "write-explain" || scenario === "deepen") {
    return { ok: true, kind: scenario, text: raw }
  }

  return { ok: false, kind: scenario, reason: "未知的供给场景：" + String(scenario) }
}
module.exports = {
  buildPrompt: buildPrompt,
  parseResponse: parseResponse,
  EXPLAIN_LEVELS: EXPLAIN_LEVELS,
  HINT_LEVELS: HINT_LEVELS,
  KINDS: KINDS,
  SKELETON_NEEDS: SKELETON_NEEDS,
  SUPPLY_SCENARIOS: SUPPLY_SCENARIOS,
  TRACK_TEMPLATES: TRACK_TEMPLATES,
  renderTemplate: renderTemplate,
  hasSupplyTemplate: hasSupplyTemplate,
  buildSupplyPrompt: buildSupplyPrompt,
  parseSupplyResponse: parseSupplyResponse,
}
