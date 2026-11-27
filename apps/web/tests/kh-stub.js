"use strict"
/**
 * kh-stub.js —— Node 环境下的浏览器平台替身
 *
 * 与 wb-stub.js 同构，独立一份便于各模块单独演进。
 * 装一个内存版 localStorage（接口与浏览器一致），让 storage / store / stores
 * 能跑真实读写路径。
 */

/** 内存版 localStorage */
const memory = new Map()

/** 安装 localStorage 到 global */
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
}

/** 清空内存存储（单测每个用例前调用） */
function reset () {
  memory.clear()
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

module.exports = {
  installLocalStorage: installLocalStorage,
  reset: reset,
  raw: raw,
  dbKeys: dbKeys,
  memory: memory
}
