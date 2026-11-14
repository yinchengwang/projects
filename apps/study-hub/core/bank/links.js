"use strict"
/**
 * links.js —— 原题链接：地址构造、标题归一化、frontmatter 定点补丁
 *
 * 纯函数，无 IO、无网络。
 *
 * 为什么要有「定点补丁」而不是整篇重写题库文件：
 *   题库文件是人审内容，题面、用例、参考解法都可能已经被手改过。
 *   回填一个链接就把整篇覆盖掉，等于拿机器的版本盖掉人的版本——
 *   所以这里只动 frontmatter 里指定的那几行，正文一个字都不碰。
 *
 * 为什么标题要归一化：
 *   LeetCode 官方中文译名这些年改过好几轮（「两数之和 II」现在叫
 *   「两数之和 II - 输入有序数组」，「最大连续1的个数」现在是「最大连续 1 的个数」）。
 *   死脚本里存的是旧译名，直接比字符串会误判成「题号错了」。
 */

/** LeetCode 中文站题目地址前缀 */
const LEETCODE_PROBLEM_BASE = "https://leetcode.cn/problems/"

/** 牛客搜索页地址前缀（牛客的题目直达地址是一段不可推导的 hash，只能退到搜索页） */
const NOWCODER_SEARCH_BASE = "https://www.nowcoder.com/search?query="

/** Codeforces 题目地址前缀 */
const CODEFORCES_PROBLEM_BASE = "https://codeforces.com/problemset/problem/"

/** 难度 → LeetCode 的难度等级（1 易 / 2 中 / 3 难） */
const DIFFICULTY_LEVEL = { easy: 1, medium: 2, hard: 3 }

/** frontmatter 字段的固定顺序（未列出的字段排到后面，保持原有相对次序） */
const FIELD_ORDER = [
  "id",
  "source",
  "source_id",
  "title",
  "difficulty",
  "topics",
  "tracks",
  "status",
  "needs_fill",
  "url",
  "search_url",
  "created",
  "updated",
]

/**
 * 标题归一化：把两套写法拉回同一个形状，好让它们能比。
 *   - 全角/半角空白全去掉（「最大连续 1 的个数」≡「最大连续1的个数」）
 *   - 常见中英文标点去掉
 *   - 罗马数字 Ⅱ 换成 ASCII 的 II（LeetCode 用罗马数字，旧译名用两个 I）
 * @param {string} text
 * @returns {string}
 */
