"use strict"
/**
 * import-codeforces.js —— 导入 Codeforces 的题面与样例
 *
 * 用法：
 *   node tools/import-codeforces.js --list
 *   node tools/import-codeforces.js --dry-run
 *   node tools/import-codeforces.js
 *   node tools/import-codeforces.js --tier 3 --limit 3
 *   node tools/import-codeforces.js --bank /path/to/bank
 *   node tools/import-codeforces.js --rewrite   （重新生成目标目录里的机器产物）
 *
 * 落在哪：
 *   默认落到工程仓内置题库 apps/algo-plan/bank/<轨道>/cf-*.md（跟代码一起进版本控制）。
 *   这不是 learning/bank：那边是人审内容仓，51 个机器抓来的文件一次性涌进去，
 *   人审 diff 全是噪音。--bank 可以改落点。
 *
 * 为什么只能拿样例、拿不到测试数据：
 *   Codeforces 的完整测试数据是打包成 zip 挂在比赛页上的，必须登录才给下（匿名请求 403）。
 *   公开能白拿的只有题面正文、输入输出说明，以及题面里写着的 Examples。
 *   所以这里的用例就是从 Examples 里提取出来的。
 *
 * 为什么按 rating 分档选：
 *   CF 有上万道题，全导进来毫无意义。用户的诉求是「算法能力逐步增强」，
 *   所以这里按 rating 切成若干档，每档取若干道，形成一条由易到难的阶梯。
 *
 * 本工具**绝不**碰任何仓的 git（不 commit、不 push），是否提交由用户决定。
 */

const fs = require("node:fs")
const path = require("node:path")

const { resolveLearningRoot, localBankDir } = require("../core/paths.js")
const { createProblem, makeCodeforcesId } = require("../core/bank/problem.js")
const { serializeProblemMarkdown, parseProblemMarkdown } = require("../core/bank/parse.js")
const links = require("../core/bank/links.js")
const cf = require("../core/bank/codeforces.js")
const bankFiles = require("./bank-files.js")
const { CF_NOTES } = require("./cf-notes.js")

/** Codeforces 题目全集接口 */
const PROBLEMSET_URL = "https://codeforces.com/api/problemset.problems"

/** Codeforces 比赛列表接口（拿比赛名字，题面里给个出处） */
const CONTEST_LIST_URL = "https://codeforces.com/api/contest.list"

/** 题面页地址 */
const PROBLEM_PAGE_BASE = "https://codeforces.com/problemset/problem/"

/** 请求头：CF 会区别对待默认的 fetch UA */
const HEADERS = {
  "User-Agent":
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36",
  "Accept-Language": "en-US,en;q=0.9",
}

/** 抓题面页的间隔（毫秒）。CF 对高频请求会直接封 IP，别太急。 */
const FETCH_DELAY_MS = 1500

/** 缓存文件名（落在 algo-plan/data/，该目录被 .gitignore 忽略） */
const CACHE_FILE = "cf-index.json"

/** 缓存有效期（毫秒）：7 天 */
const CACHE_TTL_MS = 7 * 24 * 60 * 60 * 1000

/**
 * 能力阶梯：按 rating 切档。
 *
 * 轨道对应 app 里的四条学习轨（fundamentals 基础 / interview 面试 /
 * advanced 进阶 / engineering 工程），难度对应三档。
 */
const TIERS = [
  { key: 1, name: "入门", min: 800, max: 1200, track: "fundamentals", difficulty: "easy", count: 8 },
  { key: 2, name: "基础", min: 1200, max: 1400, track: "fundamentals", difficulty: "easy", count: 8 },
  { key: 3, name: "进阶", min: 1400, max: 1600, track: "interview", difficulty: "medium", count: 8 },
  { key: 4, name: "熟练", min: 1600, max: 1800, track: "interview", difficulty: "medium", count: 8 },
  { key: 5, name: "挑战", min: 1800, max: 2100, track: "advanced", difficulty: "hard", count: 8 },
  { key: 6, name: "深入", min: 2100, max: 2400, track: "advanced", difficulty: "hard", count: 6 },
  { key: 7, name: "极限", min: 2400, max: 2800, track: "engineering", difficulty: "hard", count: 5 },
]

