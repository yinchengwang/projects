/**
 * store.js —— 数据域工厂
 *
 * 替代原 Taro 版的 12 个 Zustand store。每个域只需声明存储键与默认值，
 * 增删改查与落盘由工厂统一处理。
 *
 * 约定：每次写操作立即落盘，并返回最新数据，调用方直接拿去 setData。
 */

const storage = require('./storage.js')
const id = require('./id.js')

/**
 * 列表型数据域
 * @param {{key:string, idField?:string, seed?:function}} options
 */
function createListStore (options) {
  const key = options.key
  const idField = options.idField || 'id'

  function getAll () {
    const list = storage.get(key, null)
    if (list === null) {
      const initial = options.seed ? options.seed() : []
      storage.set(key, initial)
      return initial
    }
    return Array.isArray(list) ? list : []
  }

  function setAll (list) {
    const safe = Array.isArray(list) ? list : []
    storage.set(key, safe)
    return safe
  }

  /**
   * 新增（默认插到最前，符合"最近优先"的列表习惯）
   * @param {object} input 不含 id 与 createdAt 的字段
   */
  function add (input) {
    const item = Object.assign({
      createdAt: new Date().toISOString()
    }, input)
    if (!item[idField]) item[idField] = id.genId()

    const list = getAll()
    list.unshift(item)
    setAll(list)
    return item
  }

  /** 按 id 局部更新 */
  function update (itemId, patch) {
    const list = getAll()
    let updated = null
    const next = list.map(function (item) {
      if (item[idField] !== itemId) return item
      updated = Object.assign({}, item, patch)
      return updated
    })
    if (updated) setAll(next)
    return updated
  }

  /** 按 id 删除 */
  function remove (itemId) {
    const list = getAll()
    const next = list.filter(function (item) {
      return item[idField] !== itemId
    })
    const changed = next.length !== list.length
    if (changed) setAll(next)
    return changed
  }

  /** 按 id 查找 */
  function find (itemId) {
    const list = getAll()
    for (let i = 0; i < list.length; i++) {
      if (list[i][idField] === itemId) return list[i]
    }
    return null
  }

  /** 清空 */
  function clear () {
    return setAll([])
  }

  /** 数量 */
  function count (predicate) {
    const list = getAll()
    if (!predicate) return list.length
    return list.filter(predicate).length
  }

  return {
    key: key,
    getAll: getAll,
    setAll: setAll,
    add: add,
    update: update,
    remove: remove,
    find: find,
    clear: clear,
    count: count
  }
}

/**
 * 单对象型数据域（settings 等）
 * @param {{key:string, defaultValue:object}} options
 */
function createObjectStore (options) {
  const key = options.key
  const defaults = options.defaultValue || {}

  function get () {
    const stored = storage.get(key, null)
    if (stored === null || typeof stored !== 'object') {
      storage.set(key, defaults)
      return Object.assign({}, defaults)
    }
    return Object.assign({}, defaults, stored)
  }

  /** 合并写入 */
  function patch (partial) {
    const next = Object.assign(get(), partial)
    storage.set(key, next)
    return next
  }

  /** 恢复默认 */
  function reset () {
    storage.set(key, defaults)
    return Object.assign({}, defaults)
  }

  return {
    key: key,
    get: get,
    patch: patch,
    reset: reset,
    defaults: defaults
  }
}

module.exports = {
  createListStore: createListStore,
  createObjectStore: createObjectStore
}
