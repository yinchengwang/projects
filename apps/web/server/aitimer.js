"use strict"
/**
 * aitimer.js —— AI 定时任务产物浏览（服务端）
 *
 * 扫描仓库根下的 `ai-timer/` 目录，把三个 AI 定时任务的产物
 * （AI 简报、代码仓巡检、每日一特性）汇总成一个统一索引，供前端浏览。
 *
 * 关键约束：
 * 1. `ai-timer/` 是 gitignored 目录，新克隆的仓可能根本没有它 ——
 *    此时接口必须返回 200 + `{ ok: true, exists: false, collections: [] }`，不能 500。
 * 2. 文档 id 含斜杠（`learn/db/pg-hot-update`），路由参数只匹配一段，
 *    因此读文档一律走 query 参数 `?id=`。
 * 3. 零 npm 依赖，只用 node 内置模块。
 */

const fs = require("node:fs")
const path = require("node:path")

/** 仓库根目录（本文件位于 apps/web/server/，往上三级） */
const REPO_ROOT = path.resolve(__dirname, "..", "..", "..")

/** 三个集合在 ai-timer/ 下的子目录名 */
const DIR_BRIEFING = "AI简报"
const DIR_INSPECTION = "代码仓巡检"
const DIR_LEARN = "每日一特性"

/** 每日一特性下的两级子目录 */
const LEARN_DEEP_DIR = "learn_deep"
const LEARN_INDEX_DIR = "_index"

/** 允许用环境变量覆盖 ai-timer 根目录（测试用） */
function aiTimerRoot () {
  const override = process.env.AI_TIMER_ROOT
  if (override && String(override).trim() !== "") return path.resolve(String(override).trim())
  return path.join(REPO_ROOT, "ai-timer")
}

/* ------------------------------------------------------------
 * 解析层
 * ---------------------------------------------------------- */

/**
 * 拆出 YAML frontmatter 与正文。
 *
 * 只认文档里实际出现的字段；未出现的字段不出现在 meta 里。
 * 支持的字段：stack / difficulty / status / links / ref / created / updated / tags / title。
 *
 * @param {string} text 全文
 * @returns {{meta: object, body: string}}
 */
function parseFrontmatter (text) {
  const src = typeof text === "string" ? text : ""
  const meta = {}
  // 必须是文件开头的 `---` 行，且后面紧跟一个 `---` 行才算 frontmatter
  const match = /^\uFEFF?---[ \t]*\r?\n([\s\S]*?)\r?\n---[ \t]*(?:\r?\n|$)/.exec(src)
  if (!match) return { meta: meta, body: src }
  const body = src.slice(match[0].length)
  parseMetaBlock(match[1], meta)
  return { meta: meta, body: body }
}

/**
 * 解析 frontmatter 原文块，把认得的字段写进 meta。
 * @param {string} block frontmatter 内的原文
 * @param {object} meta 目标对象（原地写入）
 * @returns {object} meta
 */
function parseMetaBlock (block, meta) {
  const lines = block.split(/\r?\n/)
  let i = 0
  while (i < lines.length) {
    const line = lines[i]
    i += 1
    if (line.trim() === "") continue
    // 顶层键必须是 `key: value`（缩进的行属于上一个键的列表）
    const kv = /^([A-Za-z_][A-Za-z0-9_-]*)[ \t]*:(.*)$/.exec(line)
    if (!kv) continue
    const key = kv[1]
    const rest = kv[2].trim()
    if (key === "links" || key === "ref" || key === "tags") {
      const parsed = parseListField(lines, i, rest)
      meta[key] = parsed.items
      i = parsed.next
      continue
    }
    if (rest === "") {
      // 形如 `key:` 后跟缩进列表 —— 对 links/ref 走上面的分支，这里只留空串
      meta[key] = ""
      continue
    }
    meta[key] = stripQuotes(rest)
  }
  return meta
}

/**
 * 解析列表型字段：兼容内联 `[a, b]` 与缩进 `- a` 多行写法。
 *
 * @param {string[]} lines 全部行
 * @param {number} start 当前键所在行的下一行下标
 * @param {string} inline 冒号后的内联内容（可能为空）
 * @returns {{items: string[], next: number}} 解析出的数组与下一行下标
 */
