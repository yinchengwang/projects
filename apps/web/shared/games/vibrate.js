"use strict"
/**
 * vibrate.js —— 带节流的振动反馈
 *
 * 从小程序版搬来，节流逻辑一字不改。底层从 wx.vibrateShort 换成
 * platform.vibrateShort（后者在设备不支持时静默返回，等价于原来的 try/catch）。
 */

const platform = require("../platform.js")

let lastAt = 0

/**
 * 触发短振动（默认 80ms 节流，避免连续触发）
 * @param {string} type light | medium | heavy
 */
function trigger (type) {
  const now = Date.now()
  if (now - lastAt < 80) return
  lastAt = now
  try {
    platform.vibrateShort({ type: type || "light" })
  } catch (e) {
    // 设备不支持时静默忽略
  }
}

module.exports = { trigger: trigger }
