"use strict"
/**
 * sync-links.js —— 回填题库的真实题号与原题链接
 *
 * 用法：
 *   ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/sync-links.js --dry-run
 *   ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/sync-links.js
 *   ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/sync-links.js --refresh
 *
 * 干两件事：
 *   1. 把 source_id（源站真实题号）写进题库 frontmatter；
 *   2. 把原题链接（url）写进题库 frontmatter。
 *
 * 只动 frontmatter 里指定的行，正文（题面、用例、参考解法）一个字都不碰——
 * 那些是人审内容，机器不该盖掉人的版本。
 *
 * 数据来源：
 *   LeetCode  https://leetcode.cn/api/problems/all/   题号 → slug
 *              https://leetcode.cn/graphql/             slug → 中文标题（批量别名查询）
 *   牛客      拿不到直达地址（题目地址是一段不可推导的 hash），退到搜索页
 *
 * 索引会缓存到 algo-plan/data/lc-index.json（默认 7 天过期，--refresh 强制重建）。
 *
 * 本工具**绝不**碰 learning 仓的 git（不 commit、不 push）。
 */

const fs = require("node:fs")
const path = require("node:path")

const { resolveLearningRoot } = require("../core/paths.js")
const { parseProblemMarkdown } = require("../core/bank/parse.js")
const links = require("../core/bank/links.js")
const bankFiles = require("./bank-files.js")

/** LeetCode 题目全集接口 */
const PROBLEMSET_URL = "https://leetcode.cn/api/problems/all/"

/** LeetCode GraphQL 接口（拿中文标题） */
const GRAPHQL_URL = "https://leetcode.cn/graphql/"

/** 请求头：站点会区别对待默认的 fetch UA，带上浏览器 UA 才有中文标题 */
const HEADERS = {
  "User-Agent":
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36",
  Referer: "https://leetcode.cn/problemset/",
}

/** 一次 GraphQL 查多少道题（用别名批量查，省往返） */
const GRAPHQL_BATCH = 50

/** 缓存文件名 */
const CACHE_FILE = "lc-index.json"

/** 缓存有效期（毫秒）：7 天 */
const CACHE_TTL_MS = 7 * 24 * 60 * 60 * 1000

/** 缓存文件路径 */
function cachePath() {
  return path.join(__dirname, "..", "data", CACHE_FILE)
}

/**
 * 拉题目全集，建立「题号 → slug」索引，顺带记下难度。
 * @returns {Promise<{byId: Object<string,string>, difficulty: Object<string,number>}>}
 */
async function fetchProblemset() {
  const res = await fetch(PROBLEMSET_URL, { headers: HEADERS })
  if (!res.ok) throw new Error("拉取题目全集失败：" + res.status)
  const body = await res.json()
  const pairs = body.stat_status_pairs || []
  const byId = {}
  const difficulty = {}
  pairs.forEach(function (pair) {
    const stat = pair.stat || {}
    const id = String(stat.frontend_question_id || "")
    // 只收纯数字题号：LCP 82、面试题 16 之类的非数字编号用不上
    if (!/^\d+$/.test(id)) return
    const slug = String(stat.question__title_slug || "")
    if (slug === "") return
    byId[id] = slug
    const level = pair.difficulty && Number(pair.difficulty.level)
    if (level) difficulty[slug] = level
  })
  return { byId: byId, difficulty: difficulty }
}

/**
 * 批量查 slug 的中文标题（GraphQL 别名批量查，一批 50 道）。
 * @param {string[]} slugs
 * @returns {Promise<Object<string,string>>} slug → 中文标题
 */
async function fetchCnTitles(slugs) {
  const out = {}
  const batches = []
  for (let i = 0; i < slugs.length; i += GRAPHQL_BATCH) {
    batches.push(slugs.slice(i, i + GRAPHQL_BATCH))
  }
  let cursor = 0

  async function worker() {
    while (cursor < batches.length) {
      const batch = batches[cursor++]
      const variables = {}
      const declarations = []
      const fields = batch
        .map(function (slug, i) {
          variables["s" + i] = slug
          declarations.push("$s" + i + ":String!")
          return "q" + i + ": question(titleSlug:$s" + i + "){questionId translatedTitle}"
        })
        .join(" ")
      const query = "query(" + declarations.join(",") + ") {" + fields + "}"
      try {
        const res = await fetch(GRAPHQL_URL, {
          method: "POST",
          headers: Object.assign({ "Content-Type": "application/json" }, HEADERS),
          body: JSON.stringify({ query: query, variables: variables }),
        })
        if (!res.ok) continue
        const body = await res.json()
        const data = body.data || {}
        batch.forEach(function (slug, i) {
          const node = data["q" + i]
          if (node && node.translatedTitle) out[slug] = node.translatedTitle
        })
      } catch (e) {
        // 单批失败就跳过这一批：宁可少几条标题，也不要整个工具挂掉
      }
    }
  }

  await Promise.all([worker(), worker(), worker(), worker()])
  return out
}

