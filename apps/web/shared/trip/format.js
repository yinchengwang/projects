"use strict"
/**
 * format.js —— 日期与字符串格式化
 *
 * 从小程序版 apps/trip-log/utils/format.js 搬来，逻辑一字未改
 *（只是从 2 空格缩进的双引号风格统一成本仓的写法）。
 */

function pad2 (n) {
  return n < 10 ? "0" + n : "" + n
}

function todayISO () {
  const d = new Date()
  return d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate())
}

function ymdPretty (iso) {
  if (!iso) return ""
  const parts = iso.split("-")
  return parts[0] + "年" + parseInt(parts[1], 10) + "月" + parseInt(parts[2], 10) + "日"
}

function ymdShort (iso) {
  if (!iso) return ""
  const parts = iso.split("-")
  return parts[0] + "." + parseInt(parts[1], 10) + "." + parseInt(parts[2], 10)
}

function groupKey (iso) {
  if (!iso) return "未知时间"
  const parts = iso.split("-")
  return parts[0] + "-" + parts[1]
}

function groupLabel (iso) {
  if (!iso) return "未知时间"
  const parts = iso.split("-")
  return parts[0] + "年" + parseInt(parts[1], 10) + "月"
}

module.exports = {
  pad2: pad2,
  todayISO: todayISO,
  ymdPretty: ymdPretty,
  ymdShort: ymdShort,
  groupKey: groupKey,
  groupLabel: groupLabel
}
