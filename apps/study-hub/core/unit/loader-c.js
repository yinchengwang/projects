"use strict"
/**
 * loader-c.js —— 统一单元文件 → Unit
 *
 * 读 learning/units/<track>/*.md，产出 Unit。契约 §3.3 冻结了行为与校验文案。
 * 这是**新内容的唯一落点**：P4 生成的单元文件、P5 生成的算法题都落这里。
 */

const path = require("node:path")

const { parseFrontmatter, splitSections, parseCases } = require("../bank/parse")
// Unit 的枚举只有 core/unit/unit.js 一份（G41）：这里引用，不另抄。
const {
  createUnit,
  ID_PATTERN,
  toBool,
  KINDS,
  GRADED,
  STATUSES,
  DIFFICULTIES,
  PROGRESSES,
  SOURCES,
} = require("./unit")
// 轨道 → id 前缀的映射只有 core/track/tracks.js 一份（G41）：这里引用，不另抄。
const { trackOf } = require("../track/tracks.js")

/** track → units/ 下的目录名。 */
const TRACK_DIR = { algorithm: "algorithm", database: "database", ai: "ai" }

/**
 * 取 frontmatter 里的字符串值。
 * @param {object} fm
 * @param {string} key
 * @returns {string}
 */
function str(fm, key) {
  const value = fm[key]
  return value === undefined || value === null ? "" : String(value).trim()
}

/**
 * 判断文件里有没有**不在开头**的 frontmatter 块（`---` 行 + 内容 + `---` 行）。
 *
 * 只认文件里**第一处** `---` 行——frontmatter 如果存在，必然是文件的第一块。
 * 这样就不会把正文里的分隔线（Markdown 的水平线 `---`）误判成 frontmatter。
 *
 * @param {string} src 已统一成 \n 的文件全文
 * @returns {boolean}
 */
function hasFrontmatterLater(src) {
  const text = String(src === undefined || src === null ? "" : src)
  // 文件里第一处 `---` 行
  const first = /(^|\n)---[ \t]*\n/.exec(text)
  if (!first) return false
  // 这一处 `---` 在不在文件开头？（first[1] 是它前面吃掉的那个换行）
  const fenceStart = first.index + first[1].length
  if (fenceStart === 0) return false
  // 还得有收尾的 `---` 行，才算一个完整的 frontmatter 块
  const after = text.slice(first.index + first[0].length)
  const close = /\n---[ \t]*(?:\n|$)/.exec(after)
  if (!close) return false
  const content = after.slice(0, close.index)
  // 内容要么是空的，要么像 YAML（至少有一行 key: value）
  if (content.trim() === "") return true
  return /^[ \t]*[A-Za-z_][A-Za-z0-9_-]*[ \t]*:/m.test(content)
}

/**
 * 解析一份单元文件，产出 Unit。
 *
 * @param {string} text 文件全文
 * @param {object} opts { path: string } —— 相对 units/ 根的路径，如 "database/mvcc.md"（必填，G42）
 * @returns {{unit: object|null, warnings: string[]}}
 */
