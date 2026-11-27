"use strict"
/**
 * photos.js —— IndexedDB 图片存储层
 *
 * trip-log 的照片原存在腾讯云（storage.uploadFile）。Web 侧没有 openid 也没有
 * 云 SDK，改用 IndexedDB 存 Blob：
 *   - 元数据（title/date/tags…）走 localStorage（db: 前缀）
 *   - 照片走 IndexedDB（存 Blob 二进制）
 *
 * 为什么要 IndexedDB 而不是 localStorage：单条记录最多 9 张 × 5MB = 45MB，
 * 远超 localStorage 的 ~5MB 配额。
 *
 * 键的构造：`<tripId>:<photoId>`，list(tripId) 用前缀匹配筛出该记录的全部照片。
 *
 * 所有方法都是 Promise 化的：IndexedDB 的原生回调 API 太难用。
 */

const DB_NAME = "apps-web-photos"
const DB_VERSION = 1
const STORE_NAME = "photos"

/** 打开数据库的 Promise（同一个库只 open 一次） */
let dbPromise = null

/**
 * IndexedDB 是否可用。
 * @returns {Promise<boolean>}
 */
function isAvailable () {
  return Promise.resolve(typeof indexedDB !== "undefined" && indexedDB !== null)
}

/**
 * 打开（或复用）数据库连接。
 * @returns {Promise<object>} IDBDatabase
 */
function openDb () {
  if (dbPromise) return dbPromise
  dbPromise = new Promise(function (resolve, reject) {
    if (typeof indexedDB === "undefined" || indexedDB === null) {
      reject(new Error("当前环境不支持 IndexedDB"))
      return
    }
    let request
    try {
      request = indexedDB.open(DB_NAME, DB_VERSION)
    } catch (e) {
      reject(new Error("IndexedDB 打开失败：" + (e && e.message ? e.message : e)))
      return
    }
    request.onupgradeneeded = function () {
      const db = request.result
      if (!db.objectStoreNames.contains(STORE_NAME)) {
        db.createObjectStore(STORE_NAME, { keyPath: "key" })
      }
    }
    request.onsuccess = function () { resolve(request.result) }
    request.onerror = function () {
      reject(new Error("IndexedDB 打开失败"))
    }
  })
  // 打开失败时不要缓存失败的 promise，否则后续永远失败
  dbPromise.catch(function () { dbPromise = null })
  return dbPromise
}

/**
 * 在一个事务里执行操作。
 *
 * `fn` 可以返回 `IDBRequest`（get / getAll / count 这类查询），也可以返回
 * 普通值（put / delete / clear 这类写操作，真实 IndexedDB 里它们也返回
 * IDBRequest 但结果无意义）。**必须区分这两种**：真实浏览器里查询返回的是
 * IDBRequest 而不是数据本身，直接把返回值当数据用会拿到一个没有 filter /
 * map 的对象 —— 这正是 `photos.list()` 在浏览器里报
 * 「(all || []).filter is not a function」的原因。
 *
 * @param {string[]} storeNames
 * @param {string} mode
 * @param {Function} fn 接收 objectStore，返回 IDBRequest 或普通值
 * @returns {Promise<*>} 查询类返回 request.result，写入类返回 undefined
 */
function withStore (storeNames, mode, fn) {
  return openDb().then(function (db) {
    return new Promise(function (resolve, reject) {
      let tx
      try {
        tx = db.transaction(storeNames, mode)
      } catch (e) {
        reject(new Error("IndexedDB 事务创建失败：" + (e && e.message ? e.message : e)))
        return
      }
      let settled = false
      let request = null
      let plainValue
      tx.oncomplete = function () {
        if (settled) return
        settled = true
        resolve(request ? request.result : plainValue)
      }
      tx.onerror = function () {
        if (settled) return
        settled = true
        reject(new Error("IndexedDB 操作失败"))
      }
      tx.onabort = function () {
        if (settled) return
        settled = true
        reject(new Error("IndexedDB 操作已中止"))
      }
      try {
        const out = fn(tx.objectStore(STORE_NAME))
        if (out && typeof out === "object" && "onsuccess" in out) request = out
        else plainValue = out
      } catch (e) {
        settled = true
        reject(new Error("IndexedDB 操作失败：" + (e && e.message ? e.message : e)))
      }
    })
  })
}

/** 构造复合键 */
function makeKey (tripId, photoId) {
  return String(tripId) + ":" + String(photoId)
}

/**
 * 存一张照片。
 * @param {string} tripId
 * @param {string} photoId
 * @param {Blob} blob
 * @returns {Promise<void>}
 */
function put (tripId, photoId, blob) {
  return withStore([STORE_NAME], "readwrite", function (store) {
    store.put({ key: makeKey(tripId, photoId), tripId: String(tripId), photoId: String(photoId), blob: blob })
  }).then(function () {})
}

/**
 * 取一张照片。
 * @param {string} tripId
 * @param {string} photoId
 * @returns {Promise<object|null>} 存的记录（含 blob 字段）；没有则 null
 */
function get (tripId, photoId) {
  return withStore([STORE_NAME], "readonly", function (store) {
    return store.get(makeKey(tripId, photoId))
  }).then(function (row) {
    // IndexedDB 的 get 对不存在的键返回 undefined —— 统一成 null，
    // 调用方用 `if (!row)` 判断即可，不必同时处理两种「空」
    return row === undefined ? null : row
  })
}

/**
 * 删一张照片。
 * @param {string} tripId
 * @param {string} photoId
 * @returns {Promise<void>}
 */
function remove (tripId, photoId) {
  return withStore([STORE_NAME], "readwrite", function (store) {
    store.delete(makeKey(tripId, photoId))
  }).then(function () {})
}

/**
 * 列出某条记录的全部照片 id。
 * @param {string} tripId
 * @returns {Promise<string[]>}
 */
function list (tripId) {
  return withStore([STORE_NAME], "readonly", function (store) {
    return store.getAll()
  }).then(function (all) {
    const prefix = String(tripId) + ":"
    return (all || [])
      .filter(function (r) { return r.key && r.key.indexOf(prefix) === 0 })
      .map(function (r) { return r.photoId })
  })
}

/**
 * 清掉某条记录的全部照片。
 * @param {string} tripId
 * @returns {Promise<void>}
 */
function deleteAll (tripId) {
  return list(tripId).then(function (ids) {
    const jobs = ids.map(function (id) { return remove(tripId, id) })
    return Promise.all(jobs)
  }).then(function () {})
}

/**
 * 统计全部照片数。
 * @returns {Promise<number>}
 */
function count () {
  return withStore([STORE_NAME], "readonly", function (store) {
    return store.count()
  })
}

/**
 * 清空全部照片。
 * @returns {Promise<void>}
 */
function clear () {
  return withStore([STORE_NAME], "readwrite", function (store) {
    store.clear()
  }).then(function () {})
}

module.exports = {
  put: put,
  get: get,
  delete: remove,
  list: list,
  deleteAll: deleteAll,
  count: count,
  clear: clear,
  isAvailable: isAvailable
}
