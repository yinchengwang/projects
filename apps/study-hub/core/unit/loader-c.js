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

/** frontmatter 的围栏行：整行只有三个减号（允许尾随空白）。 */
const FENCE = /^---[ \t]*$/

/**
 * 诊断「为什么没解析到 frontmatter」——给出能看出**真实原因**的分类。
 *
 * 主正则 `^---\n` 只在**完美顶格**时才匹配。匹配不上有好几种真实原因，
 * 一律甩一句「缺少 frontmatter」会让人白折腾：P4 的供给流水线会自动写单元文件，
 * 尾随空白、前导空行都会出现，而报错指不出方向就没法修。
 *
 * 几种结论：
 *   - `fence-not-exact`：第一行确实是分隔行，但减号后面有空白 —— 只差这一点格式。
 *   - `no-closing-fence`：第一行是完整的分隔行（`---`），但后面再没有 `---` 收尾。
 *   - `empty-frontmatter`：开头与收尾两行 `---` **紧挨着**（中间一行都没有）。
 *     主正则要求收尾的 `---` 前面有一个换行（即中间至少一行），所以这种也匹配不上；
 *     但它**有**收尾行，不能说成「没有收尾」。
 *   - `not-at-top`：文件里有个**像** frontmatter 的块（`---` + `key: value` + `---`），
 *     但它不在文件最前面。这里**不断言**前面是空行还是别的文字——没证据的事不说。
 *   - `none`：确实找不到像 frontmatter 的块。
 *
 * @param {string} src 已统一成 \n 的文件全文
 * @returns {"none"|"fence-not-exact"|"no-closing-fence"|"empty-frontmatter"|"not-at-top"}
 */
function diagnoseFrontmatter(src) {
  const lines = String(src === undefined || src === null ? "" : src).split("\n")

  // 第一行就是分隔行却没被主正则吃下：只可能是减号后面多了空白。
  if (/^---[ \t]+$/.test(lines[0])) return "fence-not-exact"
  // 第一行是干净的分隔行、主正则却没匹配上：要么后面没有收尾行，
  // 要么收尾行**紧贴**在第一行下面（`---\n---`）——主正则要求收尾的 `---` 前有一个换行，
  // 两行紧挨时那个换行被开头的 `---\n` 吃掉了，所以匹配不上。这两种原因得分开报。
  if (FENCE.test(lines[0])) {
    const hasClose = lines.slice(1).some(function (line) {
      return FENCE.test(line)
    })
    return hasClose ? "empty-frontmatter" : "no-closing-fence"
  }

  // 从第二行起找第一处分隔行，再看它后面有没有配对的收尾分隔行。
  for (let i = 1; i < lines.length; i += 1) {
    if (!FENCE.test(lines[i])) continue
    let close = -1
    for (let j = i + 1; j < lines.length; j += 1) {
      if (FENCE.test(lines[j])) {
        close = j
        break
      }
    }
    if (close === -1) return "none"
    const body = lines.slice(i + 1, close)
    // 内容为空，或至少有一行像 YAML 的 `key: value` —— 才算是「像 frontmatter 的块」。
    if (body.every(function (line) { return line.trim() === "" })) return "not-at-top"
    if (/^[ \t]*[A-Za-z_][A-Za-z0-9_-]*[ \t]*:/m.test(body.join("\n"))) return "not-at-top"
    return "none"
  }
  return "none"
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
  } else {
    // 顶格要求是对的，不放宽；但报错文案要能看出**真实原因**，
    // 不能一律甩「缺少 frontmatter」——P4 的供给流水线会自动写单元文件，
    // 尾随空白与前置内容都会出现，指不出方向就没法修。
    const reason = diagnoseFrontmatter(src)
    if (reason === "fence-not-exact") {
      warnings.push("frontmatter 的分隔行必须正好是三个减号（---），减号后面不能有空白")
    } else if (reason === "no-closing-fence") {
      warnings.push("frontmatter 没有收尾：第一行是 --- 之后，还得再有一行 --- 把 frontmatter 关掉")
    } else if (reason === "empty-frontmatter") {
      warnings.push("frontmatter 内容为空：开头 --- 与收尾 --- 之间至少要有一行（空行也行）")
    } else if (reason === "not-at-top") {
      // 不断言前面是空行还是正文——没证据的事不说，只说「不在最前面」。
      warnings.push("frontmatter 必须顶格：文件第一行就得是 ---，前面不能有任何内容")
    } else {
      warnings.push("缺少 frontmatter")
    }
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