/**
 * CF 的 tag → 中文 topic。
 *
 * 中文词表跟存量题库对齐（动态规划 / 深度优先搜索 / 哈希表 / 贪心 / 双指针 …），
 * 这样题库里同一个算法不会冒出两套名字。
 */
const TAG_TOPICS = {
  "dp": "动态规划",
  "greedy": "贪心",
  "graphs": "图论",
  "dfs and similar": "深度优先搜索",
  "dsu": "并查集",
  "binary search": "二分查找",
  "sortings": "排序",
  "strings": "字符串",
  "math": "数学",
  "two pointers": "双指针",
  "bitmasks": "位运算",
  "trees": "树",
  "number theory": "数论",
  "combinatorics": "组合数学",
  "shortest paths": "最短路",
  "constructive algorithms": "构造",
  "brute force": "暴力枚举",
  "hashing": "哈希表",
  "divide and conquer": "分治",
  "flows": "网络流",
  "matrices": "矩阵",
  "meet-in-the-middle": "折半搜索",
  "probabilities": "概率",
  "games": "博弈",
  "data structures": "数据结构",
  "expression parsing": "表达式解析",
  "fft": "快速傅里叶变换",
  "graph matchings": "图匹配",
  "ternary search": "三分查找",
  "schedules": "调度",
  "string suffix structures": "后缀结构",
  "chinese remainder theorem": "中国剩余定理",
  "2-sat": "2-SAT",
  "communication": "通信",
  "implementation": "模拟",
}

/**
 * topic 的排序偏好：越靠前越「核心」，一个题只取前几个。
 * 顺序即 CF 常见套路的主次关系（DP > 图论 > 数学 …）。
 */
const TAG_PRIORITY = [
  "dp",
  "greedy",
  "graphs",
  "dfs and similar",
  "dsu",
  "binary search",
  "sortings",
  "strings",
  "math",
  "two pointers",
  "bitmasks",
  "trees",
  "number theory",
  "combinatorics",
  "shortest paths",
  "constructive algorithms",
  "brute force",
  "hashing",
  "divide and conquer",
  "flows",
  "matrices",
  "meet-in-the-middle",
  "probabilities",
  "games",
  "data structures",
  "expression parsing",
  "fft",
  "graph matchings",
  "ternary search",
  "schedules",
  "string suffix structures",
  "chinese remainder theorem",
  "2-sat",
  "communication",
  "implementation",
]

/** 不导入的 tag：交互题没法判，星题是特殊题（不属于常规算法阶梯） */
const SKIP_TAGS = ["interactive", "*special"]

/** 一个题最多带几个 topic */
const MAX_TOPICS = 3

/**
 * 题目的中文译名与题意概要见 tools/cf-notes.js（人工整理）。
 * 这里只留个空壳方便外部引用。
 */
const TITLE_ZH = Object.keys(CF_NOTES).reduce(function (out, key) {
  out[key] = CF_NOTES[key].title
  return out
}, {})

/** 缓存文件路径 */
function cachePath() {
  return path.join(__dirname, "..", "data", CACHE_FILE)
}

/**
 * 拉题目全集。
 * @returns {Promise<object[]>}
 */
async function fetchProblemset() {
  const res = await fetch(PROBLEMSET_URL, { headers: HEADERS })
  if (!res.ok) throw new Error("拉取 CF 题目全集失败：" + res.status)
  const body = await res.json()
  if (body.status !== "OK") throw new Error("CF 接口返回异常：" + body.status + " " + (body.comment || ""))
  return body.result.problems || []
}

/**
 * 拉比赛列表，拿「比赛号 → 比赛名」。
 * @returns {Promise<Object<string,string>>}
 */
async function fetchContestNames() {
  const res = await fetch(CONTEST_LIST_URL, { headers: HEADERS })
  if (!res.ok) throw new Error("拉取 CF 比赛列表失败：" + res.status)
  const body = await res.json()
  const names = Object.create(null)
  ;(body.result || []).forEach(function (contest) {
    if (contest.phase === "FINISHED" && contest.name) names[String(contest.id)] = contest.name
  })
  return names
}

/**
 * 读缓存（过期就返回 null）。
 * @param {string} file
 * @returns {object|null}
 */
