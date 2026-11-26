/**
 * date.js —— 日期工具
 *
 * 全部输出本地时区的 YYYY-MM-DD 字符串，便于字符串比较与存储。
 */

function pad2 (n) {
  return String(n).padStart(2, '0')
}

/** 格式化为 YYYY-MM-DD */
function formatYMD (d) {
  const date = new Date(d)
  return date.getFullYear() + '-' + pad2(date.getMonth() + 1) + '-' + pad2(date.getDate())
}

/** 格式化为 YYYY-MM-DD HH:mm */
function formatYMDHM (d) {
  const date = new Date(d)
  return formatYMD(date) + ' ' + pad2(date.getHours()) + ':' + pad2(date.getMinutes())
}

/** 今天 */
function todayStr () {
  return formatYMD(new Date())
}

/** 相对时间：刚刚 / N 分钟前 / N 小时前 / N 天前 / 具体日期 */
function formatRelative (iso) {
  if (!iso) return ''
  const diff = Math.floor((Date.now() - new Date(iso).getTime()) / 1000)
  if (diff < 0) return '刚刚'
  if (diff < 60) return '刚刚'
  if (diff < 3600) return Math.floor(diff / 60) + ' 分钟前'
  if (diff < 86400) return Math.floor(diff / 3600) + ' 小时前'
  if (diff < 86400 * 7) return Math.floor(diff / 86400) + ' 天前'
  return formatYMD(iso)
}

/** 本周范围（周一 → 周日） */
function weekRange (base) {
  const d = base ? new Date(base) : new Date()
  const day = d.getDay() === 0 ? 7 : d.getDay()
  const monday = new Date(d)
  monday.setDate(d.getDate() - (day - 1))
  monday.setHours(0, 0, 0, 0)
  const sunday = new Date(monday)
  sunday.setDate(monday.getDate() + 6)
  return { start: formatYMD(monday), end: formatYMD(sunday) }
}

/** N 天前的日期字符串 */
function daysAgo (n, base) {
  const d = base ? new Date(base) : new Date()
  d.setDate(d.getDate() - n)
  return formatYMD(d)
}

/** 两个日期之间的所有日期（含首尾） */
function dateListBetween (start, end) {
  const out = []
  const cursor = new Date(start)
  const e = new Date(end)
  while (cursor <= e) {
    out.push(formatYMD(cursor))
    cursor.setDate(cursor.getDate() + 1)
  }
  return out
}

/** 人类可读日期：今天 / 昨天 / YYYY-MM-DD 周X */
function humanDay (iso) {
  const today = todayStr()
  if (iso === today) return '今天'
  if (iso === daysAgo(1)) return '昨天'
  const d = new Date(iso)
  const week = ['日', '一', '二', '三', '四', '五', '六'][d.getDay()]
  return iso + ' 周' + week
}

/** 问候语（按时段） */
function greeting () {
  const h = new Date().getHours()
  if (h < 6) return '夜深了'
  if (h < 12) return '上午好'
  if (h < 14) return '中午好'
  if (h < 18) return '下午好'
  return '晚上好'
}

/** 月日 + 星期，例如 "10月22日 周四" */
function todayLabel () {
  const d = new Date()
  const week = ['日', '一', '二', '三', '四', '五', '六'][d.getDay()]
  return (d.getMonth() + 1) + '月' + d.getDate() + '日 周' + week
}

module.exports = {
  formatYMD: formatYMD,
  formatYMDHM: formatYMDHM,
  todayStr: todayStr,
  formatRelative: formatRelative,
  weekRange: weekRange,
  daysAgo: daysAgo,
  dateListBetween: dateListBetween,
  humanDay: humanDay,
  greeting: greeting,
  todayLabel: todayLabel
}
