"use strict"
/**
 * wb-stub.js —— Node 环境下的浏览器平台替身
 *
 * 提供两样东西：
 *   1. localStorage —— 内存版，接口与浏览器一致（getItem/setItem/removeItem/key/length）
 *   2. 平台层替身 —— 让被测代码可以调用 platform 的交互 API 并观测结果
 *
 * 与小程序版 wx-stub.js 的区别：小程序版把 wx 装到 global.wx；
 * 这里装 localStorage 与 global.__WB_PLATFORM__。
 */

/** 内存版 localStorage */
const memory = new Map()
const wxStorage = new Map()
const calls = []

function installLocalStorage () {
  global.localStorage = {
    getItem: function (k) { return memory.has(k) ? memory.get(k) : null },
    setItem: function (k, v) { memory.set(k, String(v)) },
    removeItem: function (k) { memory.delete(k) },
    clear: function () { memory.clear() },
    key: function (i) {
      const keys = Array.from(memory.keys())
      return i < keys.length ? keys[i] : null
    },
    get length () { return memory.size }
  }
  // 少量旧断言还需确认 wx.storageSync 与 localStorage 相互隔离。
  global.wx = {
    getStorageSync: function (k) { return wxStorage.has(k) ? wxStorage.get(k) : "" },
    setStorageSync: function (k, v) { wxStorage.set(k, v) },
    removeStorageSync: function (k) { wxStorage.delete(k) },
    showToast: function (opts) { calls.push({ name: "showToast", opts: opts }) },
    showModal: function (opts) { calls.push({ name: "showModal", opts: opts }); return Promise.resolve({ confirm: false, content: "" }) },
    showActionSheet: function (opts) { calls.push({ name: "showActionSheet", opts: opts }); return Promise.resolve({ tapIndex: -1 }) },
    navigateTo: function (opts) { calls.push({ name: "navigateTo", url: opts && opts.url }) },
    navigateBack: function () { calls.push({ name: "navigateBack" }) },
    switchTab: function (opts) { calls.push({ name: "switchTab", url: opts && opts.url }) },
    setClipboardData: function (opts) { calls.push({ name: "setClipboardData", opts: opts }); if (opts && typeof opts.success === "function") opts.success({ errMsg: "setClipboardData:ok" }); return Promise.resolve() },
    getClipboardData: function (opts) { calls.push({ name: "getClipboardData" }); if (opts && opts.success) opts.success({ data: "" }) },
    vibrateShort: function (opts) { calls.push({ name: "vibrateShort", opts: opts }) },
    setNavigationBarTitle: function (opts) { calls.push({ name: "setNavigationBarTitle", opts: opts }) }
  }
}

/** 清空内存存储（单测每个用例前调用） */
function reset () {
  memory.clear()
  wxStorage.clear()
}

/** 直接读内存，便于断言 */
function raw (key) {
  return memory.has(key) ? memory.get(key) : undefined
}

/** 读取全部 db: 前缀的键 */
function dbKeys () {
  return Array.from(memory.keys()).filter(function (k) {
    return k.indexOf("db:") === 0
  })
}

/**
 * 兼容迁入用例里尚未改写的 wx.getStorageSync。
 * 仅用于断言其他命名空间键未被删除，不供业务代码使用。
 */
function getStorageSync (key) {
  return wxStorage.has(key) ? wxStorage.get(key) : ""
}

module.exports = {
  installLocalStorage: installLocalStorage,
  reset: reset,
  raw: raw,
  dbKeys: dbKeys,
  getStorageSync: getStorageSync,
  memory: memory
}
