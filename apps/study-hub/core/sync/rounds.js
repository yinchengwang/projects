"use strict"
/**
 * rounds.js —— 轮次解析 / 勾选写回 / 错题订正 / 工作区查脏
 *
 * learning/progress 下的 9 个轮次文件格式极不统一（勘察结论）：
 *   - 一个 frontmatter 都没有，都以 "# R<NN> <主题> — 学习进度" 开头
 *     （r30 是「进度追踪」，r32 是「审计进度日志」）
 *   - 清单表的列名逐个文件不同，实测 7 种表头
 *   - 完成标记是字符串 done，未知值统一是 TBD-deferred
 * 所以解析一律按「列名关键词」匹配，不按列序、不按固定列数（G21）。
 * 解析不了的行跳过并计入告警，绝不整体报错。
 *
 * 写回 learning 仓的三条硬规则（设计文档 §4.4）：
 *   1. 防并发：写前查 `git status --porcelain -z`，工作区脏就拒绝写回，
 *      并明确指出哪个文件被动过。
 *   2. 只改目标行：读-改-写，目标行只替换目标单元格，其余内容逐字节不变。
 *   3. 绝不自动 commit：写完只把 diff 返回给用户，提交是用户的决定（G25）。
 *
 * 「脏」的判定有一条例外，见 SELF_WRITTEN 的说明：app 自己上一轮写下的
 * 未提交改动不算「被别人动过」，否则用户连着勾两格就会被自己的改动挡住。
 */

const fs = require("node:fs")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

/**
 * 列角色关键词表。
 * 第一轮按这里的关键词强匹配（同一列只能被一个角色占用，先到先得）；
 * 强 id 优先于 title，「卡片id」这种强写法优先于裸「id」。
 */
const ROLE_KEYWORDS = [
  { role: "id", keywords: ["卡片id", "卡id", "题目id", "题id", "题号"] },
  { role: "title", keywords: ["名称", "标题", "主题", "题目"] },
  { role: "status", keywords: ["状态"] },
  { role: "doneDate", keywords: ["完成时间", "完成日期", "完成日"] },
  { role: "commitSha", keywords: ["commitsha", "commit", "sha"] },
  { role: "language", keywords: ["语言"] },
  { role: "order", keywords: ["序号", "编号"] },
]

/**
 * git status --porcelain -z 的一条记录：两字符状态码 + 一个空格 + 路径。
 * 用正则而不是 slice(3)：重命名记录的状态码是 "R "（第二个字符是空格），
 * 硬切会把路径前的一个空格也切下来；正则按「状态码(2) + 分隔空格(1)」取，
 * 路径本身带的空格与中文都不受影响（G20）。
 */
const PORCELAIN_RECORD = /^([ MADRCU?!])([ MADRCU?!]) ([\s\S]*)$/

/* ==================== 解析 ==================== */

/**
 * 表头归一化：转小写、去空白、去星号。
 * 「卡片 ID」→ 「卡片id」，「commit SHA」→ 「commitsha」。
 */
