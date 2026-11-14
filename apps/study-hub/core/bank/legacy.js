"use strict"
/**
 * legacy.js —— 死脚本内联题目清单的静态解析（纯函数层，无 IO）
 *
 * 对应设计文档 §3.5 的「B 路：复活死代码」。
 *
 * 为什么必须静态解析、绝不执行被解析的脚本：
 *   1. 三个脚本都读已删除的 project/reading-radar/data/app/practice-data.js，直接跑必然 ENOENT；
 *   2. gen-practice-data.js 里有 6 处题号写成了未定义标识符
 *      （offer22 / offer18 / deleteNode / offer23 / offer09 / offer30），真跑会 ReferenceError；
 *   3. 死脚本唯一的价值就是「源码里的内联清单」，执行它们反而多余。
 *
 * 本模块只做「按下面的规则解析源码文本」：
 *   - cat("slug", "标题", phase, order, icon, desc, items变量)   → gen-practice-data.js
 *   - addProblems("slug", [ ... ])                              → expand-practice.js
 *   - 两种都没有                                                  → split-practice-data.js（记告警）
 */

const { makeId, normalizeTopic } = require("./problem.js")

/** 分类 slug → 轨道。25 个 slug 全覆盖，理由见 M2 计划的映射表。 */
const CATEGORY_TRACK_MAP = {
  // fundamentals：基础数据结构与基础算法
  "array": "fundamentals",
  "string": "fundamentals",
  "linked-list": "fundamentals",
  "stack": "fundamentals",
  "queue": "fundamentals",
  "sorting": "fundamentals",
  "binary-tree": "fundamentals",
  "bst": "fundamentals",
  "heap": "fundamentals",
  "bit-manipulation": "fundamentals",
  // interview：面试高频套路
  "two-pointers": "interview",
  "sliding-window": "interview",
  "prefix-sum": "interview",
  "hash-table": "interview",
  "binary-search": "interview",
  "dfs": "interview",
  "bfs": "interview",
  "backtracking": "interview",
  "interview-classic": "interview",
  // advanced：需要证明或套路偏深的题型
  "greedy": "advanced",
  "dp-basic": "advanced",
  "dp-advanced": "advanced",
  "union-find": "advanced",
  "graph": "advanced",
  "math": "advanced",
}

/** 未知 slug 的兜底轨道（契约 §1 钉死） */
const FALLBACK_TRACK = "interview"

/**
 * 标签别名表：原始写法 → 统一写法。
 * 设计文档 §3.4 要求「同一知识点只能有一种写法」。
 * 刻意不合并的：哈希集合/哈希表、二分查找/二分答案、抽样/蓄水池抽样。
 */
const TOPIC_ALIASES = {
  "哈希": "哈希表",
  "哈希映射": "哈希表",
  "二分": "二分查找",
  "标准二分": "二分查找",
  "变体二分": "二分查找",
  "答案二分": "二分答案",
  "DP": "动态规划",
  "BFS": "广度优先搜索",
  "DFS": "深度优先搜索",
  "BST": "二叉搜索树",
  "双射": "双射哈希",
  "归并": "归并排序",
  "变长": "变长窗口",
  "定长": "定长窗口",
  "从后往前": "倒序遍历",
  "原地标记": "原地哈希",
}

/**
 * 从 open 处的括号开始，找到配对的右括号位置。
 * 同时统计 () [] {} 三种括号，并跳过字符串字面量里的括号。
 * 反引号模板字符串不做插值处理（死脚本里没有用到）。
 * @param {string} text
 * @param {number} open 左括号下标
 * @returns {number} 配对右括号下标；找不到返回 -1
 */
function findClosing(text, open) {
  let depth = 0
  let quote = null
  for (let i = open; i < text.length; i++) {
    const ch = text[i]
    if (quote) {
      if (ch === "\\") {
        i++
        continue
      }
      if (ch === quote) quote = null
      continue
    }
    if (ch === '"' || ch === "'" || ch === "`") {
      quote = ch
      continue
    }
    if (ch === "(" || ch === "[" || ch === "{") {
      depth++
    } else if (ch === ")" || ch === "]" || ch === "}") {
      depth--
      if (depth === 0) return i
    }
  }
  return -1
}

