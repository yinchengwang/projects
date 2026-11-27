"use strict"
/**
 * kh-harness.js —— knowledge-hub 页面加载脚手架
 *
 * 对应小程序版 tests/page-harness.js：
 *   - 加载真实页面模块，构造可直接调用生命周期的实例
 *   - setData 支持 'a.b' 点路径（knowledge-hub 有 12 处这种写法）
 *   - 交互层从 wx.* 换成 platform.*
 *
 * 与小程序版的两处适配（只影响测试侧的观测口径，不改任何断言）：
 *   1. 页面在浏览器里必须传完整路径 `/knowledge/pages/...`（platform.navigateTo
 *      会把 `/pages/` 开头补成 `/workbench`，那是 workbench 的约定）；
 *      测试断言的却是模块内路径 `/pages/...`，所以 mock 收到的路径会剥掉
 *      `/knowledge` 前缀，保持断言原文不变。
 *   2. 小程序版 mock 用 `opts.success(...)` 回调风格；这里统一按 Promise 风格
 *      记录与返回，页面代码只认 Promise。
 */

const path = require("path")

/** 平台层调用记录 */
const calls = []

/** 小程序原始路由前缀（测试断言的形态） */
const MODULE_PREFIX = "/knowledge"

/**
 * 取小程序形态的路径：剥掉 /knowledge 前缀。
 * @param {string} url
 * @returns {string}
 */
function toMiniProgramPath (url) {
  const text = String(url === undefined || url === null ? "" : url)
  if (text.indexOf(MODULE_PREFIX + "/") === 0) return text.slice(MODULE_PREFIX.length)
  return text
}

/** 平台层替身：包一层记录器，避免真的去碰 DOM */
function installPlatformStub () {
  const platform = require("../shared/platform.js")
  const names = [
    "showToast", "showModal", "showActionSheet", "navigateTo", "navigateBack", "switchTab",
    "showLoading", "hideLoading", "setClipboardData", "getClipboardData", "vibrateShort",
    "chooseMedia", "previewImage", "stopPullDownRefresh"
  ]

  names.forEach(function (name) {
    const original = platform[name]
    platform[name] = function () {
      const args = Array.prototype.slice.call(arguments)
      calls.push({ name: name, args: args, opts: args[0] })
      return original.apply(platform, args)
    }
  })

  // 少量页面需要的交互，默认给一个安静的实现（Node 下没有 DOM 浮层）
  const defaults = {
    showToast: function () { return null },
    showModal: function () { return Promise.resolve({ confirm: false, content: "" }) },
    showActionSheet: function () { return Promise.resolve({ tapIndex: -1 }) },
    navigateTo: function () {},
    navigateBack: function () {},
    switchTab: function () {},
    showLoading: function () { return null },
    hideLoading: function () {},
    setClipboardData: function () { return Promise.resolve() },
    getClipboardData: function () { return Promise.resolve({ data: "" }) },
    vibrateShort: function () { return false },
    chooseMedia: function () { return Promise.reject(new Error("当前环境不支持选择文件")) },
    previewImage: function () { return null },
    stopPullDownRefresh: function () {}
  }

  Object.keys(defaults).forEach(function (name) {
    const impl = defaults[name]
    platform[name] = function () {
      const args = Array.prototype.slice.call(arguments)
      calls.push({ name: name, args: args, opts: args[0] })
      return impl.apply(null, args)
    }
  })

  global.__KH_PLATFORM_STUB__ = platform
  return platform
}

installPlatformStub()

/**
 * 加载页面并返回可调用实例。
 * @param {string} route 形如 "pages/dashboard/index"
 */