function parseListField (lines, start, inline) {
  const out = []
  if (inline !== "") {
    // 内联写法：[a, b] 或 [a, b, ] 或裸值
    const inner = inline.charAt(0) === "[" && inline.charAt(inline.length - 1) === "]"
      ? inline.slice(1, -1)
      : inline
    inner.split(",").forEach(function (item) {
      const v = stripQuotes(item.trim())
      if (v !== "") out.push(v)
    })
    return { items: out, next: start }
  }
  // 多行写法：`- a` 逐行
  let j = start
  while (j < lines.length) {
    const l = lines[j]
    if (l.trim() === "") { j += 1; continue }
    const item = /^[ \t]+-[ \t]+(.*)$/.exec(l)
    if (!item) break
    const v = stripQuotes(item[1].trim())
    if (v !== "") out.push(v)
    j += 1
  }
  return { items: out, next: j }
}

/**
 * 去掉首尾的成对引号。
 * @param {string} v
 * @returns {string}
 */
function stripQuotes (v) {
  if (typeof v !== "string") return ""
  const s = v.trim()
  if (s.length >= 2) {
    const first = s.charAt(0)
    const last = s.charAt(s.length - 1)
    if ((first === "\"" && last === "\"") || (first === "'" && last === "'")) {
      return s.slice(1, -1)
    }
  }
  return s
}

/**
 * 从正文里取第一个一级标题（`# 标题`）的文本。
 *
 * @param {string} body Markdown 正文
 * @returns {string} 标题文本，取不到返回空串
 */
