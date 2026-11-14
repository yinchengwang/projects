"use strict"
/**
 * index.js —— 三个 loader 的统一入口 + 单元索引缓存
 *
 * 契约 §3.4 冻结了行为：
 *   - 去重：同一 id 出现多次时先到先得，顺序是 loader C → loader A → loader B
 *   - 顺序：units 按 id 字符串升序
 *   - 不抛错：任何单个文件解析失败只进 warnings
 *
 * 为什么要这个顺序：loader C 承载的是 P4/P5 新产出的单元文件，
 * 它的 status 是执行器校验过的裁决，必须压过 loader B 的「教材即 verified」默认值。
 */

const fs = require("node:fs")
const path = require("node:path")

const loaderA = require("./loader-a")
const loaderB = require("./loader-b")
const loaderC = require("./loader-c")
const { TRACKS, GROKKING_DB_CARDS, GROKKING_AI_CARDS } = require("../track/tracks")

// 契约 §13：三条顶层轨的**唯一定义处**是 core/track/tracks.js。这里只引用，不另开一份。

/**
 * 列出目录下的 .md 文件名（不递归），按名字排序。
 * @param {string} dir
 * @returns {string[]}
 */
function listMarkdown(dir) {
  let names = []
  try {
    names = fs.readdirSync(dir)
  } catch (e) {
    return []
  }
  return names
    .filter(function (name) {
      return name.endsWith(".md")
    })
    .sort()
}

/**
 * 读一个文本文件；读不到返回 null。
 * @param {string} abs
 * @returns {string|null}
 */
function readText(abs) {
  try {
    return fs.readFileSync(abs, "utf8")
  } catch (e) {
    return null
  }
}

/**
 * 扫 loader C 的单元文件。
 * @param {string|null} unitsDir learning/units 的绝对路径
 * @param {string[]} warnings 收集警告
 * @returns {object[]}
 */
function loadFromUnitsDir(unitsDir, warnings) {
  const units = []
  if (!unitsDir) return units
  TRACKS.forEach(function (track) {
    // track 是**对象**，轨道代码取 track.code（不是 track 本身）。
    const dir = path.join(unitsDir, track.code)
    listMarkdown(dir).forEach(function (name) {
      const text = readText(path.join(dir, name))
      if (text === null) {
        warnings.push("读不了单元文件：" + name)
        return
      }
      const res = loaderC.parseUnitFile(text, { path: track.code + "/" + name })
      ;(res.warnings || []).forEach(function (w) {
        warnings.push(w)
      })
      if (res.unit) units.push(res.unit)
    })
  })
  return units
}

/**
 * 扫 loader A 的题库目录（bank/<子轨>/*.md）。
 * @param {string|null} bankRoot 题库根的绝对路径
 * @param {string[]} warnings
 * @returns {object[]}
 */
function loadFromBank(bankRoot, warnings) {
  const units = []
  if (!bankRoot) return units
  let subs = []
  try {
    subs = fs
      .readdirSync(bankRoot, { withFileTypes: true })
      .filter(function (item) {
        return item.isDirectory()
      })
      .map(function (item) {
        return item.name
      })
      .sort()
  } catch (e) {
    warnings.push("题库目录读不到：" + bankRoot)
    return units
  }
  subs.forEach(function (sub) {
    const dir = path.join(bankRoot, sub)
    listMarkdown(dir).forEach(function (name) {
      const text = readText(path.join(dir, name))
      if (text === null) {
        warnings.push("读不了题库文件：" + name)
        return
      }
      const res = loaderA.parseUnitMarkdown(text, { track: "algorithm", relPath: "bank/" + sub + "/" + name })
      ;(res.warnings || []).forEach(function (w) {
        warnings.push(w)
      })
      if (res.unit) units.push(res.unit)
    })
  })
  return units
}