function normalizeHeader(text) {
  return String(text === null || text === undefined ? "" : text)
    .toLowerCase()
    .replace(/\s+/g, "")
    .replace(/[*`]/g, "")
    .trim()
}

/**
 * 拆一行 markdown 表格：去掉首尾的竖线，逐格 trim。
 * @param {string} line
 * @returns {string[]}
 */
function splitRow(line) {
  const trimmed = String(line || "").trim()
  let body = trimmed
  // 左边有 "|" 就切掉，右边有 "|" 也切掉；没有就原样（别把第一列切没了）
  if (body.charAt(0) === "|") body = body.slice(1)
  if (body.charAt(body.length - 1) === "|") body = body.slice(0, -1)
  return body.split("|").map(function (cell) {
    return cell.trim()
  })
}

/**
 * 是不是表格分隔行（|---|---|）。
 */
function isSeparatorRow(cells) {
  if (!cells || cells.length === 0) return false
  return cells.every(function (cell) {
    return cell === "" || /^:?-{2,}:?$/.test(cell)
  })
}

/**
 * 找出文件里所有连续的表格块。
 * @param {string[]} lines
 * @returns {Array<{start:number, rows:Array<{index:number,line:string,cells:string[]}>}>}
 */
function findTableBlocks(lines) {
  const blocks = []
  let current = null
  lines.forEach(function (line, index) {
    const trimmed = String(line).trim()
    if (trimmed.charAt(0) === "|") {
      if (!current) current = { start: index, rows: [] }
      current.rows.push({ index: index, line: line, cells: splitRow(line) })
      return
    }
    if (current) {
      blocks.push(current)
      current = null
    }
  })
  if (current) blocks.push(current)
  return blocks
}

/**
 * 取某一列的值；列不存在或越界一律返回空串。
 */
function cellAt(cells, index) {
  if (index === undefined || index === null) return ""
  if (!cells || index < 0 || index >= cells.length) return ""
  return String(cells[index]).trim()
}

/**
 * 某一列的所有数据值是不是纯数字。
 */
function allNumeric(rows, index) {
  if (!rows || rows.length === 0) return false
  let seen = 0
  for (const row of rows) {
    const value = cellAt(row.cells, index)
    if (value === "") continue
    seen += 1
    if (!/^\d+$/.test(value)) return false
  }
  return seen > 0
}

/**
 * 按列名关键词把表头映射到角色。
 * @param {string[]} headerCells 表头格
 * @param {Array<{cells:string[]}>} dataRows 数据行（用于判断「纯数字的 ID 列其实是序号」）
 * @returns {Object<string, number>} 角色 → 列下标
 */
function matchColumns(headerCells, dataRows) {
  const norms = headerCells.map(normalizeHeader)
  const assigned = {}
  const taken = {}

  function claim(role, index) {
    if (taken[index]) return false
    assigned[role] = index
    taken[index] = true
    return true
  }

  // 第一轮：关键词强匹配
  ROLE_KEYWORDS.forEach(function (spec) {
    for (let i = 0; i < norms.length; i += 1) {
      if (taken[i]) continue
      const matched = spec.keywords.some(function (kw) {
        return norms[i].indexOf(kw) !== -1
      })
      if (matched) {
        claim(spec.role, i)
        return
      }
    }
  })

  // 第二轮：弱 id 匹配（裸「id」列、「卡片」列）
  const idCandidates = []
  for (let i = 0; i < norms.length; i += 1) {
    if (taken[i]) continue
    const norm = norms[i]
    if (norm.indexOf("id") !== -1 || norm === "卡片" || norm === "卡") {
      idCandidates.push(i)
    }
  }
  let numericIdColumn = -1
  if (idCandidates.length > 0) {
    let picked = -1
    for (const i of idCandidates) {
      if (allNumeric(dataRows, i)) {
        if (numericIdColumn === -1) numericIdColumn = i
        continue
      }
      picked = i
      break
    }
    if (picked === -1) picked = idCandidates[0]
    claim("id", picked)
  }

  // 第三轮：序号列（「#」或含「序」）
  if (assigned.order === undefined) {
    for (let i = 0; i < norms.length; i += 1) {
      if (taken[i]) continue
      const norm = norms[i]
      if (norm === "#" || norm.indexOf("序") !== -1) {
        claim("order", i)
        break
      }
    }
  }

  // 第四轮：全数字的 id 候选列改判成序号（r25 的「ID | 卡片」就是这种情况）
  if (assigned.order === undefined && numericIdColumn !== -1) {
    claim("order", numericIdColumn)
  }

  return assigned
}

/**
 * 解析「基本信息」：两列表格 + 无序列表两种写法都认。
 * 只看清单表之前的行，避免把清单后面的小节误当成基本信息。
 * @param {string[]} lines
 * @param {number} limit 清单表起始行（不含）
 * @returns {{goal: string, status: string, startedAt: string}}
 */
function parseBasicInfo(lines, limit) {
  const info = { goal: "", status: "", startedAt: "" }
  const end = limit === undefined ? lines.length : Math.min(limit, lines.length)

  function apply(key, value) {
    const k = String(key || "").replace(/[*`]/g, "").trim()
    const v = String(value || "").trim()
    if (k === "" || v === "") return
    if (info.goal === "" && (k.indexOf("目标") !== -1 || k.indexOf("主题") !== -1)) {
      info.goal = v
      return
    }
    if (info.status === "" && k.indexOf("状态") !== -1) {
      info.status = v
      return
    }
    if (info.startedAt === "" && k.indexOf("开始") !== -1) {
      info.startedAt = /^\d{4}-\d{2}-\d{2}$/.test(v) ? v : ""
    }
  }

  // 两列表格：| 字段 | 值 |
  findTableBlocks(lines.slice(0, end)).forEach(function (block) {
    if (block.rows[0].cells.length !== 2) return
    block.rows.slice(1).forEach(function (row) {
      if (isSeparatorRow(row.cells)) return
      if (row.cells.length < 2) return
      apply(row.cells[0], row.cells[1])
    })
  })

  // 无序列表：- **开始日期**: 2026-07-12
  for (let i = 0; i < end; i += 1) {
    const m = /^[-*]\s+(.+?)\s*[:：]\s*(.*)$/.exec(String(lines[i]).trim())
    if (!m) continue
    apply(m[1], m[2])
  }

  return info
}

