"use strict"
/**
 * loader-b.js —— grokking 卡目录 → Unit
 *
 * 读 scaffold/grokking/<card>/ 目录，产出 Unit。契约 §3.2 冻结了行为。
 *
 * 铁律：**只读**。断言脚本写在**独立文件**里，main.py 原文一个字都不动——
 * 教学资产零污染。卡片的 status 写回由 P4 的 verify 通过 loader C 的单元文件完成。
 */

const fs = require("node:fs")
const path = require("node:path")

const { createUnit } = require("./unit")
// 轨道 → id 前缀的映射只有 core/track/tracks.js 一份（G41）：这里引用，不另抄。
const { trackOf } = require("../track/tracks.js")

/** 两段正文之间的分隔线。 */
const SECTION_BREAK = "\n\n---\n\n"

/**
 * 从 README 全文里抠出第一行的 `# 标题`；抠不到就用 stem 兜底。
 *
 * @param {string} readme
 * @param {string} stem
 * @returns {string}
 */
function titleOf(readme, stem) {
  const lines = String(readme || "").split(/\r?\n/)
  for (let i = 0; i < lines.length; i += 1) {
    const m = /^#\s+(.*)$/.exec(lines[i].trim())
    if (m) {
      const title = m[1].trim()
      if (title !== "") return title
    }
  }
  return stem
}

/**
 * 读一张卡目录，产出 Unit。
 *
 * @param {string} dirAbs 卡目录的绝对路径
 * @param {object} opts { learningRoot?: string, track: "database"|"ai", unitsDir?: string|null }
 * @returns {{unit: object|null, warnings: string[]}}
 */
function parseCardDirectory(dirAbs, opts) {
  const options = opts || {}
  const track = options.track === "ai" ? "ai" : "database"
  const warnings = []
  const stem = path.basename(String(dirAbs || "").trim())
  // 前缀从 tracks.js 取（G41），不在这里手抄一份 "db:" / "ai:"。
  const t = trackOf(track)
  const prefix = t ? t.idPrefix + ":" : ""

  const readmePath = path.join(dirAbs, "README.md")
  if (!fs.existsSync(readmePath)) {
    warnings.push("卡目录没有 README.md，跳过：" + stem)
    return { unit: null, warnings: warnings }
  }
  const readme = fs.readFileSync(readmePath, "utf8")

  const notesPath = path.join(dirAbs, "NOTES.md")
  const notes = fs.existsSync(notesPath) ? fs.readFileSync(notesPath, "utf8") : ""

  // 两段都读不到内容 → 这张卡没法讲，跳过
  if (readme.trim() === "" && notes.trim() === "") {
    warnings.push("卡目录既没有 README 内容也没有 NOTES 内容，跳过：" + stem)
    return { unit: null, warnings: warnings }
  }

  const parts = []
  if (readme.trim() !== "") parts.push(readme)
  if (notes.trim() !== "") parts.push(notes)
  const statement = parts.join(SECTION_BREAK)

  const demoPath = path.join(dirAbs, "main.py")
  const hasDemo = fs.existsSync(demoPath)

  // 断言脚本的期望落点。恒填——「脚本该放哪」是卡片的属性；
  // 「脚本在不在」由 P2 的判题器判断（缺失 → compile_error）。
  const scriptRel = "units/" + track + "/" + stem + ".assert.py"
  if (options.unitsDir) {
    const scriptAbs = path.join(options.unitsDir, track, stem + ".assert.py")
    if (!fs.existsSync(scriptAbs)) {
      warnings.push("断言脚本还没写：" + scriptRel + "（P2 判题时报 compile_error）")
    }
  }

  const unit = createUnit({
    id: prefix + stem,
    track: track,
    kind: "card",
    title: titleOf(readme, stem),
    statement: statement,
    entry: {
      demo: hasDemo ? "scaffold/grokking/" + stem + "/main.py" : "",
      reference: "",
      graded: "script",
      script: scriptRel,
      language: "python",
    },
    asserts: [],
    topics: [],
    difficulty: "medium",
    source: "grokking",
    status: hasDemo ? "verified" : "draft",
    progress: "todo",
    reviewed: false,
    reviewed_at: "",
    subtracks: [],
    path: "scaffold/grokking/" + stem + "/",
  })

  return { unit: unit, warnings: warnings }
}

module.exports = {
  SECTION_BREAK: SECTION_BREAK,
  titleOf: titleOf,
  parseCardDirectory: parseCardDirectory,
}
