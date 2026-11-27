"use strict"
/**
 * wb-harness.js —— 页面加载脚手架
 *
 * 对应小程序版的 tests/page-harness.js：加载真实页面模块，构造一个可直接
 * 调用生命周期与事件处理器的实例。
 *
 * 与小程序版的差异：
 *   - 视图层的 Web 页面直接 module.exports 配置对象
 *   - 交互层从 wx.* 换成 platform.*，故 mockPlatform 挂在 platform 上
 */

const path = require("path")

/** 平台层替身：记录调用并可被 mock 覆盖 */
const calls = []
const overrides = {}

/** 最近一次剪贴板文本，供 CommonJS 页面测试验证 */
let clipboardText = ""

/** 装一个最小可用的 platform 替身 */
function installPlatformStub () {
  const noop = function () {}

  global.__WB_PLATFORM_STUB__ = {
    showToast: function (opts) { calls.push({ name: "showToast", opts: opts }); return null },
    showModal: function (opts) {
      calls.push({ name: "showModal", opts: opts })
      if (opts && typeof opts.success === "function") {
        opts.success({ confirm: false, content: "", tapIndex: -1 })
        return Promise.resolve({ confirm: false, content: "", tapIndex: -1 })
      }
      return Promise.resolve({ confirm: false, content: "" })
    },
    showActionSheet: function (opts) {
      calls.push({ name: "showActionSheet", opts: opts })
      if (opts && typeof opts.success === "function") {
        opts.success({ tapIndex: -1 })
        return Promise.resolve({ tapIndex: -1 })
      }
      return Promise.resolve({ tapIndex: -1 })
    },
    navigateTo: function (url) { calls.push({ name: "navigateTo", url: url }) },
    navigateBack: function () { calls.push({ name: "navigateBack" }) },
    switchTab: function (url) { calls.push({ name: "switchTab", url: url }) },
    showLoading: function (opts) { calls.push({ name: "showLoading", opts: opts }) },
    hideLoading: function () { calls.push({ name: "hideLoading" }) },
    setClipboardData: function (opts) {
      calls.push({ name: "setClipboardData", opts: opts })
      clipboardText = opts && opts.data ? String(opts.data) : ""
      return Promise.resolve()
    },
    getClipboardData: function () {
      calls.push({ name: "getClipboardData" })
      return Promise.resolve({ data: "" })
    },
    vibrateShort: function (opts) { calls.push({ name: "vibrateShort", opts: opts }) },
    chooseMedia: function () { return Promise.reject(new Error("当前环境不支持选择文件")) },
    previewImage: function (opts) { calls.push({ name: "previewImage", opts: opts }) },
    stopPullDownRefresh: noop
  }
}

installPlatformStub()

/**
 * 加载页面并返回可调用实例。
 * @param {string} route 形如 "pages/dashboard/index"
 */
function createPage (route) {
  const abs = path.resolve(__dirname, "..", "web", "workbench", route + ".js")
  let config = null
  global.__WB_PAGE__ = function (o) { config = o }

  let exported = null
  try {
    delete require.cache[require.resolve(abs)]
    exported = require(abs)
  } finally {
    delete global.__WB_PAGE__
  }

  // Web 页面直接 module.exports 导出；兼容旧测试期待的注册形状
  if (!config && exported && typeof exported === "object") config = exported
  if (!config) throw new Error("页面未导出配置对象：" + route)

  const inst = {}
  Object.keys(config).forEach(function (k) { inst[k] = config[k] })
  inst.data = JSON.parse(JSON.stringify(config.data || {}))
  inst.state = Object.assign({}, inst.data)
  /** 浅合并 setData，语义与小程序一致 */
  inst.setData = function (patch) {
    Object.keys(patch).forEach(function (k) {
      const parts = k.split(".")
      let nodeData = inst.data
      let nodeState = inst.state
      for (let i = 0; i < parts.length - 1; i++) {
        const part = parts[i]
        if (typeof nodeData[part] !== "object" || nodeData[part] === null) nodeData[part] = {}
        if (typeof nodeState[part] !== "object" || nodeState[part] === null) nodeState[part] = {}
        nodeData = nodeData[part]
        nodeState = nodeState[part]
      }
      const field = parts[parts.length - 1]
      nodeData[field] = patch[k]
      nodeState[field] = patch[k]
    })
    // 主题包装器直接更新 state，随后页面 refresh() 可能只 setData 业务键；
    // 复制当前主题字段回 data，匹配小程序 setData 的单一状态存储模型。
    ;["themeClass", "isDark", "brand", "brandSoft"].forEach(function (key) {
      if (Object.prototype.hasOwnProperty.call(inst.state, key)) inst.data[key] = inst.state[key]
    })
  }
  return inst
}