function parseUnitFile(text, opts) {
  const options = opts || {}
  const warnings = []
  const relPath = String(options.path === undefined || options.path === null ? "" : options.path)

  // G42：opts.path 必填（契约 §3.3）。缺了就只给一条警告，不产出单元——
  // 否则 path 会退化成 "units/"：非空，却指向目录，下游永远找不到文件。
  if (relPath.trim() === "") {
    warnings.push("单元文件缺少 path，无法产出单元")
    return { unit: null, warnings: warnings }
  }

  const src = String(text || "").replace(/\r\n/g, "\n")
  let body = src
  let fm = {}
  const match = /^---\n([\s\S]*?)\n---\n?/.exec(src)
  if (match) {
    fm = parseFrontmatter(match[1])
    body = src.slice(match[0].length)
  } else if (src.charAt(0) === "\uFEFF") {
    // 文件带 BOM：frontmatter 明明在，但正则锚在字符串开头，BOM 把它顶掉了。
    warnings.push("单元文件带 BOM，请去掉 BOM 再试")
  } else if (hasFrontmatterLater(src)) {
    // frontmatter 存在但不在文件开头（前面有空行 / 别的内容）。
    // 顶格要求是对的，不放宽；但报错文案要能看出真实原因——
    // P4 的供给流水线会自动写单元文件，BOM / 前导空行出现概率不低。
    warnings.push("frontmatter 必须顶格，前面不能有空行")
  } else {
    warnings.push("缺少 frontmatter")
  }

  const id = str(fm, "id")
  if (!ID_PATTERN.test(id)) {
    warnings.push("单元 id 非法：" + id)
    return { unit: null, warnings: warnings }
  }

  const track = str(fm, "track")
  const t = trackOf(track)
  if (!t || t.idPrefix !== id.split(":")[0]) {
    warnings.push("单元 id 前缀与 track 不一致：" + id + " / " + track)
    return { unit: null, warnings: warnings }
  }

  const kind = str(fm, "kind")
  if (KINDS.indexOf(kind) === -1) {
    warnings.push("单元 " + id + " 的 kind 非法：" + kind)
    return { unit: null, warnings: warnings }
  }

  const graded = str(fm, "graded")
  if (GRADED.indexOf(graded) === -1) {
    warnings.push("单元 " + id + " 的 graded 非法：" + graded)
    return { unit: null, warnings: warnings }
  }

  const status = str(fm, "status")
  if (STATUSES.indexOf(status) === -1) {
    warnings.push("单元 " + id + " 的 status 非法：" + status)
    return { unit: null, warnings: warnings }
  }

  const script = str(fm, "script")
  if (graded === "script" && script === "") {
    warnings.push("单元 " + id + " 声明 graded: script 但没写 script 路径")
    return { unit: null, warnings: warnings }
  }

  // 目录与 track 必须对得上（units/<track>/）
  const dirName = path.basename(path.dirname(relPath))
  if (dirName !== "" && dirName !== "." && TRACK_DIR[track] !== dirName) {
    warnings.push("单元 " + id + " 放在 units/" + dirName + "/ 下，与 track 不符")
    return { unit: null, warnings: warnings }
  }

  const sections = splitSections(body)
  // DB/AI 卡的小节叫「讲解」，算法题的小节叫「题面」——两个都认。
  const statement = String(sections["讲解"] || sections["题面"] || "").trim()
  const related = String(sections["关联"] || "").trim()

  // 算法轨单元的「测试用例」表格转成 output 断言（与 loader A 同构）
  const asserts = []
  parseCases(sections["测试用例"] || "").forEach(function (item, index) {
    // tol 在 v1 一律丢弃并留一句中文警告（与 loader A 同文案，契约 §3.1.1）。
    if (item.tol !== null && item.tol !== undefined) {
      warnings.push("用例 " + (index + 1) + " 带 tol 参数，v1 按精确比对处理")
    }
    asserts.push({
      type: "output",
      input: item.input === undefined || item.input === null ? "" : String(item.input),
      expect: item.expect === undefined || item.expect === null ? "" : String(item.expect),
      cmp: "exact",
    })
  })

  const difficulty = str(fm, "difficulty")
  const source = str(fm, "source")
  const progress = str(fm, "progress")

  let unit = null
  try {
    unit = createUnit({
      id: id,
      track: track,
      kind: kind,
      title: str(fm, "title"),
      statement: statement,
      entry: {
        demo: str(fm, "demo"),
        reference: str(fm, "reference"),
        graded: graded,
        script: graded === "script" ? script : "",
        // 算法轨的入口语言恒为 cpp；别的轨照 frontmatter 写。
        language: track === "algorithm" ? "cpp" : str(fm, "language"),
      },
      asserts: asserts,
      topics: fm.topics || [],
      difficulty: DIFFICULTIES.indexOf(difficulty) !== -1 ? difficulty : "medium",
      source: SOURCES.indexOf(source) !== -1 ? source : "generated",
      status: status,
      progress: PROGRESSES.indexOf(progress) !== -1 ? progress : "todo",
      reviewed: toBool(fm.reviewed),
      reviewed_at: str(fm, "reviewed_at"),
      subtracks: fm.subtracks || [],
      // 契约 §1.2：Unit.path 一律相对**内容根**（learning 仓根）。
      // loader A = bank/…、loader B = scaffold/grokking/…、loader C = units/<track>/<stem>.md —— 三者口径一致。
      path: "units/" + relPath,
      related: related,
    })
  } catch (err) {
    warnings.push(String(err && err.message ? err.message : err))
    return { unit: null, warnings: warnings }
  }

  return { unit: unit, warnings: warnings }
}

module.exports = {
  // 轨道 → id 前缀的映射不在这儿导出：只有 core/track/tracks.js 一份（G41）。
  TRACK_DIR: TRACK_DIR,
  parseUnitFile: parseUnitFile,
}
