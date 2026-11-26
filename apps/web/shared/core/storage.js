"use strict"
/**
 * storage.js —— 本地存储封装（localStorage 后端）
 *
 * 前缀 `db:`，与小程序版同键名，便于已有数据直接沿用。
 *
 * 与小程序版的差异只有一处：底层从 `wx.setStorageSync` 换成 `localStorage`。
 * 对外的 9 个函数签名与语义完全不变，因此上层 store/stores/seed 等
 * 30 余个调用方零改动。
 *
 * localStorage 只能存字符串，故一律 JSON.stringify / JSON.parse。
 * 键不存在时 getItem 返回 null（小程序版返回 ''），本模块统一归一为 null。
 */

/** 存储键前缀 */
const PREFIX = "db:"

/**
 * 数据域的存储键。
 *
 * 注意：knowledge-hub 会在加载时 Object.assign 扩展这个对象（加 daily、
 * excerpts 等 14 个键）。本模块导出的是**同一个对象引用**，扩展对调用方可见。
 */
const KEYS = {
  todos: "todos",
  techNotes: "tech_notes",
  snippets: "snippets",
  interview: "interview",
  resources: "resources",
  reading: "reading",
  fitness: "fitness",
  mood: "mood",
  schedule: "schedule",
  projects: "projects",
  insights: "insights",
  settings: "settings",
  seeded: "__seeded__"
}

function fullKey (key) {
  return PREFIX + key
}

/**
 * 读取
 * @param {string} key
 * @param {*} defaultValue 读不到时返回
 * @returns {*}
 */
function get (key, defaultValue) {
  try {
    const raw = localStorage.getItem(fullKey(key))
    if (raw === null || raw === undefined || raw === "") return defaultValue
    try {
      return JSON.parse(raw)
    } catch (e) {
      return defaultValue
    }
  } catch (e) {
    return defaultValue
  }
}

/**
 * 写入
 * @param {string} key
 * @param {*} value
 * @returns {boolean} 写成功返回 true，配额不足等失败返回 false
 */
function set (key, value) {
  try {
    localStorage.setItem(fullKey(key), JSON.stringify(value))
    return true
  } catch (e) {
    console.warn("[storage] 写入失败", key, e)
    return false
  }
}

/**
 * 删除单个键
 * @param {string} key
 * @returns {boolean}
 */
function remove (key) {
  try {
    localStorage.removeItem(fullKey(key))
    return true
  } catch (e) {
    return false
  }
}

/** 列出本应用的全部键（不含前缀） */
function keys () {
  const out = []
  try {
    for (let i = 0; i < localStorage.length; i += 1) {
      const k = localStorage.key(i)
      if (k !== null && k.indexOf(PREFIX) === 0) out.push(k.slice(PREFIX.length))
    }
  } catch (e) {
    console.warn("[storage] 列举键失败", e)
  }
  return out
}

/**
 * 清空本应用全部数据（只清 db: 前缀的键）
 * @returns {boolean}
 */
function clearAll () {
  try {
    keys().forEach(function (k) { localStorage.removeItem(fullKey(k)) })
    return true
  } catch (e) {
    console.warn("[storage] 清空失败", e)
    return false
  }
}

/**
 * 导出全部数据为 JSON 字符串
 * @returns {string} { exportedAt, version, data: { key: value } }
 */
function exportAll () {
  const data = {}
  try {
    keys().forEach(function (shortKey) {
      if (shortKey === KEYS.seeded) return
      const raw = localStorage.getItem(fullKey(shortKey))
      if (raw === null || raw === undefined || raw === "") return
      try {
        data[shortKey] = JSON.parse(raw)
      } catch (e) {
        data[shortKey] = raw
      }
    })
  } catch (e) {
    console.warn("[storage] 导出失败", e)
  }
  return JSON.stringify({
    exportedAt: new Date().toISOString(),
    version: 1,
    data: data
  }, null, 2)
}

/**
 * 从 JSON 导入
 * @param {string} json
 * @param {'merge'|'overwrite'} mode merge 只填空缺键，overwrite 全量覆盖
 * @returns {{ok:boolean, count:number, error?:string}}
 */
function importAll (json, mode) {
  const m = mode || "merge"
  try {
    const parsed = JSON.parse(json)
    const data = parsed && parsed.data
    if (!data || typeof data !== "object" || Array.isArray(data)) {
      return { ok: false, count: 0, error: "备份格式不正确：缺少 data 字段" }
    }
    let count = 0
    Object.keys(data).forEach(function (k) {
      if (m === "overwrite") {
        set(k, data[k])
      } else {
        const existing = get(k, null)
        if (existing === null) set(k, data[k])
      }
      count += 1
    })
    return { ok: true, count: count }
  } catch (err) {
    return { ok: false, count: 0, error: (err && err.message) || String(err) }
  }
}

/**
 * 统计占用。
 * localStorage 没有 getStorageInfoSync，容量按字符数估算（与小程序版的 KB 口径不同，
 * 但只用于展示，不参与任何判定）。
 * @returns {{keys:number, currentSize:number}}
 */
function usage () {
  try {
    const list = keys()
    let size = 0
    list.forEach(function (k) {
      const raw = localStorage.getItem(fullKey(k))
      if (raw !== null) size += raw.length
    })
    return { keys: list.length, currentSize: Math.ceil(size / 1024) }
  } catch (e) {
    return { keys: 0, currentSize: 0 }
  }
}

module.exports = {
  PREFIX: PREFIX,
  KEYS: KEYS,
  get: get,
  set: set,
  remove: remove,
  keys: keys,
  clearAll: clearAll,
  exportAll: exportAll,
  importAll: importAll,
  usage: usage
}
