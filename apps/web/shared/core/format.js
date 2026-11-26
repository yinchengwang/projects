/**
 * format.js —— 格式化工具
 */

/** 限制在 [lo, hi] 区间 */
function clamp (n, lo, hi) {
  return Math.min(hi, Math.max(lo, n))
}

/** 百分比（0-100 整数） */
function percent (done, total) {
  if (!total) return 0
  return clamp(Math.round((done / total) * 100), 0, 100)
}

/** 分钟 → 人类可读，例如 "1 小时 20 分钟" */
function duration (min) {
  if (!isFinite(min) || min < 0) return '0 分钟'
  const h = Math.floor(min / 60)
  const m = Math.round(min % 60)
  if (h === 0) return m + ' 分钟'
  if (m === 0) return h + ' 小时'
  return h + ' 小时 ' + m + ' 分钟'
}

/** 字节数 → 人类可读 */
function bytes (n) {
  if (!isFinite(n) || n < 0) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let i = 0
  let v = n
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024
    i += 1
  }
  return v.toFixed(v < 10 && i > 0 ? 2 : 0) + ' ' + units[i]
}

/** 超长截断 */
function truncate (str, n) {
  if (!str) return ''
  return str.length > n ? str.slice(0, n) + '…' : str
}

module.exports = {
  clamp: clamp,
  percent: percent,
  duration: duration,
  bytes: bytes,
  truncate: truncate
}
