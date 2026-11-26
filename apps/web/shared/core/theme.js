"use strict"
/**
 * theme.js —— 主题解析
 *
 * 主题靠 class 承载 CSS 变量（不用 [data-theme]，兼容性更好）：
 *   .theme-light { --bg-primary: #F7F8FA; ... }
 *   .theme-dark  { --bg-primary: #0B0F17; ... }
 *
 * 与小程序版的差异：小程序版用 wx.setNavigationBarColor / setTabBarStyle
 * 同步原生导航栏；Web 没有原生导航栏，改为把主题类名写到根元素上，
 * 由 CSS 变量自然生效。`build()` 的返回形状保持不变，页面代码零改动。
 */

const constants = require("./constants.js")

/** 读取系统主题 */
function systemTheme () {
  try {
    if (typeof window !== "undefined" && typeof window.matchMedia === "function") {
      return window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light"
    }
    return "light"
  } catch (e) {
    return "light"
  }
}

/**
 * 把设置里的 mode 解析成实际主题
 * @param {'light'|'dark'|'auto'} mode
 * @returns {'light'|'dark'}
 */
function resolve (mode) {
  if (mode === "light" || mode === "dark") return mode
  return systemTheme()
}

/**
 * HEX → rgba 字符串
 * @param {string} hex
 * @param {number} alpha
 * @returns {string}
 */
function hexToRgba (hex, alpha) {
  const h = String(hex).replace("#", "")
  const full = h.length === 3 ? h.split("").map(function (c) { return c + c }).join("") : h
  const r = parseInt(full.slice(0, 2), 16)
  const g = parseInt(full.slice(2, 4), 16)
  const b = parseInt(full.slice(4, 6), 16)
  return "rgba(" + r + "," + g + "," + b + "," + alpha + ")"
}

/**
 * 取主色定义
 * @param {string} key
 * @returns {{key: string, label: string, light: string, dark: string}}
 */
function primaryOf (key) {
  for (let i = 0; i < constants.PRIMARY_COLORS.length; i++) {
    if (constants.PRIMARY_COLORS[i].key === key) return constants.PRIMARY_COLORS[i]
  }
  return constants.PRIMARY_COLORS[0]
}

/**
 * 根据设置计算主题数据。
 * @param {object} settings settings.get() 的结果
 * @returns {{themeClass: string, isDark: boolean, brand: string, brandSoft: string, navFront: string, navBg: string}}
 */
function build (settings) {
  const mode = (settings && settings.theme) || "auto"
  const theme = resolve(mode)
  const isDark = theme === "dark"
  const primary = primaryOf((settings && settings.primaryColor) || "sky")
  const brand = isDark ? primary.dark : primary.light

  return {
    themeClass: isDark ? "theme-dark" : "theme-light",
    isDark: isDark,
    brand: brand,
    brandSoft: hexToRgba(brand, isDark ? 0.18 : 0.12),
    navFront: isDark ? "#ffffff" : "#000000",
    navBg: isDark ? "#0B0F17" : "#F7F8FA"
  }
}

/**
 * 把主题类名与主色变量写到根元素上。
 * @param {object} rootEl 根元素（通常是 document.body）
 * @param {object} themeData build() 的结果
 */
function applyThemeClass (rootEl, themeData) {
  if (!rootEl || !themeData) return
  const classes = String(rootEl.className || "").split(/\s+/)
    .filter(function (c) { return c !== "theme-light" && c !== "theme-dark" })
  classes.push(themeData.themeClass)
  rootEl.className = classes.join(" ")
  if (rootEl.style) {
    rootEl.style.setProperty = rootEl.style.setProperty || function () {}
    rootEl.style.setProperty("--brand", themeData.brand)
    rootEl.style.setProperty("--brand-soft", themeData.brandSoft)
  }
}

/**
 * 订阅系统主题变化（仅 auto 模式需要）。
 * @param {Function} callback
 * @returns {boolean} 注册成功返回 true
 */
function onSystemThemeChange (callback) {
  try {
    if (typeof window !== "undefined" && typeof window.matchMedia === "function") {
      const mq = window.matchMedia("(prefers-color-scheme: dark)")
      if (typeof mq.addEventListener === "function") {
        mq.addEventListener("change", function () {
          callback(mq.matches ? "dark" : "light")
        })
        return true
      }
    }
    return false
  } catch (e) {
    return false
  }
}

module.exports = {
  systemTheme: systemTheme,
  resolve: resolve,
  build: build,
  hexToRgba: hexToRgba,
  primaryOf: primaryOf,
  applyThemeClass: applyThemeClass,
  onSystemThemeChange: onSystemThemeChange
}
