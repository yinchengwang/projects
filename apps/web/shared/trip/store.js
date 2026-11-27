"use strict"
/**
 * store.js —— trip-log 的数据层
 *
 * 元数据存 localStorage（db: 前缀），照片存 IndexedDB（见 shared/photos.js）。
 *
 * 与小程序版的差异：云端 database() → 本地 storage + photos。
 */

const storage = require("../core/storage.js")
const id = require("../core/id.js")
const photos = require("../photos.js")

const KEY = "trips"

/** 读全部元数据（不含照片二进制） */
function readAll () {
  const list = storage.get(KEY, null)
  return Array.isArray(list) ? list : []
}

/** 写全部元数据 */
function writeAll (list) {
  storage.set(KEY, Array.isArray(list) ? list : [])
}

/**
 * 同步取全部记录（只读 localStorage，不碰照片）。
 * 列表页用这个 —— 渲染不必等 IndexedDB。
 * @returns {Array}
 */
function listSync () {
  const all = readAll()
  all.sort(function (a, b) {
    if (a.date !== b.date) return a.date < b.date ? 1 : -1
    return (b.createdAt || 0) - (a.createdAt || 0)
  })
  return all
}

/**
 * 全部记录，按日期倒序（同日按创建时间倒序）。
 * @returns {Promise<Array>}
 */
async function list () {
  return listSync()
}

/**
 * 取一条记录。
 * @param {string} recordId
 * @returns {Promise<object|null>}
 */
async function get (recordId) {
  const all = readAll()
  for (let i = 0; i < all.length; i++) {
    if (all[i].id === recordId) return all[i]
  }
  return null
}

/**
 * 新增一条记录。
 * @param {{id?: string, title?: string, date?: string, weather?: string,
 *          location?: string, companions?: string, tags?: Array, notes?: string,
 *          photos?: Array<{photoId: string, blob: object}>}} input
 * @returns {Promise<object>} 新记录（含 id 与 photoIds）
 */
async function add (input) {
  const it = input || {}
  const recordId = it.id || id.genId()

  // 先存照片，拿到成功再写元数据 —— 避免出现「有元数据没照片」的半截状态
  const photoIds = []
  const incoming = Array.isArray(it.photos) ? it.photos : []
  for (const p of incoming) {
    await photos.put(recordId, p.photoId, p.blob)
    photoIds.push(p.photoId)
  }

  const record = {
    id: recordId,
    title: it.title || "",
    date: it.date || "",
    weather: it.weather || "",
    location: it.location || "",
    companions: it.companions || "",
    tags: Array.isArray(it.tags) ? it.tags : [],
    notes: it.notes || "",
    photoIds: photoIds,
    createdAt: Date.now()
  }
  const all = readAll()
  all.push(record)
  writeAll(all)
  return record
}

/**
 * 更新一条记录。patch 里的 photos 同 add 的语义（会替换掉旧的）。
 * @param {string} recordId
 * @param {object} patch
 * @returns {Promise<object|null>}
 */
async function update (recordId, patch) {
  const all = readAll()
  let updated = null
  const next = []
  for (const r of all) {
    if (r.id !== recordId) {
      next.push(r)
      continue
    }
    const merged = Object.assign({}, r, patch || {})
    if (Array.isArray(patch && patch.photos)) {
      // 换了照片：先删旧的，再存新的
      await photos.deleteAll(recordId)
      merged.photoIds = []
      for (const p of patch.photos) {
        await photos.put(recordId, p.photoId, p.blob)
        merged.photoIds.push(p.photoId)
      }
      delete merged.photos
    }
    updated = merged
    next.push(merged)
  }
  if (updated) writeAll(next)
  return updated
}

/**
 * 删除一条记录，连同它的照片。
 * @param {string} recordId
 * @returns {Promise<boolean>} 是否真的删掉了
 */
async function remove (recordId) {
  const all = readAll()
  const next = all.filter(function (r) { return r.id !== recordId })
  const changed = next.length !== all.length
  if (!changed) return false
  await photos.deleteAll(recordId)
  writeAll(next)
  return true
}

/**
 * 取一条记录的照片 URL 列表。
 *
 * 返回 objectURL，**调用方负责在用完后 revokeObjectURL** —— 否则内存泄漏。
 *
 * @param {string} recordId
 * @returns {Promise<Array<{photoId: string, url: string}>>}
 */
async function getPhotoUrls (recordId) {
  const record = await get(recordId)
  if (!record) return []
  const ids = Array.isArray(record.photoIds) ? record.photoIds : []
  const out = []
  for (const photoId of ids) {
    const row = await photos.get(recordId, photoId)
    if (!row || !row.blob) continue
    let url = ""
    try {
      url = URL.createObjectURL(row.blob)
    } catch (e) {
      // Node 测试环境下 blob 不是真正的 Blob，URL.createObjectURL 会抛错
      url = ""
    }
    out.push({ photoId: photoId, url: url })
  }
  return out
}

/**
 * 清空全部记录与照片。
 * @returns {Promise<void>}
 */
async function clear () {
  const all = readAll()
  for (const r of all) {
    await photos.deleteAll(r.id)
  }
  writeAll([])
  await photos.clear()
}

module.exports = {
  list: list,
  listSync: listSync,
  get: get,
  add: add,
  update: update,
  remove: remove,
  getPhotoUrls: getPhotoUrls,
  clear: clear
}
