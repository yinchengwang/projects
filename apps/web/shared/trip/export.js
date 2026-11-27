"use strict"
/**
 * export.js —— trip-log 的数据导出与导入
 *
 * 放弃腾讯云同步后的补偿手段：把全部记录（含照片）导成一个 JSON 文件，
 * 用户可以自己存到别处，必要时再导入回来。
 *
 * 照片以 base64 嵌进 JSON —— 文件会比较大，但换来了「一个文件就是全部数据」。
 */

const store = require("./store.js")
const photos = require("../photos.js")

/**
 * Blob → base64 字符串。
 * @param {Blob} blob
 * @returns {Promise<string>}
 */
function blobToBase64 (blob) {
  return new Promise(function (resolve, reject) {
    if (typeof FileReader === "undefined") {
      // Node 测试环境：blob 是普通对象，直接取 data
      resolve(blob && blob.data !== undefined ? String(blob.data) : "")
      return
    }
    const reader = new FileReader()
    reader.onload = function () {
      const s = String(reader.result || "")
      // 去掉 "data:...;base64," 前缀
      const comma = s.indexOf(",")
      resolve(comma >= 0 ? s.slice(comma + 1) : s)
    }
    reader.onerror = function () { reject(new Error("读取图片失败")) }
    reader.readAsDataURL(blob)
  })
}

/**
 * base64 字符串 → Blob。
 * @param {string} base64
 * @returns {Promise<object>}
 */
function base64ToBlob (base64) {
  const raw = String(base64 || "")
  if (typeof Blob === "undefined") {
    // Node 测试环境
    return Promise.resolve({ data: raw, size: raw.length })
  }
  const binary = atob(raw)
  const bytes = new Uint8Array(binary.length)
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i)
  return Promise.resolve(new Blob([bytes]))
}

/**
 * 导出全部数据为 JSON 字符串。
 * @returns {Promise<string>}
 */
async function exportAll () {
  const records = await store.list()
  const out = []
  for (const r of records) {
    const ids = Array.isArray(r.photoIds) ? r.photoIds : []
    const photoList = []
    for (const photoId of ids) {
      const row = await photos.get(r.id, photoId)
      if (!row || !row.blob) continue
      photoList.push({ photoId: photoId, base64: await blobToBase64(row.blob) })
    }
    out.push({
      id: r.id,
      title: r.title,
      date: r.date,
      weather: r.weather,
      location: r.location,
      companions: r.companions,
      tags: r.tags,
      notes: r.notes,
      createdAt: r.createdAt,
      photos: photoList
    })
  }
  return JSON.stringify({
    exportedAt: new Date().toISOString(),
    version: 1,
    data: out
  }, null, 2)
}

/**
 * 从 JSON 导入。
 * @param {string} json
 * @param {'merge'|'overwrite'} mode
 * @returns {Promise<{ok: boolean, count: number, error?: string}>}
 */
async function importAll (json, mode) {
  const m = mode || "merge"
  try {
    const parsed = JSON.parse(json)
    const data = parsed && parsed.data
    if (!Array.isArray(data)) {
      return { ok: false, count: 0, error: "备份格式不正确：缺少 data 数组" }
    }
    if (m === "overwrite") {
      await store.clear()
    }
    let count = 0
    for (const r of data) {
      if (!r || !r.id) continue
      const photoList = Array.isArray(r.photos) ? r.photos : []
      const blobs = []
      for (const p of photoList) {
        blobs.push({ photoId: p.photoId, blob: await base64ToBlob(p.base64) })
      }
      await store.add({
        id: r.id,
        title: r.title,
        date: r.date,
        weather: r.weather,
        location: r.location,
        companions: r.companions,
        tags: r.tags,
        notes: r.notes,
        photos: blobs
      })
      count += 1
    }
    return { ok: true, count: count }
  } catch (err) {
    return { ok: false, count: 0, error: (err && err.message) || String(err) }
  }
}

module.exports = {
  exportAll: exportAll,
  importAll: importAll,
  blobToBase64: blobToBase64,
  base64ToBlob: base64ToBlob
}