/** 模拟 input / change 事件 */
function fire (page, handler, value) {
  page[handler]({ detail: { value: value } })
}

/** 模拟 click 事件（带 dataset） */
function tap (page, handler, dataset) {
  return page[handler]({ currentTarget: { dataset: dataset || {} } })
}

/**
 * 临时替换某个平台 API，返回还原函数。
 * @param {string} name
 * @param {Function} impl
 */
function mockPlatform (name, impl) {
  const platform = require("../shared/platform.js")
  const original = platform[name]
  if (name === "showModal" || name === "showActionSheet") {
    platform[name] = function (opts) {
      let result
      try { result = impl(opts) } catch (err) { return Promise.reject(err) }
      // 迁入测试仍使用小程序 callback 写法；同时返回 Promise 供 Web 页面使用。
      if (result && typeof result.then === "function") return result
      if (opts && typeof opts.success === "function") opts.success(result || { confirm: false, tapIndex: -1, content: "" })
      return Promise.resolve(result)
    }
  } else {
    platform[name] = impl
  }
  return function () { platform[name] = original }
}

/** 取某个平台 API 的最后一次调用 */
function lastCall (name) {
  for (let i = calls.length - 1; i >= 0; i--) {
    if (calls[i].name === name) return calls[i]
  }
  return null
}

/** 清空调用记录 */
function getClipboardText () { return clipboardText }

function resetCalls () {
  calls.length = 0
}


/**
 * 加载 Web 组件（目前仅 capture-sheet），回传可直接调用的方法实例。
 * @param {string} route 形如 "components/capture-sheet/index"
 */
function createComponent (route) {
  // 组件渲染依赖 document；迁入的 Node 用例没有主动安装 DOM，
  // 在边界脚手架里补上最小桩即可保持生产代码纯 Web。
  if (typeof document === "undefined") require("./dom-stub.js").install()
  const abs = path.resolve(__dirname, "..", "web", "workbench", route + ".js")
  delete require.cache[require.resolve(abs)]
  const component = require(abs)
  if (!component || typeof component.create !== "function") throw new Error("组件未导出 create()：" + route)
  const events = []
  const instance = component.create({
    onClose: function () { events.push({ name: "close" }) },
    onCreated: function (type) { events.push({ name: "created", detail: { type: type } }) }
  })
  instance.__events = events
  instance.triggerEvent = function (name, detail) { events.push({ name: name, detail: detail }) }
  instance.onPick = function (event) { return instance.pick(event.currentTarget.dataset.type) }
  instance.onClose = instance.close
  instance.noop = function () {}
  return instance
}

function lastEvent (inst, name) {
  for (let i = inst.__events.length - 1; i >= 0; i--) {
    if (inst.__events[i].name === name) return inst.__events[i]
  }
  return null
}
module.exports = {
  createPage: createPage,
  createComponent: createComponent,
  lastEvent: lastEvent,
  fire: fire,
  tap: tap,
  mockPlatform: mockPlatform,
  lastCall: lastCall,
  getClipboardText: getClipboardText,
  resetCalls: resetCalls
}