/**
 * 组装反查索引：归一化后的中文标题 → slug 列表。
 * @param {Object<string,string>} titles slug → 中文标题
 * @returns {Object<string,string[]>}
 */
function buildTitleIndex(titles) {
  const byTitle = Object.create(null)
  Object.keys(titles).forEach(function (slug) {
    const key = links.normalizeTitle(titles[slug])
    if (key === "") return
    if (!byTitle[key]) byTitle[key] = []
    byTitle[key].push(slug)
  })
  return byTitle
}

/**
 * 取索引（带缓存）。
 * @param {{refresh?: boolean, cacheFile?: string}} opts
 * @returns {Promise<{byId: Object<string,string>, titles: Object<string,string>, byTitle: Object<string,string[]>}>}
 */
async function loadIndex(opts) {
  const options = opts || {}
  const file = options.cacheFile || cachePath()

  if (!options.refresh) {
    try {
      const raw = JSON.parse(fs.readFileSync(file, "utf8"))
      const age = Date.now() - Number(raw.fetchedAt || 0)
      if (age >= 0 && age < CACHE_TTL_MS && raw.byId) {
        return {
          byId: raw.byId,
          titles: raw.titles || {},
          difficulty: raw.difficulty || {},
          byTitle: buildTitleIndex(raw.titles || {}),
          cached: true,
        }
      }
    } catch (e) {
      // 缓存没有或坏了，重新拉
    }
  }

  const ps = await fetchProblemset()
  const slugs = Object.keys(ps.byId).map(function (k) {
    return ps.byId[k]
  })
  const titles = await fetchCnTitles(slugs)

  try {
    fs.mkdirSync(path.dirname(file), { recursive: true })
    fs.writeFileSync(
      file,
      JSON.stringify({
        fetchedAt: Date.now(),
        byId: ps.byId,
        titles: titles,
        difficulty: ps.difficulty,
      }),
      "utf8"
    )
  } catch (e) {
    // 缓存写不进去不算失败，接着用内存里的索引
  }

  return {
    byId: ps.byId,
    titles: titles,
    difficulty: ps.difficulty,
    byTitle: buildTitleIndex(titles),
    cached: false,
  }
}

/**
 * 从题库 id 里取源站题号。
 *   lc-00027 → 27；nc-00003 → 3（剑指 Offer 3）；cf-1234-a → 不适用（CF 另有来源）
 *
 * 前导零要去掉：题库 id 里的数字是补零到 5 位的（lc-00007），
 * 而源站索引的 key 是原样题号（"7"），不去零就永远对不上。
 * @param {string} id
 * @returns {string}
 */
function sourceIdFromId(id) {
  const m = /^([a-z]+)-(\d+)$/.exec(String(id || ""))
  if (!m) return ""
  return String(Number(m[2]))
}

/**
 * 主流程。
 * @param {{env?: object, dryRun?: boolean, refresh?: boolean, learningRoot?: string}} opts
 * @returns {Promise<object>}
 */