function extractTitle (body) {
  const src = typeof body === "string" ? body : ""
  // 去掉文件头可能存在的 UTF-8 BOM，否则首行的 `#` 匹配不到
  const text = src.charAt(0) === "\uFEFF" ? src.slice(1) : src
  const m = /^#[ \t]+(.+?)[ \t]*$/m.exec(text)
  if (!m) return ""
  return m[1].replace(/[*_`]/g, "").trim()
}

/* ------------------------------------------------------------
 * 索引层
 * ---------------------------------------------------------- */

/**
 * 列出目录下的 .md 文件（不含子目录），按文件名排序。
 * 零字节的 .md 是定时任务留下的空壳（如 `_probe.md`），不算文档，跳过。
 *
 * @param {string} dir 目录绝对路径
 * @returns {string[]} 文件绝对路径
 */
function listMarkdownFiles (dir) {
  let entries
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true })
  } catch (e) {
    return []
  }
  return entries
    .filter(function (e) { return e.isFile() && /\.md$/i.test(e.name) })
    .map(function (e) { return path.join(dir, e.name) })
    .filter(function (p) {
      try {
        return fs.statSync(p).size > 0
      } catch (e) {
        return false
      }
    })
    .sort()
}

/**
 * 列出目录下的子目录。
 * @param {string} dir 目录绝对路径
 * @returns {string[]} 子目录绝对路径
 */
function listSubDirs (dir) {
  let entries
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true })
  } catch (e) {
    return []
  }
  return entries
    .filter(function (e) { return e.isDirectory() })
    .map(function (e) { return path.join(dir, e.name) })
    .sort()
}

/**
 * 读一个 .md 文件，解析出 frontmatter 与正文，取基本信息。
 *
 * @param {string} filePath 文件绝对路径
 * @param {string} id 全局唯一 id
 * @param {string} fallbackTitle 标题兜底（通常是文件名去扩展名）
 * @returns {{id: string, title: string, date: string, size: number, mtime: string,
 *            meta: object, isIndex: boolean, path: string}}
 */
function readDocEntry (filePath, id, fallbackTitle) {
  let stat = null
  let text = ""
  try {
    stat = fs.statSync(filePath)
    text = fs.readFileSync(filePath, "utf8")
  } catch (e) {
    stat = null
    text = ""
  }
  const parsed = parseFrontmatter(text)
  const meta = parsed.meta
  const title = (meta.title && String(meta.title).trim() !== "") ? String(meta.title).trim() : (extractTitle(parsed.body) || fallbackTitle)
  return {
    id: id,
    title: title,
    date: meta.created || meta.updated || "",
    size: stat ? stat.size : 0,
    mtime: stat ? stat.mtime.toISOString() : "",
    meta: meta,
    isIndex: false,
    path: filePath
  }
}

/**
 * 取系列简介：优先取 README 第一段引用块（`> ...`），否则取第一个非标题段落。
 *
 * @param {string} body Markdown 正文
 * @returns {string} 最多 120 字的简介
 */
function extractDescription (body) {
  const src = typeof body === "string" ? body : ""
  const lines = src.split(/\r?\n/)
  const quote = []
  const para = []
  let inQuote = false
  for (const raw of lines) {
    const line = raw.trim()
    if (line === "") {
      if (inQuote && quote.length > 0) break
      continue
    }
    if (line.charAt(0) === ">") {
      inQuote = true
      quote.push(line.replace(/^>+\s*/, ""))
      continue
    }
    if (line.charAt(0) === "#") {
      if (quote.length > 0 || para.length > 0) break
      continue
    }
    if (quote.length > 0) break
    para.push(line)
  }
  let desc = quote.join(" ").trim()
  if (desc === "") desc = para.join(" ").trim()
  if (desc.length > 120) desc = desc.slice(0, 120)
  return desc
}

/**
 * 构建 AI 简报集合。
 *
 * 规则：文件名以 `YYYY-MM-DD` 开头的才算简报正文，日期取该前缀；
 * `Briefing Index.md` 归为索引页。按日期倒序。
 *
 * @param {string} root ai-timer 绝对路径
 * @returns {{id: string, name: string, desc: string, docs: Array}}
 */
function buildBriefing (root) {
  const dir = path.join(root, DIR_BRIEFING)
  const docs = []
  listMarkdownFiles(dir).forEach(function (filePath) {
    const name = path.basename(filePath)
    const base = name.replace(/\.md$/i, "")
    const dateMatch = /^(\d{4}-\d{2}-\d{2})/.exec(base)
    const isIndex = !dateMatch
    const id = isIndex ? ("briefing/" + base) : ("briefing/" + dateMatch[1])
    const doc = readDocEntry(filePath, id, base)
    doc.isIndex = isIndex
    doc.date = isIndex ? (doc.meta.created || doc.meta.updated || "") : dateMatch[1]
    doc.collection = "briefing"
    docs.push(doc)
  })
  docs.sort(function (a, b) {
    // 日期倒序；索引页沉底
    if (a.isIndex !== b.isIndex) return a.isIndex ? 1 : -1
    if (a.date === b.date) return a.id < b.id ? -1 : (a.id > b.id ? 1 : 0)
    return a.date < b.date ? 1 : -1
  })
  return {
    id: "briefing",
    name: "AI 简报",
    desc: "每日数据库与 AI 领域简报，定时任务产出、按日期归档。",
    docs: docs
  }
}

/**
 * 构建代码仓巡检集合。
 *
 * 规则：文件名形如 `代码仓巡检-YYYY-MM-DD` 的，日期取那段；
 * 其余（如 `巡检指令.md`）归为指令页。按日期倒序。
 *
 * @param {string} root ai-timer 绝对路径
 * @returns {{id: string, name: string, desc: string, docs: Array}}
 */
function buildInspection (root) {
  const dir = path.join(root, DIR_INSPECTION)
  const docs = []
  listMarkdownFiles(dir).forEach(function (filePath) {
    const name = path.basename(filePath)
    const base = name.replace(/\.md$/i, "")
    const dateMatch = /^代码仓巡检-(\d{4}-\d{2}-\d{2})/.exec(base)
    const isIndex = !dateMatch
    const id = isIndex ? ("inspection/" + base) : ("inspection/" + dateMatch[1])
    const doc = readDocEntry(filePath, id, base)
    doc.isIndex = isIndex
    doc.date = isIndex ? (doc.meta.created || doc.meta.updated || "") : dateMatch[1]
    doc.collection = "inspection"
    docs.push(doc)
  })
  docs.sort(function (a, b) {
    if (a.isIndex !== b.isIndex) return a.isIndex ? 1 : -1
    if (a.date === b.date) return a.id < b.id ? -1 : (a.id > b.id ? 1 : 0)
    return a.date < b.date ? 1 : -1
  })
  return {
    id: "inspection",
    name: "代码仓巡检",
    desc: "每日代码仓只读巡检报告，记录任务进展与冗余证据。",
    docs: docs
  }
}

/**
 * 构建「每日一特性」集合（内含各领域系列）。
 *
 * @param {string} root ai-timer 绝对路径
 * @returns {{id: string, name: string, desc: string, series: Array}}
 */
function buildLearn (root) {
  const learnDir = path.join(root, DIR_LEARN)
  const deepDir = path.join(learnDir, LEARN_DEEP_DIR)
  const indexDir = path.join(learnDir, LEARN_INDEX_DIR)
  const series = []

  listSubDirs(deepDir).forEach(function (seriesDir) {
    const seriesId = path.basename(seriesDir)
    const docs = []
    listMarkdownFiles(seriesDir).forEach(function (filePath) {
      const base = path.basename(filePath).replace(/\.md$/i, "")
      const isIndex = base.toLowerCase() === "readme"
      const slug = isIndex ? "index" : base
      const doc = readDocEntry(filePath, "learn/" + seriesId + "/" + slug, base)
      doc.isIndex = isIndex
      doc.series = seriesId
      doc.collection = "learn"
      docs.push(doc)
    })
    // 首页置顶，其余按文件名排序
    docs.sort(function (a, b) {
      if (a.isIndex !== b.isIndex) return a.isIndex ? -1 : 1
      return a.id < b.id ? -1 : (a.id > b.id ? 1 : 0)
    })

    // 系列名：取 _index/<领域>.md 的第一个一级标题，取不到用目录名
    let name = seriesId
    const indexFile = path.join(indexDir, seriesId + ".md")
    if (fs.existsSync(indexFile)) {
      try {
        const parsed = parseFrontmatter(fs.readFileSync(indexFile, "utf8"))
        const t = extractTitle(parsed.body)
        if (t !== "") name = t
      } catch (e) {
        // 索引页读不了就退回目录名
      }
    }

    // 系列简介：取该系列 README 的第一段引用块或第一个非标题段落
    let desc = ""
    const readmeFile = path.join(seriesDir, "README.md")
    if (fs.existsSync(readmeFile)) {
      try {
        const parsed = parseFrontmatter(fs.readFileSync(readmeFile, "utf8"))
        desc = extractDescription(parsed.body)
      } catch (e) {
        desc = ""
      }
    }

    series.push({ id: seriesId, name: name, desc: desc, docs: docs })
  })

  return {
    id: "learn",
    name: "每日一特性",
    desc: "按领域（栈）组织的深度系列，每个领域一份 README 索引页与若干专题文章。",
    series: series,
    // series 的别名：前端按「集合元素是否自带 docs」区分「系列」与「文章」，
    // learn 的元素就是系列，所以 docs 直接指向 series。
    docs: series
  }
}

/**
 * 扫描 ai-timer 目录，产出统一索引。
 *
 * 目录不存在时返回 `{ root, exists: false, collections: [] }`，不抛异常。
 *
 * @param {string} [root] ai-timer 绝对路径，缺省取环境变量或仓库根下的 ai-timer
 * @returns {{root: string, exists: boolean, collections: Array}}
 */
function buildIndex (root) {
  const base = root ? path.resolve(root) : aiTimerRoot()
  let exists = false
  try {
    exists = fs.existsSync(base) && fs.statSync(base).isDirectory()
  } catch (e) {
    exists = false
  }
  if (!exists) return { root: base, exists: false, collections: [] }
  return {
    root: base,
    exists: true,
    collections: [
      buildBriefing(base),
      buildInspection(base),
      buildLearn(base)
    ]
  }
}

/* ------------------------------------------------------------
 * 文档读取
 * ---------------------------------------------------------- */

/**
 * 在索引里按 id 找一篇文档。
 *
 * @param {string} docId 形如 `briefing/2026-11-28`、`learn/db/pg-hot-update`
 * @param {object} [index] 已建好的索引，缺省现建
 * @returns {object|null} 命中的 doc，未命中返回 null
 */
function findDoc (docId, index) {
  const idx = index || buildIndex()
  if (!idx.exists) return null
  const id = String(docId || "")
  for (const coll of idx.collections) {
    // 只认真正的文档条目：系列条目只有 id / name / docs，没有 path
    if (Array.isArray(coll.docs)) {
      for (const doc of coll.docs) {
        if (doc && doc.path && doc.id === id) return doc
      }
    }
    if (Array.isArray(coll.series)) {
      for (const s of coll.series) {
        for (const doc of s.docs) {
          if (doc.id === id) return doc
        }
      }
    }
  }
  return null
}

/**
 * 读一篇文档：返回 frontmatter、正文与路径。
 *
 * date / size 一并带出，前端标题栏要显示「日期 · 大小」。
 *
 * @param {string} docId 文档 id
 * @returns {{id: string, title: string, date: string, size: number,
 *            meta: object, body: string, path: string}|null}
 */
function readDoc (docId) {
  const doc = findDoc(docId)
  if (!doc) return null
  let text = ""
  try {
    text = fs.readFileSync(doc.path, "utf8")
  } catch (e) {
    return null
  }
  const parsed = parseFrontmatter(text)
  return {
    id: doc.id,
    title: doc.title,
    date: doc.date,
    size: doc.size,
    meta: parsed.meta,
    body: parsed.body,
    path: doc.path
  }
}

/**
 * 读一篇文档的原始全文（含 frontmatter）。
 *
 * @param {string} docId 文档 id
 * @returns {{id: string, title: string, path: string, raw: string}|null}
 */
function readRawDoc (docId) {
  const doc = findDoc(docId)
  if (!doc) return null
  let raw = ""
  try {
    raw = fs.readFileSync(doc.path, "utf8")
  } catch (e) {
    return null
  }
  return { id: doc.id, title: doc.title, path: doc.path, raw: raw }
}

/* ------------------------------------------------------------
 * HTTP 路由
 * ---------------------------------------------------------- */

/**
 * 注册 AI 定时任务产物的三个接口。
 *
 * @param {object} router router.js 造的路由表（有 add 方法）
 * @param {Function} sendJson 发 JSON 响应的函数，签名 (res, status, body)
 * @returns {object} router（便于链式调用）
 */
function registerAiTimerRoutes (router, sendJson) {
  const reply = function (res, status, body) {
    sendJson(res, status, body)
  }

  // 完整索引
  router.add("GET", "/api/aitimer/index", function (req, res) {
    const idx = buildIndex()
    reply(res, 200, {
      ok: true,
      root: idx.root,
      exists: idx.exists,
      collections: idx.collections
    })
  })

  // 单篇文档（id 含斜杠，走 query 参数）
  router.add("GET", "/api/aitimer/doc", function (req, res, params, query) {
    const id = query ? query.get("id") : ""
    if (!id) {
      reply(res, 400, { error: "缺少 id 参数" })
      return
    }
    const doc = readDoc(id)
    if (!doc) {
      reply(res, 404, { error: "未找到文档：" + id })
      return
    }
    reply(res, 200, { ok: true, doc: doc })
  })

  // 原始 Markdown 全文（含 frontmatter）
  router.add("GET", "/api/aitimer/raw", function (req, res, params, query) {
    const id = query ? query.get("id") : ""
    if (!id) {
      reply(res, 400, { error: "缺少 id 参数" })
      return
    }
    const raw = readRawDoc(id)
    if (!raw) {
      reply(res, 404, { error: "未找到文档：" + id })
      return
    }
    reply(res, 200, { ok: true, id: raw.id, title: raw.title, path: raw.path, raw: raw.raw })
  })

  return router
}

/**
 * 兼容别名：与 registerAiTimerRoutes 等价。
 * @param {object} router
 * @param {Function} sendJson
 * @returns {object} router
 */
function createAiTimerRoutes (router, sendJson) {
  return registerAiTimerRoutes(router, sendJson)
}

module.exports = {
  REPO_ROOT: REPO_ROOT,
  aiTimerRoot: aiTimerRoot,
  parseFrontmatter: parseFrontmatter,
  extractTitle: extractTitle,
  extractDescription: extractDescription,
  buildIndex: buildIndex,
  findDoc: findDoc,
  readDoc: readDoc,
  readRawDoc: readRawDoc,
  registerAiTimerRoutes: registerAiTimerRoutes,
  createAiTimerRoutes: createAiTimerRoutes
}