function normalizeTitle(text) {
  const ROMAN = { "Ⅰ": "I", "Ⅱ": "II", "Ⅲ": "III", "Ⅳ": "IV", "Ⅴ": "V", "Ⅵ": "VI", "Ⅶ": "VII", "Ⅷ": "VIII", "Ⅸ": "IX", "Ⅹ": "X" }
  let s = String(text === null || text === undefined ? "" : text)
  s = s.replace(/[ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩ]/g, function (ch) {
    return ROMAN[ch] || ch
  })
  s = s.replace(/[\s　]/g, "")
  s = s.replace(/[，。、；：？！“”‘’（）()\[\]【】《》〈〉·・\-_/\\|,.:;?!#$%^&*+=~·]/g, "")
  return s.toLowerCase()
}

/**
 * LeetCode 题目地址。
 * @param {string} slug 题目的英文 slug（two-sum）
 * @returns {string}
 */
function leetcodeUrl(slug) {
  const s = String(slug === null || slug === undefined ? "" : slug).trim()
  if (s === "") return ""
  return LEETCODE_PROBLEM_BASE + encodeURIComponent(s) + "/"
}

/**
 * Codeforces 题目地址。
 * @param {number|string} contestId
 * @param {string} index 题号下标（A / B / C1）
 * @returns {string}
 */
function codeforcesUrl(contestId, index) {
  const cid = String(contestId === null || contestId === undefined ? "" : contestId).trim()
  const idx = String(index === null || index === undefined ? "" : index).trim()
  if (!/^\d+$/.test(cid) || idx === "") return ""
  return CODEFORCES_PROBLEM_BASE + cid + "/" + encodeURIComponent(idx)
}

/**
 * 牛客搜索页地址。牛客每道题的直达地址是一段 32 位 hash，
 * 没有任何公开规则能从「剑指 Offer 第 N 题」推出来，只能退到搜索页。
 * @param {string} title 题目标题
 * @returns {string}
 */
function nowcoderSearchUrl(title) {
  const s = String(title === null || title === undefined ? "" : title).trim()
  if (s === "") return ""
  return NOWCODER_SEARCH_BASE + encodeURIComponent(s)
}

/**
 * 在 frontmatter 里改一个字段，正文不动。
 *
 * 语义：
 *   value 非空 → 该字段有就替换、没有就按 FIELD_ORDER 插到合适位置
 *   value 为空 → 该字段有就删掉、没有就什么都不做
 *
 * @param {string} text 题库文件全文
 * @param {string} key 字段名（frontmatter 里的写法，如 source_id）
 * @param {string} value 新值；空串表示删除该行
 * @returns {string} 改完的全文；没有 frontmatter 就原样返回
 */
function setFrontmatterField(text, key, value) {
  const src = String(text === null || text === undefined ? "" : text)
  const m = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/.exec(src)
  if (!m) return src
  const eol = src.indexOf("\r\n") !== -1 ? "\r\n" : "\n"
  const lines = m[1].split(/\r?\n/)

  let at = -1
  for (let i = 0; i < lines.length; i++) {
    if (new RegExp("^" + key + "\\s*:").test(lines[i])) {
      at = i
      break
    }
  }

  const val = String(value === null || value === undefined ? "" : value).trim()
  if (val === "") {
    if (at === -1) return src
    lines.splice(at, 1)
  } else {
    const line = key + ": " + val
    if (at !== -1) {
      lines[at] = line
    } else {
      // 按 FIELD_ORDER 插到第一个「排在 key 后面」的已知字段之前
      const mine = FIELD_ORDER.indexOf(key)
      let pos = lines.length
      for (let i = 0; i < lines.length; i++) {
        const hit = /^([A-Za-z_][A-Za-z0-9_]*)\s*:/.exec(lines[i])
        if (!hit) continue
        const idx = FIELD_ORDER.indexOf(hit[1])
        if (idx === -1) continue
        if (mine === -1 || idx > mine) {
          pos = i
          break
        }
      }
      lines.splice(pos, 0, line)
    }
  }

  return "---" + eol + lines.join(eol) + eol + "---" + eol + src.slice(m[0].length)
}

/**
 * 把 frontmatter 的已知字段按 FIELD_ORDER 重排，未知字段排到后面。
 * 只动 frontmatter 的行序，正文一个字不改。
 * @param {string} text
 * @returns {string}
 */
function normalizeFrontmatterOrder(text) {
  const src = String(text === null || text === undefined ? "" : text)
  const m = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/.exec(src)
  if (!m) return src
  const eol = src.indexOf("\r\n") !== -1 ? "\r\n" : "\n"
  const lines = m[1].split(/\r?\n/)

  const known = []
  const unknown = []
  lines.forEach(function (line) {
    const hit = /^([A-Za-z_][A-Za-z0-9_]*)\s*:/.exec(line)
    if (!hit || FIELD_ORDER.indexOf(hit[1]) === -1) {
      unknown.push(line)
      return
    }
    known.push(line)
  })
  known.sort(function (a, b) {
    const ka = /^([A-Za-z_][A-Za-z0-9_]*)/.exec(a)[1]
    const kb = /^([A-Za-z_][A-Za-z0-9_]*)/.exec(b)[1]
    return FIELD_ORDER.indexOf(ka) - FIELD_ORDER.indexOf(kb)
  })

  const merged = known.concat(unknown)
  if (merged.join("\n") === lines.join("\n")) return src
  return "---" + eol + merged.join(eol) + eol + "---" + eol + src.slice(m[0].length)
}

/**
 * 修复「frontmatter 结束符前少了换行」的题库文件。
 *
 * 这种坏文件的形态是：
 *   ---
 *   id: lc-00001
 *   ...
 *   updated: 2026-11-11---      ← 这里本该是 \n---
 *   ...
 * 一旦这样，parseProblemMarkdown 的 /^---\n([\s\S]*?)\n---/ 就再也匹配不上，
 * 整道题（题面、用例、解法）会被当成「没有 frontmatter」而读不出来。
 *
 * 判据很窄：文件开头是 ---，且从第 3 个字符起找不到任何一个「换行 + ---」，
 * 说明 frontmatter 的结束符整份文件里只有一个候选 —— 那就是它，补个换行即可。
 * 正文里就算有 --- 也不受影响，因为好文件本来就匹配得到，不会进这个分支。
 *
 * @param {string} text
 * @returns {string}
 */
function repairFrontmatter(text) {
  const src = String(text === null || text === undefined ? "" : text)
  if (src.slice(0, 3) !== "---") return src
  if (src.indexOf("\n---", 3) !== -1) return src
  const at = src.indexOf("---", 3)
  if (at === -1) return src
  return src.slice(0, at) + "\n" + src.slice(at)
}

/**
 * 从 frontmatter 里读一个字段（不做整体解析，只拿这一行）。
 * @param {string} text
 * @param {string} key
 * @returns {string}
 */
function readFrontmatterField(text, key) {
  const src = String(text === null || text === undefined ? "" : text)
  const m = /^---\r?\n([\s\S]*?)\r?\n---/.exec(src)
  if (!m) return ""
  const re = new RegExp("^" + key + "\\s*:\\s*(.*)$", "m")
  const hit = re.exec(m[1])
  if (!hit) return ""
  return String(hit[1]).trim()
}

/**
 * 用「中文标题 → slug」反查表纠正题号。
 *
 * 只在「反查结果唯一」时才纠正——标题重了（多道同名题）就宁可不改，
 * 错链接比没链接更误导人。
 *
 * @param {{byId: Object<string,string>, byTitle: Object<string,string[]>, difficulty?: Object<string,number>}} index
 * @param {string} sourceId 题库里记的题号
 * @param {string} title 题库里记的标题
 * @param {string} [wantDifficulty] 题库里记的难度（easy/medium/hard），用来给反查结果加一道保险
 * @returns {{slug: string, corrected: boolean, reason: string}}
 */
function resolveByTitle(index, sourceId, title, wantDifficulty) {
  const byTitle = (index && index.byTitle) || {}
  const byId = (index && index.byId) || {}
  const difficulty = (index && index.difficulty) || {}
  const norm = normalizeTitle(title)
  if (norm === "") return { slug: "", corrected: false, reason: "标题为空" }
  const cands = byTitle[norm] || []
  if (cands.length === 0) return { slug: "", corrected: false, reason: "标题在索引里查不到" }
  if (cands.length > 1) return { slug: "", corrected: false, reason: "标题重名，无法确定" }
  const slug = cands[0]
  const current = byId[String(sourceId)] || ""
  if (slug === current) return { slug: slug, corrected: false, reason: "题号与标题一致" }

  // 反查到了别的题：难度对不上就别改，宁可留个可疑链接也不给个确定的错链接
  const want = DIFFICULTY_LEVEL[String(wantDifficulty || "").toLowerCase()]
  const got = difficulty[slug]
  if (want && got && want !== got) {
    return {
      slug: "",
      corrected: false,
      reason: "标题反查到 " + slug + "，但难度对不上，不改",
    }
  }

  const rightId = Object.keys(byId).filter(function (k) {
    return byId[k] === slug
  })[0]
  return {
    slug: slug,
    corrected: true,
    reason: "题号 " + sourceId + " 与标题不符，实际是 " + rightId,
  }
}

module.exports = {
  LEETCODE_PROBLEM_BASE: LEETCODE_PROBLEM_BASE,
  NOWCODER_SEARCH_BASE: NOWCODER_SEARCH_BASE,
  CODEFORCES_PROBLEM_BASE: CODEFORCES_PROBLEM_BASE,
  DIFFICULTY_LEVEL: DIFFICULTY_LEVEL,
  FIELD_ORDER: FIELD_ORDER,
  normalizeTitle: normalizeTitle,
  leetcodeUrl: leetcodeUrl,
  codeforcesUrl: codeforcesUrl,
  nowcoderSearchUrl: nowcoderSearchUrl,
  setFrontmatterField: setFrontmatterField,
  normalizeFrontmatterOrder: normalizeFrontmatterOrder,
  repairFrontmatter: repairFrontmatter,
  readFrontmatterField: readFrontmatterField,
  resolveByTitle: resolveByTitle,
}