/**
 * 按顶层逗号切分实参（字符串里的逗号、括号里的逗号都不算）。
 * @param {string} text
 * @returns {string[]}
 */
function splitArgs(text) {
  const args = []
  let depth = 0
  let quote = null
  let cur = ""
  for (let i = 0; i < text.length; i++) {
    const ch = text[i]
    if (quote) {
      cur += ch
      if (ch === "\\") {
        cur += text[++i] || ""
        continue
      }
      if (ch === quote) quote = null
      continue
    }
    if (ch === '"' || ch === "'" || ch === "`") {
      quote = ch
      cur += ch
      continue
    }
    if (ch === "(" || ch === "[" || ch === "{") {
      depth++
      cur += ch
      continue
    }
    if (ch === ")" || ch === "]" || ch === "}") {
      depth--
      cur += ch
      continue
    }
    if (ch === "," && depth === 0) {
      args.push(cur)
      cur = ""
      continue
    }
    cur += ch
  }
  if (cur.trim() !== "") args.push(cur)
  return args
}

/**
 * 实参是不是字符串字面量？是则返回内容，否则返回 null。
 * @param {string} arg
 * @returns {string|null}
 */
function readStringLiteral(arg) {
  const s = String(arg === undefined || arg === null ? "" : arg).trim()
  if (s.length < 2) return null
  const quote = s[0]
  if (quote !== '"' && quote !== "'" && quote !== "`") return null
  if (s[s.length - 1] !== quote) return null
  return s.slice(1, -1)
}

/**
 * 找出源码里所有 `var|let|const 名字 = [` 的数组字面量。
 * @param {string} src
 * @returns {Object<string,string>} 名字 → 数组体文本（不含最外层方括号）
 */
function harvestVarArrays(src) {
  const out = Object.create(null)
  const re = /(?:var|let|const)\s+([A-Za-z_$][\w$]*)\s*=\s*\[/g
  let m
  while ((m = re.exec(src))) {
    const open = src.indexOf("[", m.index)
    const close = findClosing(src, open)
    if (close === -1) continue
    out[m[1]] = src.slice(open + 1, close)
  }
  return out
}

/**
 * 找出源码里所有 `fnName(` 调用（跳过函数定义本身），返回实参文本列表。
 * @param {string} src
 * @param {string} fnName
 * @returns {string[]}
 */
function findCalls(src, fnName) {
  const out = []
  const re = new RegExp("\\b" + fnName + "\\s*\\(", "g")
  let m
  while ((m = re.exec(src))) {
    const before = src.slice(Math.max(0, m.index - 12), m.index)
    if (/function\s+$/.test(before)) continue
    const open = src.indexOf("(", m.index)
    const close = findClosing(src, open)
    if (close === -1) continue
    out.push(src.slice(open + 1, close))
  }
  return out
}

/**
 * 题号 → 数字。
 *   56      → 56
 *   "JZ03"  → 3（剑指 Offer 的题号写法，取末尾数字）
 *   offer22 → null（未定义标识符，脚本里根本没定义这个变量，不猜）
 * @param {string} raw
 * @returns {{number: number, raw: string}|null}
 */
function problemIdOf(raw) {
  const s = String(raw === undefined || raw === null ? "" : raw).trim()
  if (/^\d+$/.test(s)) return { number: Number(s), raw: s }
  const quoted = readStringLiteral(s)
  if (quoted !== null) {
    const m = /(\d+)\s*$/.exec(quoted)
    if (m) return { number: Number(m[1]), raw: quoted }
    return null
  }
  return null
}

/**
 * 从一段数组体文本里解析出 lc(...) / jz(...) 条目。
 * @param {string} body
 * @returns {{items: object[], warnings: string[]}}
 */
function parseItemCalls(body) {
  const items = []
  const warnings = []
  const re = /\b(lc|jz)\s*\(/g
  let m
  while ((m = re.exec(body))) {
    const before = body.slice(Math.max(0, m.index - 12), m.index)
    if (/function\s+$/.test(before)) continue
    const open = body.indexOf("(", m.index)
    const close = findClosing(body, open)
    if (close === -1) {
      warnings.push("括号不配对，已跳过：" + body.slice(m.index, m.index + 30))
      continue
    }
    const args = splitArgs(body.slice(open + 1, close))
    const bareTitle = readStringLiteral(args[0])
    const difficulty = readStringLiteral(args[2])
    if (bareTitle === null || difficulty === null) {
      warnings.push("标题或难度不是字符串字面量，已跳过：" + body.slice(m.index, m.index + 40))
      continue
    }
    const pid = problemIdOf(args[1])
    if (pid === null) {
      warnings.push(
        "题号不是数字（" +
          String(args[1] === undefined ? "" : args[1]).trim() +
          "），已跳过：" +
          bareTitle
      )
      continue
    }
    const tags = []
    const tagRe = /"([^"]*)"|'([^']*)'/g
    let tm
    while ((tm = tagRe.exec(args[3] || ""))) {
      tags.push(tm[1] !== undefined ? tm[1] : tm[2])
    }
    // jz() 在运行时才给标题加 "剑指 Offer <题号>. " 前缀，静态解析要还原成同样的样子
    const title = m[1] === "jz" ? "剑指 Offer " + pid.raw + ". " + bareTitle : bareTitle
    items.push({
      platform: m[1] === "lc" ? "leetcode" : "nowcoder",
      problemId: pid.number,
      title: title,
      difficulty: difficulty,
      tags: tags,
      repeat: false,
    })
  }
  return { items: items, warnings: warnings }
}