function readCache(file) {
  try {
    const raw = JSON.parse(fs.readFileSync(file, "utf8"))
    const age = Date.now() - Number(raw.fetchedAt || 0)
    if (age >= 0 && age < CACHE_TTL_MS && raw.problems) return raw
  } catch (e) {
    // 缓存没有或坏了，重新拉
  }
  return null
}

/**
 * 取索引（带缓存）。
 * @param {{refresh?: boolean}} opts
 * @returns {Promise<{problems: object[], names: Object<string,string>, cached: boolean}>}
 */
async function loadIndex(opts) {
  const options = opts || {}
  const file = cachePath()
  if (!options.refresh) {
    const cached = readCache(file)
    if (cached) {
      return { problems: cached.problems, names: cached.names || {}, cached: true }
    }
  }
  const problems = await fetchProblemset()
  const names = await fetchContestNames()
  try {
    fs.mkdirSync(path.dirname(file), { recursive: true })
    fs.writeFileSync(
      file,
      JSON.stringify({ fetchedAt: Date.now(), problems: problems, names: names }),
      "utf8"
    )
  } catch (e) {
    // 缓存写不进去不算失败
  }
  return { problems: problems, names: names, cached: false }
}

/**
 * 这个题该不该要。
 * @param {object} p CF 元数据
 * @returns {boolean}
 */
function isCandidate(p) {
  if (!p || typeof p.rating !== "number") return false
  if (p.type && p.type !== "PROGRAMMING") return false
  const tags = p.tags || []
  for (let i = 0; i < tags.length; i++) {
    if (SKIP_TAGS.indexOf(tags[i]) !== -1) return false
  }
  return tags.length > 0
}

/**
 * 选一道题的 topic（按偏好排序，取前几个）。
 * @param {string[]} tags CF 标签
 * @returns {string[]}
 */
function pickTopics(tags) {
  const list = (tags || []).slice()
  list.sort(function (a, b) {
    const ia = TAG_PRIORITY.indexOf(a)
    const ib = TAG_PRIORITY.indexOf(b)
    const ra = ia === -1 ? 999 : ia
    const rb = ib === -1 ? 999 : ib
    if (ra !== rb) return ra - rb
    return a < b ? -1 : a > b ? 1 : 0
  })
  const out = []
  list.forEach(function (tag) {
    const topic = TAG_TOPICS[tag]
    if (!topic) return
    if (out.indexOf(topic) !== -1) return
    out.push(topic)
  })
  if (out.length === 0) return ["未分类"]
  return out.slice(0, MAX_TOPICS)
}

/**
 * 在一档里选题。
 *
 * 策略：同一档内按 rating 由低到高排，然后贪心地挑「目前出现最少的 tag」——
 * 这样一档里的题会尽量覆盖不同算法，而不是十道动态规划。
 *
 * @param {object[]} problems 全部题目
 * @param {object} tier 档位定义
 * @returns {object[]}
 */
function pickTier(problems, tier) {
  const candidates = problems.filter(function (p) {
    return isCandidate(p) && p.rating >= tier.min && p.rating < tier.max
  })
  candidates.sort(function (a, b) {
    if (a.rating !== b.rating) return a.rating - b.rating
    if (a.contestId !== b.contestId) return a.contestId - b.contestId
    return a.index < b.index ? -1 : a.index > b.index ? 1 : 0
  })

  const used = Object.create(null)
  const chosen = []
  const taken = Object.create(null)
  const want = tier.count

  while (chosen.length < want) {
    let best = null
    let bestScore = Infinity
    for (let i = 0; i < candidates.length; i++) {
      const p = candidates[i]
      const key = p.contestId + "-" + p.index
      if (taken[key]) continue
      // 这一道题里最「冷门」的 tag 出现次数 —— 越冷门越该先挑
      let score = Infinity
      const tags = p.tags || []
      for (let t = 0; t < tags.length; t++) {
        const c = used[tags[t]] || 0
        if (c < score) score = c
      }
      if (score < bestScore) {
        bestScore = score
        best = p
        if (score === 0) break
      }
    }
    if (!best) break
    const key = best.contestId + "-" + best.index
    taken[key] = true
    chosen.push(best)
    ;(best.tags || []).forEach(function (tag) {
      used[tag] = (used[tag] || 0) + 1
    })
  }
  return chosen
}

