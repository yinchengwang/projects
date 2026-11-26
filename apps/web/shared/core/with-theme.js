"use strict"
/**
 * with-theme.js —— 页面主题包装器
 *
 * 小程序版用高阶函数包装 Page 配置（因为 Behavior 只对 Component 生效）。
 * Web 版同样用包装器统一注入主题，页面形状：
 *
 *   const { withTheme } = require('../../core/with-theme.js')
 *   export default withTheme({
 *     data: { ... },
 *     render(state) { ... }
 *   })
 *
 * 包装后页面自动获得：
 *   - state.themeClass / isDark / brand / brandSoft
 *   - rootElement：承载主题类名的根节点
 *   - refreshTheme()：手动刷新主题（如设置页切换后立即生效）
 */

const theme = require("./theme.js")
const storage = require("./storage.js")

/** 读当前设置 */
function readSettings (key) {
  const storageKey = key || storage.KEYS.settings
  return storage.get(storageKey, null) || { theme: "auto", primaryColor: "sky" }
}

/**
 * 立即把主题写入页面 state 并同步到根元素。
 * @param {object} page 页面对象（带 state 与 rootElement）
 * @returns {object} 主题数据
 */
function applyTheme (page, settingsKey) {
  const settings = readSettings(settingsKey)
  const data = theme.build(settings)
  page.state = page.state || {}
  page.state.themeClass = data.themeClass
  page.state.isDark = data.isDark
  page.state.brand = data.brand
  page.state.brandSoft = data.brandSoft
  theme.applyThemeClass(page.rootElement || (typeof document !== "undefined" ? document.body : null), data)
  return data
}

/**
 * 包装页面配置。
 * @param {object} options 页面配置
 * @returns {object} 包装后的配置
 */
function withTheme (options, settingsKey) {
  const config = Object.assign({}, options)

  config.data = Object.assign({
    themeClass: "theme-light",
    isDark: false,
    brand: "#0284C7",
    brandSoft: "rgba(2,132,199,0.12)"
  }, options.data)

  const userOnMount = options.onMount
  const userOnUnmount = options.onUnmount

  config.onMount = function (rootElement) {
    this.rootElement = rootElement || this.rootElement
    applyTheme(this, settingsKey)
    this.__bindSystemTheme()
    if (userOnMount) userOnMount.call(this, rootElement)
  }

  config.onUnmount = function () {
    if (userOnUnmount) userOnUnmount.call(this)
  }

  /** 仅在 auto 模式下响应系统主题切换 */
  config.__bindSystemTheme = function () {
    if (this.__themeBound) return
    this.__themeBound = true
    const self = this
    theme.onSystemThemeChange(function () {
      const settings = readSettings(settingsKey)
      if (settings.theme !== "auto") return
      applyTheme(self, settingsKey)
    })
  }

  /** 供页面手动刷新主题 */
  config.refreshTheme = function () {
    return applyTheme(this, settingsKey)
  }

  return config
}

module.exports = {
  withTheme: withTheme,
  applyTheme: applyTheme,
  readSettings: readSettings
}