/**
 * 解析一个死脚本的源码文本 → 分类列表。
 * @param {string} text 源码全文
 * @returns {{categories: Array<{slug: string, title: string|null, items: object[]}>, warnings: string[]}}
 */
function parseLegacyCategories(text) {
  const src = String(text === undefined || text === null ? "" : text)
  const warnings = []
  const categories = []
  const vars = harvestVarArrays(src)

  // 形状一：cat("slug", "标题", phase, order, icon, desc, items变量)
  findCalls(src, "cat").forEach(function (argsText) {
    const args = splitArgs(argsText)
    const slug = readStringLiteral(args[0])
    if (slug === null) {
      warnings.push("cat() 的第一个实参不是字符串字面量，已跳过")
      return
    }
    const title = readStringLiteral(args[1])
    const itemsArg = String(args[6] === undefined ? "" : args[6]).trim()
    let body = null
    if (itemsArg.startsWith("[")) {
      body = itemsArg.slice(1, itemsArg.lastIndexOf("]"))
    } else {
      const name = itemsArg.replace(/;$/, "").trim()
      body = vars[name]
      if (body === undefined) {
        warnings.push("cat(" + slug + ") 的题目数组变量 " + name + " 找不到，已跳过")
        return
      }
    }
    const parsed = parseItemCalls(body)
    parsed.warnings.forEach(function (w) {
      warnings.push(slug + "：" + w)
    })
    categories.push({ slug: slug, title: title, items: parsed.items })
  })

  // 形状二：addProblems("slug", [ ... ])
  findCalls(src, "addProblems").forEach(function (argsText) {
    const args = splitArgs(argsText)
    const slug = readStringLiteral(args[0])
    if (slug === null) {
      warnings.push("addProblems() 的第一个实参不是字符串字面量，已跳过")
      return
    }
    const arr = String(args[1] === undefined ? "" : args[1]).trim()
    const body = arr.startsWith("[") ? arr.slice(1, arr.lastIndexOf("]")) : ""
    const parsed = parseItemCalls(body)
    parsed.warnings.forEach(function (w) {
      warnings.push(slug + "：" + w)
    })
    categories.push({ slug: slug, title: null, items: parsed.items })
  })

  // 同一个 slug 可能被追加好几批（expand-practice.js 里 5 个 slug 各追加过多次）→ 按 slug 合并
  const merged = []
  categories.forEach(function (cat) {
    const exist = merged.filter(function (c) {
      return c.slug === cat.slug
    })[0]
    if (!exist) {
      merged.push({ slug: cat.slug, title: cat.title, items: cat.items.slice() })
      return
    }
    if (!exist.title && cat.title) exist.title = cat.title
    exist.items = exist.items.concat(cat.items)
  })

  // repeat 标记：同一个 (platform, problemId) 第二次出现即为重复
  const seen = Object.create(null)
  merged.forEach(function (cat) {
    cat.items.forEach(function (item) {
      const key = item.platform + ":" + item.problemId
      if (seen[key]) item.repeat = true
      else seen[key] = true
    })
  })

  if (merged.length === 0) {
    warnings.push("脚本里没有找到内联题目清单（既没有 cat() 也没有 addProblems() 调用）")
  }
  return { categories: merged, warnings: warnings }
}