/**
 * 取一张题面页。
 *
 * 优先读本地缓存（tools/fetch-cf-pages.js 用浏览器抓下来的那批），
 * 缓存里没有才退回直连 HTTP——但 codeforces.com 的网页版挂在 Cloudflare 后面，
 * 直连多半会被挑战页挡回来，所以缓存才是正经路子。
 *
 * @param {string} pagesDir 本地题面缓存目录
 * @param {number} contestId
 * @param {string} index
 * @returns {Promise<{html: string, fromCache: boolean}>}
 */
async function fetchProblemPage(pagesDir, contestId, index) {
  const key = contestId + "-" + index
  if (pagesDir) {
    const file = path.join(pagesDir, key + ".html")
    try {
      const html = fs.readFileSync(file, "utf8")
      if (html.indexOf("problem-statement") !== -1) return { html: html, fromCache: true }
    } catch (e) {
      // 本地没有，继续直连
    }
  }
  const url = PROBLEM_PAGE_BASE + contestId + "/" + encodeURIComponent(index)
  const res = await fetch(url, { headers: HEADERS })
  if (!res.ok) throw new Error("抓题面页失败 " + res.status + "：" + url)
  const html = await res.text()
  if (html.indexOf("problem-statement") === -1) {
    throw new Error("题面页没解析出题面（多半是 Cloudflare 挑战页）：" + url)
  }
  return { html: html, fromCache: false }
}

/**
 * 睡一会儿再抓下一页。
 * @param {number} ms
 */
function sleep(ms) {
  return new Promise(function (resolve) {
    setTimeout(resolve, ms)
  })
}

/**
 * 把一张题面页变成一道题。
 * @param {object} meta CF 元数据
 * @param {string} html 题面页 HTML
 * @param {string} contestName 比赛名
 * @param {object} tier 档位
 * @returns {object} Problem
 */
function buildProblem(meta, html, contestName, tier) {
  const parsed = cf.parseProblemPage(html)
  const spec = cf.extractSpec(html)
  const id = makeCodeforcesId(meta.contestId, meta.index)
  const sourceId = meta.contestId + "-" + meta.index
  const englishName = String(meta.name || parsed.title || "").trim()
  const note = CF_NOTES[sourceId] || null
  const zhName = note ? note.title : ""

  const parts = []
  parts.push(
    "**" +
      (contestName || "Codeforces") +
      "**　" +
      sourceId +
      "　（官方难度 rating " +
      meta.rating +
      "）"
  )
  if (englishName) parts.push("原题名：" + englishName)
  // 中文题意放在最前面：读者第一眼要看到的是「这题要我干什么」
  if (note && note.note) parts.push("**题意（中文）**\n\n" + note.note)
  if (parsed.statement) parts.push("**原题面（英文）**\n\n" + parsed.statement)
  if (spec) parts.push(spec)
  if (parsed.timeLimit || parsed.memoryLimit) {
    parts.push(
      "**限制**：每组测试 " + (parsed.timeLimit || "未标注") + "，" + (parsed.memoryLimit || "未标注")
    )
  }

  const cases = (parsed.samples || []).map(function (sample) {
    return { input: sample.input, expect: sample.output, tol: null }
  })

  const extraSections = [
    {
      title: "题目信息",
      content: [
        "- 来源：Codeforces " + sourceId,
        "- 比赛：" + (contestName || "未标注"),
        "- 官方难度：rating " + meta.rating,
        "- 官方标签：" + (meta.tags || []).join("、"),
        "- 原题链接：" + links.codeforcesUrl(meta.contestId, meta.index),
        "",
        "> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。",
        "> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。",
        "> 参考解法尚未补充，`参考解法` 一节留空。",
      ].join("\n"),
    },
  ]

  return createProblem({
    id: id,
    source: "codeforces",
    sourceId: sourceId,
    // 标题用中文译名（或英文原名），不带 "A." 这种题号前缀——
    // 前缀在 id 和「题目信息」里都有，列表里再带一遍只是噪音。
    title: zhName || englishName,
    difficulty: tier.difficulty,
    topics: pickTopics(meta.tags),
    tracks: [tier.track],
    status: "todo",
    // 题面和用例有了，参考解法还没有，所以标成待补
    needsFill: true,
    url: links.codeforcesUrl(meta.contestId, meta.index),
    searchUrl: "",
    statement: parts.join("\n\n"),
    cases: cases,
    solution: "",
    solutionLang: "",
    extraSections: extraSections,
  })
}