/**
 * 扫 loader B 的 grokking 卡。
 *
 * 只扫契约 §14.1 冻结的 29 张卡（db 16 + ai 13）——
 * grokking 目录下还有 59 张纯算法 / C++ 并发卡，不属本轮三轨范围。
 *
 * @param {string|null} grokkingDir learning/scaffold/grokking 的绝对路径
 * @param {string|null} unitsDir learning/units 的绝对路径
 * @param {string[]} warnings
 * @returns {object[]}
 */
function loadFromCards(grokkingDir, unitsDir, warnings) {
  const units = []
  if (!grokkingDir) return units
  const cards = []
  GROKKING_DB_CARDS.forEach(function (stem) {
    cards.push({ stem: stem, track: "database" })
  })
  GROKKING_AI_CARDS.forEach(function (stem) {
    cards.push({ stem: stem, track: "ai" })
  })
  cards.forEach(function (card) {
    const dir = path.join(grokkingDir, card.stem)
    if (!fs.existsSync(dir)) {
      warnings.push("卡目录不存在：" + card.stem)
      return
    }
    const res = loaderB.parseCardDirectory(dir, {
      learningRoot: path.dirname(path.dirname(grokkingDir)),
      track: card.track,
      unitsDir: unitsDir,
    })
    ;(res.warnings || []).forEach(function (w) {
      warnings.push(w)
    })
    if (res.unit) units.push(res.unit)
  })
  return units
}

/**
 * 加载全部单元。
 *
 * @param {object} opts { learningRoot, localBank, unitsDir, grokkingDir }
 * @returns {{units: object[], warnings: string[], byId: object}}
 */
function loadUnits(opts) {
  const options = opts || {}
  const warnings = []

  // 去重顺序：C → A → B
  const collected = []
    .concat(loadFromUnitsDir(options.unitsDir, warnings))
    .concat(loadFromBank(options.learningRoot ? path.join(options.learningRoot, "bank") : null, warnings))
    .concat(loadFromBank(options.localBank, warnings))
    .concat(loadFromCards(options.grokkingDir, options.unitsDir, warnings))

  const byId = {}
  const units = []
  collected.forEach(function (unit) {
    if (Object.prototype.hasOwnProperty.call(byId, unit.id)) {
      warnings.push("单元 id 重复：" + unit.id + "（保留先出现的）")
      return
    }
    byId[unit.id] = unit
    units.push(unit)
  })

  units.sort(function (a, b) {
    return a.id < b.id ? -1 : a.id > b.id ? 1 : 0
  })

  return { units: units, warnings: warnings, byId: byId }
}

/**
 * 把 loadUnits 的结果物化进 units 表（整体覆盖语义）。
 *
 * 依赖 store.replaceUnits / store.listUnits（server/db.js 提供）。
 * 不抛错：任何加载失败只进 warnings。
 *
 * @param {object} store
 * @param {object} opts 同 loadUnits
 * @returns {{count: number, warnings: string[]}}
 */
function refreshUnitIndex(store, opts, preloaded) {
  // 契约 §3.4：第三个可选参数 preloaded —— 调用方已经有 loadUnits 的结果时直接传进来，
  // 本函数**不再扫盘**（启动时扫一次就够，别把 700+ 份 md 解析两遍）。
  const res = preloaded && Array.isArray(preloaded.units)
    ? preloaded
    : loadUnits(opts)
  const now = new Date().toISOString()
  const rows = res.units.map(function (unit) {
    return {
      id: unit.id,
      track: unit.track,
      kind: unit.kind,
      title: unit.title,
      path: unit.path,
      status: unit.status,
      topics: JSON.stringify(unit.topics || []),
      difficulty: unit.difficulty,
      subtracks: JSON.stringify(unit.subtracks || []),
      updated: now,
    }
  })
  store.replaceUnits(rows)
  return { count: rows.length, warnings: res.warnings }
}

module.exports = {
  TRACKS: TRACKS,
  listMarkdown: listMarkdown,
  loadUnits: loadUnits,
  refreshUnitIndex: refreshUnitIndex,
}