/**
 * 解析死脚本源码文本 → 扁平题目清单（契约 §1 钉死的形状）。
 * @param {string} text
 * @returns {{items: Array<{platform, problemId, title, difficulty, tags, repeat}>, warnings: string[]}}
 */
function parseLegacyPracticeData(text) {
  const parsed = parseLegacyCategories(text)
  const items = []
  parsed.categories.forEach(function (cat) {
    cat.items.forEach(function (item) {
      items.push(item)
    })
  })
  return { items: items, warnings: parsed.warnings }
}

/**
 * 分类 slug → 轨道。未知 slug 落到 FALLBACK_TRACK（调用方负责计告警）。
 * @param {string} slug
 * @returns {string}
 */
function mapCategoryToTrack(slug) {
  const key = String(slug === undefined || slug === null ? "" : slug)
  if (Object.prototype.hasOwnProperty.call(CATEGORY_TRACK_MAP, key)) {
    return CATEGORY_TRACK_MAP[key]
  }
  return FALLBACK_TRACK
}

/**
 * 死脚本条目 → 稳定 ID（用 M1 的 makeId）。
 * platform: leetcode → lc，nowcoder → nc（契约 §1 钉死）。
 * @param {{platform: string, problemId: number|string}} item
 * @returns {string}
 */
function legacyIdOf(item) {
  if (!item) throw new Error("条目为空")
  const platform = String(item.platform || "")
  const pid = problemIdOf(item.problemId)
  if (pid === null) throw new Error("题号取不到数字：" + item.problemId)
  return makeId(platform, pid.number)
}

/**
 * 标签归一化：先走 M1 的 normalizeTopic，再查别名表。
 * @param {string} text
 * @returns {string}
 */
function normalizeLegacyTopic(text) {
  const base = normalizeTopic(text)
  if (base === "") return ""
  if (Object.prototype.hasOwnProperty.call(TOPIC_ALIASES, base)) {
    return TOPIC_ALIASES[base]
  }
  return base
}

/**
 * 标签列表归一化：去空、去重、保序。
 * @param {string[]|string} list
 * @returns {string[]}
 */
function normalizeLegacyTopics(list) {
  const input = Array.isArray(list) ? list : list ? [list] : []
  const seen = Object.create(null)
  const out = []
  input.forEach(function (item) {
    const topic = normalizeLegacyTopic(item)
    if (topic === "" || seen[topic]) return
    seen[topic] = true
    out.push(topic)
  })
  return out
}

module.exports = {
  CATEGORY_TRACK_MAP: CATEGORY_TRACK_MAP,
  FALLBACK_TRACK: FALLBACK_TRACK,
  TOPIC_ALIASES: TOPIC_ALIASES,
  parseLegacyCategories: parseLegacyCategories,
  parseLegacyPracticeData: parseLegacyPracticeData,
  mapCategoryToTrack: mapCategoryToTrack,
  legacyIdOf: legacyIdOf,
  normalizeLegacyTopic: normalizeLegacyTopic,
  normalizeLegacyTopics: normalizeLegacyTopics,
}