/**
 * 主流程。
 * @param {{env?: object, dryRun?: boolean, list?: boolean, refresh?: boolean, tier?: number, limit?: number,
 *          learningRoot?: string, bankDir?: string, rewrite?: boolean}} opts
 * @returns {Promise<object>}
 */
async function run(opts) {
  const options = opts || {}
  // 落盘目标：工程仓内置题库（apps/algo-plan/bank/）。
  // 为什么不是 learning/bank：那是人审内容仓，51 个机器抓来的文件一次性涌进去，
  // 人审 diff 全是噪音；而 CF 原题属于「应用自带的题源」，跟着工程仓走更合适。
  const bankDir = options.bankDir ? path.resolve(options.bankDir) : localBankDir(options.env || process.env)
  // learning 仓只用来查重（那边的同 id 题目不能被覆盖），没有它也能导。
  const learning = resolveLearningRoot(options.env || process.env, options.learningRoot)

  const pagesDir = options.pagesDir === undefined ? path.join(__dirname, "..", "data", "cf-pages") : options.pagesDir

  const index = await loadIndex(options)
  const tiers = options.tier
    ? TIERS.filter(function (t) {
        return t.key === Number(options.tier)
      })
    : TIERS

  // 先选题
  const plan = []
  tiers.forEach(function (tier) {
    const picked = pickTier(index.problems, tier)
    picked.forEach(function (meta) {
      plan.push({ meta: meta, tier: tier })
    })
  })

  if (options.list) {
    plan.forEach(function (item) {
      const key = item.meta.contestId + "-" + item.meta.index
      console.log(
        "档" +
          item.tier.key +
          " [" +
          item.tier.name +
          "] " +
          key +
          "  rating=" +
          item.meta.rating +
          "  " +
          item.meta.name +
          "  tags=" +
          (item.meta.tags || []).join(",")
      )
    })
    return { ok: true, planned: plan.length, imported: [], warnings: [] }
  }

  let todo = plan
  if (options.limit) todo = todo.slice(0, Number(options.limit))

  const bankDirExisting = bankFiles.indexExistingProblems(bankDir)
  const existing = Object.assign(
    Object.create(null),
    learning.ok ? bankFiles.indexExistingProblems(path.join(learning.root, "bank")) : null,
    bankDirExisting
  )
  // --rewrite 只允许覆盖「目标题库目录里」已有的同 id 文件；
  // learning 仓（人审内容）里的同 id 题目永远跳过，不给覆盖的余地。
  const rewrite = options.rewrite === true

  const imported = []
  const warnings = []
  let skipped = 0
  let failed = 0

  for (let i = 0; i < todo.length; i++) {
    const item = todo[i]
    const meta = item.meta
    const sourceId = meta.contestId + "-" + meta.index
    const id = makeCodeforcesId(meta.contestId, meta.index)

    if (existing[id] && !(rewrite && bankDirExisting[id])) {
      skipped += 1
      continue
    }

    let html = ""
    let fromCache = true
    try {
      const page = await fetchProblemPage(pagesDir, meta.contestId, meta.index)
      html = page.html
      fromCache = page.fromCache
    } catch (e) {
      failed += 1
      warnings.push(sourceId + "：抓题面失败 —— " + (e && e.message ? e.message : e))
      continue
    }

    let problem
    try {
      problem = buildProblem(meta, html, index.names[String(meta.contestId)] || "", item.tier)
    } catch (e) {
      failed += 1
      warnings.push(sourceId + "：题目字段非法 —— " + (e && e.message ? e.message : e))
      continue
    }

    if (!problem.statement || problem.statement.length < 40) {
      failed += 1
      warnings.push(sourceId + "：题面抓出来是空的，跳过")
      continue
    }
    if (!problem.cases || problem.cases.length === 0) {
      failed += 1
      warnings.push(sourceId + "：题面里没找到样例，跳过")
      continue
    }

    const text = serializeProblemMarkdown(problem)
    // 写盘前自己再读一遍：读不回来的题绝不能落盘
    const check = parseProblemMarkdown(text).problem
    if (!check || check.id !== id || check.cases.length !== problem.cases.length) {
      failed += 1
      warnings.push(sourceId + "：序列化后读不回来，跳过")
      continue
    }

    if (!options.dryRun) {
      const dir = path.join(bankDir, problem.tracks[0])
      const target = path.join(dir, id + ".md")
      // 二次保险：目标文件已经存在就绝不覆盖（题库是人审内容，机器不该盖掉人的版本）。
      // --rewrite 是显式例外：只对目标目录里的机器产物生效。
      const existed = fs.existsSync(target)
      if (existed && !rewrite) {
        skipped += 1
        continue
      }
      fs.mkdirSync(dir, { recursive: true })
      fs.writeFileSync(target, text, "utf8")
      process.stdout.write(
        (existed ? "  重写 " : "  新建 ") + path.relative(bankDir, target) + "\n"
      )
    } else {
      const dir = path.join(bankDir, problem.tracks[0])
      const target = path.join(dir, id + ".md")
      process.stdout.write(
        "  将新建 " +
          path.relative(bankDir, target) +
          (fs.existsSync(target) ? "（已存在，会跳过）" : "") +
          "\n"
      )
    }
    imported.push({ id: id, sourceId: sourceId, tier: item.tier.key, cases: problem.cases.length })

    // 限速，别把 CF 抓急了（读本地缓存时不用等）
    if (!fromCache && i < todo.length - 1) await sleep(FETCH_DELAY_MS)
  }

  return {
    ok: true,
    bankDir: bankDir,
    learningRoot: learning.ok ? learning.root : null,
    planned: todo.length,
    imported: imported,
    skipped: skipped,
    failed: failed,
    cached: index.cached,
    warnings: warnings,
  }
}