async function run(opts) {
  const options = opts || {}
  const learning = resolveLearningRoot(options.env || process.env, options.learningRoot)
  if (!learning.ok) {
    return { ok: false, reason: learning.reason, updated: [], warnings: [] }
  }

  const index = await loadIndex(options)
  const bankDir = path.join(learning.root, "bank")
  const files = bankFiles.listProblemFiles(bankDir)

  const updated = []
  const warnings = []
  let linked = 0
  let corrected = 0
  let unmatched = 0
  let renamed = 0
  let fallback = 0

  for (const file of files) {
    let text
    try {
      text = fs.readFileSync(file, "utf8")
    } catch (e) {
      warnings.push("读取失败：" + file)
      continue
    }
    const before = text
    // 先把「frontmatter 结束符前少换行」的坏文件修好——那种文件读不出题面，
    // 不修的话后面所有字段都写不进去。修复必须发生在解析之前。
    text = links.repairFrontmatter(text)
    const fixed = parseProblemMarkdown(text).problem
    if (!fixed || !fixed.id) {
      warnings.push(file + "：frontmatter 解析失败，已跳过")
      continue
    }
    let sourceId = fixed.sourceId || ""
    let url = fixed.url || ""
    let searchUrl = fixed.searchUrl || ""

    if (fixed.source === "leetcode") {
      // 题库 id 里的数字就是源站题号（题库是按真实题号建的，不是导入顺序号）
      const fromId = sourceIdFromId(fixed.id)
      if (sourceId === "") sourceId = fromId
      const number = String(sourceId || fromId || "")

      let slug = index.byId[number] || ""
      // 题号对不上就按标题反查：死脚本里有个别题号抄错了（2833 其实是 2124）。
      // 反查只认「唯一命中」，标题重了宁可不改——错链接比没链接更误导人。
      const fix = links.resolveByTitle(index, number, fixed.title, fixed.difficulty)
      if (fix.corrected && fix.slug) {
        const rightId = Object.keys(index.byId).filter(function (k) {
          return index.byId[k] === fix.slug
        })[0]
        sourceId = rightId
        slug = fix.slug
        corrected += 1
        warnings.push(file + "：" + fix.reason + "，已按标题纠正")
      } else if (slug === "") {
        unmatched += 1
        warnings.push(file + "：题号 " + number + " 在 LeetCode 上查不到，也无法按标题反查")
      } else {
        // 题号对得上但官方译名和题库标题不一致——多半是官方改过名字，记一笔不改
        const official = index.titles[slug] || ""
        if (official && links.normalizeTitle(official) !== links.normalizeTitle(fixed.title)) {
          renamed += 1
        }
      }
      if (slug !== "") url = links.leetcodeUrl(slug)
    } else if (fixed.source === "nowcoder") {
      // 牛客：题目直达地址拿不到，只能退到搜索页
      const fromId = sourceIdFromId(fixed.id)
      if (sourceId === "") sourceId = fromId
      url = ""
      searchUrl = links.nowcoderSearchUrl(fixed.title)
      fallback += 1
    }

    text = links.setFrontmatterField(text, "source_id", sourceId)
    text = links.setFrontmatterField(text, "url", url)
    text = links.setFrontmatterField(text, "search_url", searchUrl)
    // frontmatter 字段顺序统一一下：同样的字段、同样的次序，diff 才干净
    text = links.normalizeFrontmatterOrder(text)

    // 写回前的最后一道闸：改完之后必须还读得出题面，而且题面一个字都不能少。
    // 之前就是因为少写了一个换行，整份题库的 frontmatter 都再也匹配不上，
    // 题面、用例、解法全被当成「没有 frontmatter」而读不出来。
    const after = parseProblemMarkdown(text).problem
    if (!after || !after.id) {
      warnings.push(file + "：改写后解析失败，已放弃本次改动")
      continue
    }
    if (after.statement.length < fixed.statement.length) {
      warnings.push(file + "：改写后题面变短了，已放弃本次改动")
      continue
    }

    if (text !== before) {
      if (!options.dryRun) fs.writeFileSync(file, text, "utf8")
      updated.push(file)
    }
    if (url) linked += 1
  }

  return {
    ok: true,
    learningRoot: learning.root,
    total: files.length,
    updated: updated,
    linked: linked,
    corrected: corrected,
    unmatched: unmatched,
    renamed: renamed,
    fallback: fallback,
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
  const refresh = argv.indexOf("--refresh") !== -1
  const result = await run({ dryRun: dryRun, refresh: refresh })

  if (!result.ok) {
    console.error("同步失败：" + result.reason)
    process.exit(1)
  }

  console.log(
    (dryRun ? "（试运行）" : "") +
      "题库 " +
      result.total +
      " 道，改动 " +
      result.updated.length +
      " 个文件"
  )
  console.log(
    "有原题链接 " + result.linked + " 道，题号纠正 " + result.corrected + " 道，查不到 " + result.unmatched + " 道"
  )
  if (result.renamed > 0) {
    console.log("官方译名与题库标题不一致 " + result.renamed + " 道（只记不改，链接仍按题号走）")
  }
  console.log("牛客走搜索页兜底 " + result.fallback + " 道（牛客不公开题目直达地址）")
  if (result.warnings.length > 0) {
    console.log("告警 " + result.warnings.length + " 条：")
    result.warnings.slice(0, 20).forEach(function (line) {
      console.log("  ! " + line)
    })
    if (result.warnings.length > 20) {
      console.log("  （其余 " + (result.warnings.length - 20) + " 条省略）")
    }
  }
  console.log("提示：同步器不碰 learning 仓的 git，是否提交由你自己决定。")
}

if (require.main === module) {
  main().catch(function (e) {
    console.error("同步失败：" + (e && e.message ? e.message : e))
    process.exit(1)
  })
}

module.exports = {
  run: run,
  main: main,
  loadIndex: loadIndex,
  fetchProblemset: fetchProblemset,
  fetchCnTitles: fetchCnTitles,
  buildTitleIndex: buildTitleIndex,
  sourceIdFromId: sourceIdFromId,
  CACHE_FILE: CACHE_FILE,
  CACHE_TTL_MS: CACHE_TTL_MS,
  GRAPHQL_BATCH: GRAPHQL_BATCH,
}