/**
 * 轮次状态：基本信息里有「状态」就用它；否则看条目是否全部完成。
 */
function deriveRoundStatus(infoStatus, items) {
  if (infoStatus) {
    const value = String(infoStatus).trim().toLowerCase()
    if (value === "done" || value === "已完成" || value === "完成") return "done"
    return "active"
  }
  if (items.length > 0 && items.every(function (item) { return item.status === "done" })) {
    return "done"
  }
  return "active"
}

/**
 * 把一行数据组装成 item；解析不了返回 null。
 */
function buildItem(row, columns) {
  const cells = row.cells
  if (!cells || cells.length < 2) return null
  const id = cellAt(cells, columns.id)
  if (id === "") return null
  const statusRaw = cellAt(cells, columns.status).toLowerCase()
  return {
    raw: String(row.line).trim(),
    id: id,
    title: cellAt(cells, columns.title),
    status: statusRaw === "done" ? "done" : "todo",
    doneDate: cellAt(cells, columns.doneDate),
    commitSha: cellAt(cells, columns.commitSha),
  }
}

/**
 * 找出清单表。
 *
 * 判定顺序：
 *   1. 先找「表头能映射出 id 角色」的表——这是最可靠的特征
 *      （| 卡片 ID | 名称 |、| 序号 | ID | 名称 | 状态 | 都属于这一类）
 *   2. 找不到再退而求其次：第一个 3 列以上的表
 *   3. 两列的表也要能认（| 卡片 ID | 名称 | 这种缺列的清单表），
 *      但 | 字段 | 值 | 这种基本信息表不能认——它没有 id 角色
 *
 * @param {string[]} lines
 * @returns {{block:object, header:string[], dataRows:object[], columns:object}|null}
 */
function findItemBlock(lines) {
  const candidates = []
  findTableBlocks(lines).forEach(function (block) {
    if (block.rows.length < 2) return
    const dataRows = block.rows.slice(1).filter(function (row) {
      return !isSeparatorRow(row.cells)
    })
    if (dataRows.length === 0) return
    const header = block.rows[0].cells
    candidates.push({
      block: block,
      header: header,
      dataRows: dataRows,
      columns: matchColumns(header, dataRows),
    })
  })

  // 先认「有 id 角色」的表
  for (const candidate of candidates) {
    if (candidate.columns.id !== undefined) return candidate
  }
  // 再认 3 列以上的表
  for (const candidate of candidates) {
    if (candidate.header.length >= 3) return candidate
  }
  return null
}

/**
 * 切分行：按 \r\n 或 \n 切，返回 [{text, eol}]。
 * 每行自己的行尾符原样留着，写回时逐字节还原（learning 仓的文件是 CRLF）。
 * @param {string} text
 * @returns {Array<{text:string, eol:string}>}
 */
function splitLines(text) {
  const src = String(text === null || text === undefined ? "" : text)
  const lines = []
  const re = /\r\n|\n/g
  let start = 0
  let m
  while ((m = re.exec(src)) !== null) {
    lines.push({ text: src.slice(start, m.index), eol: m[0] })
    start = m.index + m[0].length
  }
  if (start < src.length) lines.push({ text: src.slice(start), eol: "" })
  return lines
}

/**
 * 解析一篇轮次文件。
 * @param {string} file 绝对路径
 * @param {string} text 文件内容
 * @returns {{round: object|null, warnings: string[]}}
 */