/**
 * 命令行入口。
 */
async function main() {
  const argv = process.argv.slice(2)
  const dryRun = argv.indexOf("--dry-run") !== -1
  const list = argv.indexOf("--list") !== -1
  const refresh = argv.indexOf("--refresh") !== -1
  const tierArg = argv.indexOf("--tier")
  const limitArg = argv.indexOf("--limit")
  const pagesArg = argv.indexOf("--pages")
  const bankArg = argv.indexOf("--bank")
  const rewrite = argv.indexOf("--rewrite") !== -1

  const result = await run({
    dryRun: dryRun,
    list: list,
    refresh: refresh,
    rewrite: rewrite,
    tier: tierArg === -1 ? 0 : Number(argv[tierArg + 1]),
    limit: limitArg === -1 ? 0 : Number(argv[limitArg + 1]),
    pagesDir: pagesArg === -1 ? undefined : argv[pagesArg + 1],
    bankDir: bankArg === -1 ? undefined : argv[bankArg + 1],
  })

  if (!result.ok) {
    console.error("导入失败：" + result.reason)
    process.exit(1)
  }

  if (list) {
    console.log("选中 " + result.planned + " 道")
    return
  }

  console.log(
    (dryRun ? "（试运行）" : "") +
      "计划 " +
      result.planned +
      " 道，新建 " +
      result.imported.length +
      " 道，跳过（已存在，未覆盖）" +
      result.skipped +
      " 道，失败 " +
      result.failed +
      " 道"
  )
  if (result.warnings.length > 0) {
    console.log("告警 " + result.warnings.length + " 条：")
    result.warnings.slice(0, 20).forEach(function (line) {
      console.log("  ! " + line)
    })
    if (result.warnings.length > 20) {
      console.log("  （其余 " + (result.warnings.length - 20) + " 条省略）")
    }
  }
  console.log("提示：导入器不碰 learning 仓的 git，是否提交由你自己决定。")
  console.log("落盘目录：" + result.bankDir)
}

if (require.main === module) {
  main().catch(function (e) {
    console.error("导入失败：" + (e && e.message ? e.message : e))
    process.exit(1)
  })
}

module.exports = {
  run: run,
  main: main,
  loadIndex: loadIndex,
  fetchProblemset: fetchProblemset,
  fetchContestNames: fetchContestNames,
  pickTier: pickTier,
  pickTopics: pickTopics,
  buildProblem: buildProblem,
  TIERS: TIERS,
  TAG_TOPICS: TAG_TOPICS,
  TITLE_ZH: TITLE_ZH,
  CACHE_FILE: CACHE_FILE,
  CACHE_TTL_MS: CACHE_TTL_MS,
  FETCH_DELAY_MS: FETCH_DELAY_MS,
}
