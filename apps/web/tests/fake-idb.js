"use strict"
/**
 * fake-idb.js —— Node 下的 IndexedDB 内存替身
 *
 * 复刻真实 IndexedDB 的语义：
 *   - `indexedDB.open(name, version)` **立即**发起请求（不需再调 open()）
 *   - 支持 `keyPath`：put 时若没显式给 key，取 value.key
 *   - onupgradeneeded → onsuccess 的顺序与真实一致，且都是异步
 *   - 事务在下一个宏任务触发 oncomplete
 *
 * photos.test.js 与 trip-store.test.js 共用这一份。
 */

/**
 * 装一个内存版 IndexedDB 替身。
 * @returns {object} indexedDB 替身
 */
function install () {
  const dbs = new Map()

  function getStore (db, name) {
    if (!db.stores.has(name)) db.stores.set(name, new Map())
    return db.stores.get(name)
  }

  const indexedDB = {
    open: function (name, version) {
      const request = {
        onupgradeneeded: null,
        onsuccess: null,
        onerror: null,
        result: null,
        error: null,
        readyState: "pending"
      }

      // 真实 API 里 open() 立即发起请求，这里用 setTimeout 模拟异步
      setTimeout(function () {
        if (!dbs.has(name)) dbs.set(name, { stores: new Map(), version: version || 1 })
        const db = dbs.get(name)

        const handle = {
          name: name,
          version: db.version,
          objectStoreNames: { contains: function (n) { return db.stores.has(n) } },
          createObjectStore: function (storeName) {
            getStore(db, storeName)
            return { createIndex: function () {} }
          },
          transaction: function (storeNames, mode) {
            const tx = {
              oncomplete: null,
              onerror: null,
              onabort: null,
              objectStore: function (storeName) {
                const store = getStore(db, storeName)
                return {
                  // 支持 keyPath：没显式给 key 时取 value.key
                  put: function (value, key) {
                    const k = key !== undefined ? key : (value && value.key)
                    store.set(k, value)
                  },
                  get: function (key) { return store.get(key) },
                  delete: function (key) { store.delete(key) },
                  getAll: function () { return Array.from(store.values()) },
                  clear: function () { store.clear() },
                  count: function () { return store.size }
                }
              }
            }
            // 事务提交是异步的（与真实一致：写操作要等事务提交才算完）
            setTimeout(function () {
              if (typeof tx.oncomplete === "function") tx.oncomplete()
            }, 0)
            return tx
          },
          close: function () {}
        }

        request.result = handle
        request.readyState = "done"

        // 新库才需要 upgrade；已有库直接 success
        const isNew = !db.stores.size
        if (isNew && typeof request.onupgradeneeded === "function") {
          request.onupgradeneeded({ target: request })
        }
        if (typeof request.onsuccess === "function") {
          request.onsuccess({ target: request })
        }
      }, 0)

      return request
    },

    deleteDatabase: function (name) {
      const request = { onsuccess: null, onerror: null }
      setTimeout(function () {
        dbs.delete(name)
        if (typeof request.onsuccess === "function") request.onsuccess({ target: request })
      }, 0)
      return request
    }
  }

  global.indexedDB = indexedDB
  return indexedDB
}

module.exports = { install: install }