function parseRoundText(file, text) {
  const warnings = []
  const lines = splitLines(text).map(function (line) {
    return line.text
  })

  let title = ""
  for (const line of lines) {
    const trimmed = line.trim()
    if (trimmed.charAt(0) === "#") {
      title = trimmed.replace(/^#+\s*/, "").trim()
      break
    }
  }

  const matched = path.basename(file).match(/^(r\d+)/i)
  const id = matched ? matched[1].toLowerCase() : ""

  const found = findItemBlock(lines)

  const items = []
  if (!found) {
    warnings.push("没找到清单表（表头里认不出 ID 列）")
  } else {
    found.dataRows.forEach(function (row) {
      const item = buildItem(row, found.columns)
      if (item === null) {
        warnings.push("第 " + (row.index + 1) + " 行解析不了（列数不对或没有 ID），已跳过")
        return
      }
      items.push(item)
    })
  }

  const info = parseBasicInfo(lines, found ? found.block.start : lines.length)

  const round = {
    id: id,
    file: file,
    title: title,
    goal: info.goal,
    status: deriveRoundStatus(info.status, items),
    items: items,
    startedAt: info.startedAt,
  }
  return { round: round, warnings: warnings }
}

/**
 * 解析一个轮次目录下的全部轮次文件。
 * @param {string} progressRoot learning/progress 目录
 * @returns {{rounds: object[], warnings: string[]}}
 */
function parseRounds(progressRoot) {
  const rounds = []
  const warnings = []
  if (!progressRoot || !fs.existsSync(progressRoot)) {
    return { rounds: rounds, warnings: ["轮次目录不存在：" + String(progressRoot)] }
  }

  let names = []
  try {
    names = fs
      .readdirSync(progressRoot)
      .filter(function (name) {
        return /^r\d+.*\.md$/i.test(name)
      })
      .sort()
  } catch (e) {
    return { rounds: rounds, warnings: ["读取轮次目录失败：" + e.message] }
  }

  names.forEach(function (name) {
    const file = path.join(progressRoot, name)
    let text = ""
    try {
      text = fs.readFileSync(file, "utf8")
    } catch (e) {
      warnings.push(name + "：读取失败（" + e.message + "）")
      return
    }
    const parsed = parseRoundText(file, text)
    if (parsed.round) rounds.push(parsed.round)
    parsed.warnings.forEach(function (w) {
      warnings.push(name + "：" + w)
    })
  })

  return { rounds: rounds, warnings: warnings }
}

/* ==================== 写回 / 订正 / 查脏 ==================== */

/**
 * track → 笔记 frontmatter 的 stack 枚举。
 * 笔记 schema 的 stack 取值只有 linux/db/c/ds/cpp/python，
 * 题目的 track 只有 interview/fundamentals/advanced/engineering，所以要映射。
 */
const TRACK_STACK = {
  interview: "c",
  fundamentals: "ds",
  advanced: "ds",
  engineering: "db",
}

/**
 * 本进程写过的文件（绝对路径）。
 *
 * app 写回 learning 仓之后不会自动 commit（用户才决定），所以「写完再写第二处」
 * 时工作区一定是脏的——那脏是 app 自己造成的，不是别人动过。
 * 所以这里记下自己写过的路径：查脏时把这些文件从「阻塞项」里剔除，
 * 别人动过的文件仍然一律拒绝写回。
 */
const SELF_WRITTEN = new Set()

/**
 * 检查 git 工作区是否干净。
 *
 * 按 G20：用 `git -c core.quotepath=false status --porcelain -z`，**按 NUL 切分**；
 * 路径用「状态码 + 分隔空格之后」切出来，不做 line.slice(3) 硬切
 * （重命名记录的状态码是 "R "，硬切会多带一个空格；中文路径与带空格的路径也会切错）。
 *
 * 额外加了 `--untracked-files=all`：不加的话 git 会把整个未跟踪目录折叠成
 * 一条「notes/」，报不出「到底哪个文件被动过」（设计文档 §4.4 的硬要求）。
 *
 * @param {string} repoRoot 仓库根目录
 * @returns {{ok: boolean, dirtyFiles: string[], reason: string}}
 */
function assertCleanWorktree(repoRoot) {
  let stdout = ""
  try {
    stdout = execFileSync(
      "git",
      ["-c", "core.quotepath=false", "status", "--porcelain", "-z", "--untracked-files=all"],
      { cwd: repoRoot, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }
    )
  } catch (e) {
    return {
      ok: false,
      dirtyFiles: [],
      reason: "读不到 git 状态：" + (e && e.message ? e.message : String(e)),
    }
  }

  const dirtyFiles = []
  String(stdout).split("\0").forEach(function (record) {
    if (record === "") return
    const matched = PORCELAIN_RECORD.exec(record)
    if (!matched) {
      // 重命名记录里的旧路径（-z 模式下是独立的一段），同样算被动过
      dirtyFiles.push(record)
      return
    }
    dirtyFiles.push(matched[3])
  })

  return { ok: dirtyFiles.length === 0, dirtyFiles: dirtyFiles, reason: "" }
}

/**
 * 脏工作区错误（409）。
 */
function dirtyError(dirtyFiles) {
  const err = new Error(
    "learning 仓工作区不干净，拒绝写回。被动过的文件：" +
      dirtyFiles.join("、") +
      "（如果这些改动是 app 之前写下的，先提交或撤销再试）"
  )
  err.status = 409
  err.dirtyFiles = dirtyFiles
  return err
}

/**
 * 找不到东西的错误（404）。
 */
function notFoundError(message) {
  const err = new Error(message)
  err.status = 404
  return err
}

/**
 * 写回前的查脏：别人动过的文件一律拒绝；app 自己写过的（未提交）不算。
 * @param {string} repoRoot
 * @returns {{ok: boolean, dirtyFiles: string[], own: string[], reason: string}}
 */
function checkWritable(repoRoot) {
  const clean = assertCleanWorktree(repoRoot)
  const own = []
  const others = []
  clean.dirtyFiles.forEach(function (rel) {
    const abs = path.resolve(repoRoot, rel)
    if (SELF_WRITTEN.has(abs)) own.push(rel)
    else others.push(rel)
  })
  return { ok: others.length === 0, dirtyFiles: others, own: own, reason: clean.reason }
}

/**
 * 找出 progress 目录所属的仓库根。
 *
 * 约定 progress 目录的上一级就是仓库根；但更稳的做法是向上找带 .git 的目录，
 * 找不到再退回「上一级」这个约定。
 * @param {string} progressRoot
 * @returns {string}
 */
function repoRootOf(progressRoot) {
  const abs = path.resolve(progressRoot)
  let dir = abs
  for (let i = 0; i < 8; i += 1) {
    if (fs.existsSync(path.join(dir, ".git"))) return dir
    const parent = path.dirname(dir)
    if (parent === dir) break
    dir = parent
  }
  return path.basename(abs) === "progress" ? path.dirname(abs) : abs
}

/**
 * 按轮次 ID 找到文件（r33 → r33-progress.md）。
 */
function findRoundFile(progressRoot, roundId) {
  const wanted = String(roundId || "").toLowerCase()
  let names = []
  try {
    names = fs.readdirSync(progressRoot)
  } catch (e) {
    return null
  }
  const hit = names
    .filter(function (name) { return /^r\d+.*\.md$/i.test(name) })
    .sort()
    .find(function (name) {
      const m = name.match(/^(r\d+)/i)
      return m && m[1].toLowerCase() === wanted
    })
  return hit ? path.join(progressRoot, hit) : null
}

/**
 * 在原始行数组里定位某个条目所在的那一行，并带回列映射。
 * @param {string[]} lines 原始行（不含行尾符）
 * @param {string} itemId 条目 ID
 * @returns {{lineIndex:number, cells:string[], columns:object}|null}
 */
function locateItemRow(lines, itemId) {
  const found = findItemBlock(lines)
  if (!found) return null

  for (const row of found.dataRows) {
    if (cellAt(row.cells, found.columns.id) === itemId) {
      return { lineIndex: row.index, cells: row.cells, columns: found.columns }
    }
  }
  return null
}

/**
 * 替换单元格但保留原来的缩进空格。
 * 原来的格子是空的（只有空格）时，统一写成一个空格包着新值。
 */
function replaceCell(originalSegment, value) {
  if (originalSegment.trim() === "") return " " + value + " "
  const lead = /^\s*/.exec(originalSegment)[0]
  const trail = /\s*$/.exec(originalSegment)[0]
  return lead + value + trail
}

/**
 * 行级 diff（显示用）：先算最长公共前缀与后缀，中间就是改动部分。
 * @returns {{start:number, removed:number, added:number, lines:string[]}}
 */
function diffLines(before, after, context) {
  const ctx = context === undefined ? 2 : context
  const a = String(before === null || before === undefined ? "" : before).split("\n")
  const b = String(after === null || after === undefined ? "" : after).split("\n")

  let p = 0
  while (p < a.length && p < b.length && a[p] === b[p]) p += 1
  let s = 0
  while (s < a.length - p && s < b.length - p && a[a.length - 1 - s] === b[b.length - 1 - s]) s += 1

  const lines = []
  const ctxStart = Math.max(0, p - ctx)
  for (let i = ctxStart; i < p; i += 1) lines.push(" " + a[i])
  for (let i = p; i < a.length - s; i += 1) lines.push("-" + a[i])
  for (let i = p; i < b.length - s; i += 1) lines.push("+" + b[i])
  if (before !== "" && before !== null && before !== undefined) {
    const ctxEnd = Math.min(a.length, a.length - s + ctx)
    for (let i = a.length - s; i < ctxEnd; i += 1) lines.push(" " + a[i])
  }

  return { start: p + 1, removed: a.length - s - p, added: b.length - s - p, lines: lines }
}

/**
 * 组装 unified 风格的 diff 文本。
 */
function makeDiff(file, before, after) {
  const diff = diffLines(before, after)
  if (diff.lines.length === 0) return ""
  return [
    "--- a/" + toSlash(file),
    "+++ b/" + toSlash(file),
    "@@ -" + diff.start + "," + diff.removed + " +" + diff.start + "," + diff.added + " @@",
  ]
    .concat(diff.lines)
    .join("\n")
}

/**
 * 路径统一成正斜杠（diff 头与 git 一致）。
 */
function toSlash(p) {
  return String(p === null || p === undefined ? "" : p).replace(/\\/g, "/")
}

/**
 * 勾选写回：只改目标行的目标单元格。
 * @param {string} progressRoot learning/progress 目录
 * @param {string} roundId 轮次 ID（如 r33）
 * @param {string} itemId 条目 ID
 * @param {{status?:string, doneDate?:string, commitSha?:string}} patch
 * @returns {{diff: string, warnings: string[]}}
 */
function updateRoundItem(progressRoot, roundId, itemId, patch) {
  const options = patch || {}
  const warnings = []
  const repoRoot = repoRootOf(progressRoot)

  const writable = checkWritable(repoRoot)
  if (!writable.ok) throw dirtyError(writable.dirtyFiles)
  if (writable.own.length > 0) {
    warnings.push("工作区里还有 app 之前写过的未提交改动：" + writable.own.join("、"))
  }

  const file = findRoundFile(progressRoot, roundId)
  if (!file) throw notFoundError("找不到轮次：" + roundId)

  const before = fs.readFileSync(file, "utf8")
  const rawLines = splitLines(before)
  const texts = rawLines.map(function (line) {
    return line.text
  })

  const located = locateItemRow(texts, itemId)
  if (!located) throw notFoundError("轮次 " + roundId + " 里找不到条目：" + itemId)

  const columns = located.columns
  const targets = [
    { key: "status", column: "status", label: "状态" },
    { key: "doneDate", column: "doneDate", label: "完成时间" },
    { key: "commitSha", column: "commitSha", label: "commit SHA" },
  ]

  // 只动目标行；目标行里也只动目标单元格（G25）
  const originalLine = rawLines[located.lineIndex].text
  const segments = originalLine.split("|")
  const offset = originalLine.trim().charAt(0) === "|" ? 1 : 0
  targets.forEach(function (target) {
    const value = options[target.key]
    if (value === undefined || value === null) return
    const column = columns[target.column]
    if (column === undefined) {
      warnings.push("该轮次的清单表没有「" + target.label + "」列，无法写入")
      return
    }
    const at = offset + column
    if (at < 0 || at >= segments.length) return
    segments[at] = replaceCell(segments[at], String(value))
  })

  const newLine = segments.join("|")
  if (newLine === originalLine) {
    return { diff: "", warnings: warnings }
  }

  rawLines[located.lineIndex] = { text: newLine, eol: rawLines[located.lineIndex].eol }
  const after = rawLines
    .map(function (line) {
      return line.text + line.eol
    })
    .join("")

  fs.writeFileSync(file, after, "utf8")
  SELF_WRITTEN.add(path.resolve(file))
  return {
    diff: makeDiff(path.relative(repoRoot, file), before, after),
    warnings: warnings,
  }
}

/**
 * 题目 → 笔记 frontmatter 的 stack。
 */
function stackForProblem(problem) {
  const tracks = (problem && problem.tracks) || []
  for (const track of tracks) {
    if (TRACK_STACK[track]) return TRACK_STACK[track]
  }
  return "c"
}

/**
 * 今天的日期（本地时区，YYYY-MM-DD）。
 */
function todayStamp() {
  const now = new Date()
  const y = now.getFullYear()
  const m = String(now.getMonth() + 1).padStart(2, "0")
  const d = String(now.getDate()).padStart(2, "0")
  return y + "-" + m + "-" + d
}

/**
 * 错题订正的 frontmatter。
 * 键序：notes/_meta/schema.md 的基线 8 键顺序不变，扩展键 unresolved / problem 追加在后面。
 */
function renderCorrectionFrontmatter(problem, today) {
  return [
    "---",
    "stack: " + stackForProblem(problem),
    "difficulty: " + (problem.difficulty || "medium"),
    "status: in-progress",
    "links: []",
    "ref: []",
    "created: " + today,
    "updated: " + today,
    "tags: [practice, correction]",
    "unresolved: true",
    "problem: " + problem.id,
    "---",
  ].join("\n")
}

/**
 * 一段订正正文。
 */
function renderCorrectionSection(problem, text, today) {
  const track = (problem.tracks && problem.tracks[0]) || "interview"
  const topics = (problem.topics || []).join("、")
  return [
    "## 订正 " + today,
    "",
    String(text || "").trim(),
    "",
    "## 关联",
    "",
    "- 题目：`bank/" + track + "/" + problem.id + ".md`（`" + problem.id + "`）",
    "- 知识点：" + (topics || "（无）"),
  ].join("\n")
}

/**
 * 写错题订正：新建或追加一篇 notes/practice/<id>-订正.md。
 * @param {string} learningRoot learning 仓根目录
 * @param {object} problem M1 的 Problem 形状
 * @param {string} text 订正正文
 * @returns {{file: string, diff: string}}
 */
function writeCorrection(learningRoot, problem, text) {
  const root = path.resolve(learningRoot)
  const writable = checkWritable(root)
  if (!writable.ok) throw dirtyError(writable.dirtyFiles)

  const id = String(problem.id)
  const dir = path.join(root, "notes", "practice")
  const file = path.join(dir, id + "-订正.md")
  const rel = path.join("notes", "practice", id + "-订正.md")
  const today = todayStamp()

  let before = ""
  if (fs.existsSync(file)) before = fs.readFileSync(file, "utf8")

  let after
  if (before.trim() === "") {
    after = [
      renderCorrectionFrontmatter(problem, today),
      "",
      "# " + (problem.title || id) + " · 错题订正",
      "",
      renderCorrectionSection(problem, text, today),
      "",
    ].join("\n")
  } else {
    // 追加时沿用原文件的行尾符，别把 CRLF 的文件写成一半 LF 一半 CRLF
    const eol = before.indexOf("\r\n") !== -1 ? "\r\n" : "\n"
    const section = renderCorrectionSection(problem, text, today).split("\n").join(eol)
    after = before.replace(/\s*$/, "") + eol + eol + section + eol
  }

  fs.mkdirSync(dir, { recursive: true })
  fs.writeFileSync(file, after, "utf8")
  SELF_WRITTEN.add(path.resolve(file))
  return { file: file, diff: makeDiff(rel, before, after) }
}

module.exports = {
  parseRounds: parseRounds,
  parseRoundText: parseRoundText,
  normalizeHeader: normalizeHeader,
  splitRow: splitRow,
  matchColumns: matchColumns,
  assertCleanWorktree: assertCleanWorktree,
  updateRoundItem: updateRoundItem,
  writeCorrection: writeCorrection,
  diffLines: diffLines,
  makeDiff: makeDiff,
  stackForProblem: stackForProblem,
}