function createPage (route) {
  const abs = path.resolve(__dirname, "..", "web", "knowledge", route + ".js")
  delete require.cache[require.resolve(abs)]
  const config = require(abs)
  if (!config || typeof config !== "object") throw new Error("页面未导出配置对象：" + route)
  if (typeof config.render !== "function") throw new Error("页面缺少 render()：" + route)

  const inst = {}
  Object.keys(config).forEach(function (k) { inst[k] = config[k] })
  inst.data = JSON.parse(JSON.stringify(config.data || {}))
  inst.state = Object.assign({}, inst.data)

  /**
   * 浅合并 setData，支持 'a.b' 形式的路径（与小程序一致）。
   * 未知中间层自动补对象。
   */
  inst.setData = function (patch) {
    Object.keys(patch).forEach(function (k) {
      const parts = k.split(".")
      const field = parts[parts.length - 1]
      let nodeData = inst.data
      let nodeState = inst.state
      for (let i = 0; i < parts.length - 1; i++) {
        const part = parts[i]
        if (typeof nodeData[part] !== "object" || nodeData[part] === null) nodeData[part] = {}
        if (typeof nodeState[part] !== "object" || nodeState[part] === null) nodeState[part] = {}
        nodeData = nodeData[part]
        nodeState = nodeState[part]
      }
      nodeData[field] = patch[k]
      nodeState[field] = patch[k]
    })
    // 主题包装器直接写 state（withTheme 的 applyTheme）；复制回 data，
    // 与小程序「单一状态存储」的语义一致。
    ;["themeClass", "isDark", "brand", "brandSoft"].forEach(function (key) {
      if (Object.prototype.hasOwnProperty.call(inst.state, key)) inst.data[key] = inst.state[key]
    })
  }

  return inst
}

/**
 * 加载组件并返回可调用实例。
 *
 * 组件是**工厂函数**形态（create(props)），不是小程序的 Component() ——
 * 小程序那套依赖框架注入，Web 侧没有，硬造不值当。
 *
 * @param {string} route 形如 "components/knowledge-graph"
 * @param {object} [props] 组件属性
 */
function createComponent (route, props) {
  const abs = path.resolve(__dirname, "..", "web", "knowledge", route + ".js")
  delete require.cache[require.resolve(abs)]
  const mod = require(abs)
  if (!mod || typeof mod.create !== "function") throw new Error("组件未导出 create()：" + route)
  return mod.create(props || {})
}

/** 取某个事件的最后一次触发（保留 workbench 脚手架的调用形状） */
function lastEvent (inst, name) {
  const events = inst.__events || []
  for (let i = events.length - 1; i >= 0; i--) {
    if (events[i].name === name) return events[i]
  }
  return null
}

/** 模拟 input / change 事件 */
function fire (page, handler, value) {
  return page[handler]({ detail: { value: value } })
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

  platform[name] = function () {
    const args = Array.prototype.slice.call(arguments)
    const shown = args.slice()
    if (name === "navigateTo" || name === "switchTab") shown[0] = toMiniProgramPath(args[0])
    calls.push({ name: name, args: args, opts: args[0] })

    let result
    try {
      result = impl.apply(null, shown)
    } catch (err) {
      return Promise.reject(err)
    }
    // 对话框类 API 在小程序版是回调风格；桩里允许返回字面量，统一兜成 Promise。
    if (name === "showModal" || name === "showActionSheet") {
      if (result && typeof result.then === "function") return result
      if (result === undefined) {
        return Promise.resolve(name === "showModal"
          ? { confirm: false, content: "" }
          : { tapIndex: -1 })
      }
      return Promise.resolve(result)
    }
    return result
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

/** 取某个平台 API 最后一次调用时收到的第一个参数（跳转类取路径） */
function lastCallArg (name) {
  const call = lastCall(name)
  if (!call) return null
  const first = call.args[0]
  if (name === "navigateTo" || name === "switchTab") return toMiniProgramPath(first)
  return first
}

/** 清空调用记录 */
function resetCalls () {
  calls.length = 0
}

module.exports = {
  createPage: createPage,
  createComponent: createComponent,
  lastEvent: lastEvent,
  fire: fire,
  tap: tap,
  mockPlatform: mockPlatform,
  lastCall: lastCall,
  lastCallArg: lastCallArg,
  resetCalls: resetCalls
}
