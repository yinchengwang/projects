/**
 * codetools.js —— 十个开发工具的纯函数实现
 *
 * 全部无副作用、不依赖 wx API，可在 Node 下直接单测。
 * 与原 Taro 版 src/utils/codetools.ts 功能等价，差异：
 *   - Base64 改为纯 JS 实现（原生小程序没有 btoa / atob）
 *   - 去掉 TS 类型标注
 *
 * Markdown 解析已抽到 md2html.js（knowledge-hub 也要用，不依赖工具箱）。
 */

const md2htmlModule = require('../core/md2html.js')
const md2html = md2htmlModule.md2html

// ==================== Base64（纯 JS，UTF-8 安全） ====================

const B64_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'

/** 字符串 → UTF-8 字节数组 */
function utf8ToBytes (str) {
  const bytes = []
  for (let i = 0; i < str.length; i++) {
    let code = str.charCodeAt(i)
    if (code < 0x80) {
      bytes.push(code)
    } else if (code < 0x800) {
      bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f))
    } else if (code >= 0xd800 && code <= 0xdbff) {
      // 代理对 → 4 字节
      const next = str.charCodeAt(++i)
      const cp = 0x10000 + ((code - 0xd800) << 10) + (next - 0xdc00)
      bytes.push(
        0xf0 | (cp >> 18),
        0x80 | ((cp >> 12) & 0x3f),
        0x80 | ((cp >> 6) & 0x3f),
        0x80 | (cp & 0x3f)
      )
    } else {
      bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f))
    }
  }
  return bytes
}

/** UTF-8 字节数组 → 字符串 */
function bytesToUtf8 (bytes) {
  let out = ''
  for (let i = 0; i < bytes.length;) {
    const b = bytes[i]
    if (b < 0x80) {
      out += String.fromCharCode(b)
      i += 1
    } else if (b < 0xe0) {
      out += String.fromCharCode(((b & 0x1f) << 6) | (bytes[i + 1] & 0x3f))
      i += 2
    } else if (b < 0xf0) {
      out += String.fromCharCode(
        ((b & 0x0f) << 12) | ((bytes[i + 1] & 0x3f) << 6) | (bytes[i + 2] & 0x3f)
      )
      i += 3
    } else {
      const cp =
        ((b & 0x07) << 18) |
        ((bytes[i + 1] & 0x3f) << 12) |
        ((bytes[i + 2] & 0x3f) << 6) |
        (bytes[i + 3] & 0x3f)
      const v = cp - 0x10000
      out += String.fromCharCode(0xd800 + (v >> 10), 0xdc00 + (v & 0x3ff))
      i += 4
    }
  }
  return out
}

/** 字节数组 → base64 字符串 */
function bytesToBase64 (bytes) {
  let out = ''
  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i]
    const b1 = bytes[i + 1]
    const b2 = bytes[i + 2]
    out += B64_CHARS[b0 >> 2]
    out += B64_CHARS[((b0 & 0x03) << 4) | ((b1 === undefined ? 0 : b1) >> 4)]
    out += b1 === undefined ? '=' : B64_CHARS[((b1 & 0x0f) << 2) | ((b2 === undefined ? 0 : b2) >> 6)]
    out += b2 === undefined ? '=' : B64_CHARS[b2 & 0x3f]
  }
  return out
}

/** base64 字符串 → 字节数组 */
function base64ToBytes (str) {
  const clean = String(str).replace(/[^A-Za-z0-9+/]/g, '')
  const bytes = []
  for (let i = 0; i < clean.length; i += 4) {
    const c0 = B64_CHARS.indexOf(clean[i])
    const c1 = B64_CHARS.indexOf(clean[i + 1])
    const c2 = B64_CHARS.indexOf(clean[i + 2])
    const c3 = B64_CHARS.indexOf(clean[i + 3])
    if (c0 < 0 || c1 < 0) break
    bytes.push((c0 << 2) | (c1 >> 4))
    if (c2 >= 0) bytes.push(((c1 & 0x0f) << 4) | (c2 >> 2))
    if (c3 >= 0) bytes.push(((c2 & 0x03) << 6) | c3)
  }
  return bytes
}

/** 字符串 → base64 */
function base64Encode (s) {
  return bytesToBase64(utf8ToBytes(String(s)))
}

/** base64 → 字符串（非法输入时尽力还原） */
function base64Decode (s) {
  try {
    return bytesToUtf8(base64ToBytes(s))
  } catch (e) {
    return ''
  }
}

// ==================== URL ====================

function urlEncode (s) {
  return encodeURIComponent(s)
}

function urlDecode (s) {
  try {
    return decodeURIComponent(s)
  } catch (e) {
    return s
  }
}

// ==================== JSON ====================

/**
 * JSON 转换
 * @param {string} input
 * @param {'beautify'|'minify'|'escape'|'unescape'} mode
 * @param {number} indent 缩进空格数
 */
function jsonTransform (input, mode, indent) {
  if (!input || !input.trim()) return ''
  const n = indent === undefined ? 2 : indent
  switch (mode) {
    case 'beautify':
      return JSON.stringify(JSON.parse(input), null, n)
    case 'minify':
      return JSON.stringify(JSON.parse(input))
    case 'escape':
      return JSON.stringify(input)
    case 'unescape':
      return JSON.parse(input)
    default:
      return input
  }
}

// ==================== 时间戳 ====================

function tsNow (unit) {
  return unit === 's' ? Math.floor(Date.now() / 1000) : Date.now()
}

/** 时间戳（秒或毫秒，自动判别）→ Date */
function tsToDate (input) {
  const n = typeof input === 'string' ? parseInt(input, 10) : input
  return new Date(n < 1e12 ? n * 1000 : n)
}

/** 日期字符串 → 时间戳 */
function dateToTs (input, unit) {
  const ms = new Date(input).getTime()
  return unit === 's' ? Math.floor(ms / 1000) : ms
}

/**
 * 按 pattern 格式化日期
 * 支持 YYYY / MM / DD / HH / mm / ss
 */
function fmtDate (input, pattern) {
  const d = new Date(input)
  const p = pattern || 'YYYY-MM-DD HH:mm:ss'
  const pad = n => String(n).padStart(2, '0')
  return p
    .replace('YYYY', String(d.getFullYear()))
    .replace('MM', pad(d.getMonth() + 1))
    .replace('DD', pad(d.getDate()))
    .replace('HH', pad(d.getHours()))
    .replace('mm', pad(d.getMinutes()))
    .replace('ss', pad(d.getSeconds()))
}

// ==================== UUID / 随机串 ====================

/** RFC4122 v4 格式 UUID */
function uuid () {
  return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function (c) {
    const r = (Math.random() * 16) | 0
    const v = c === 'x' ? r : (r & 0x3) | 0x8
    return v.toString(16)
  })
}

const CHARSETS = {
  lower: 'abcdefghijklmnopqrstuvwxyz',
  upper: 'ABCDEFGHIJKLMNOPQRSTUVWXYZ',
  digit: '0123456789',
  alphanumeric: 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789',
  strong: '!@#$%^&*()-_=+[]{}<>?,.'
}

/**
 * 随机字符串
 * @param {number} len
 * @param {'lower'|'upper'|'digit'|'alphanumeric'|'strong'} charset
 */
function randomString (len, charset) {
  const pool = charset === 'strong'
    ? CHARSETS.alphanumeric + CHARSETS.strong
    : (CHARSETS[charset] || CHARSETS.alphanumeric)
  let s = ''
  for (let i = 0; i < len; i++) s += pool[Math.floor(Math.random() * pool.length)]
  return s
}

// ==================== 颜色 ====================

/**
 * HEX → RGB
 * @returns {{r:number,g:number,b:number,a?:number}|null}
 */
function hexToRgb (hex) {
  let h = String(hex).trim().replace('#', '')
  if (h.length === 3) h = h.split('').map(c => c + c).join('')
  if (h.length !== 6 && h.length !== 8) return null
  const r = parseInt(h.slice(0, 2), 16)
  const g = parseInt(h.slice(2, 4), 16)
  const b = parseInt(h.slice(4, 6), 16)
  if (isNaN(r) || isNaN(g) || isNaN(b)) return null
  const out = { r: r, g: g, b: b }
  if (h.length === 8) out.a = parseInt(h.slice(6, 8), 16) / 255
  return out
}

/** RGB → HEX（自动裁剪到 0-255） */
function rgbToHex (rgb) {
  const to2 = n => Math.max(0, Math.min(255, Math.round(n))).toString(16).padStart(2, '0')
  let hex = '#' + to2(rgb.r) + to2(rgb.g) + to2(rgb.b)
  if (rgb.a !== undefined) hex += to2(rgb.a * 255)
  return hex
}

// ==================== 正则 ====================

/**
 * 正则测试 + 高亮分段
 * @returns {{matched:boolean, matchCount:number, groups:object, groupsArr:string[], error?:string, highlighted:Array<{value:string,hit:boolean}>}}
 */
function testRegex (pattern, input, flags) {
  const f = flags === undefined ? 'g' : flags
  const highlighted = []

  if (!pattern) {
    highlighted.push({ value: input, hit: false })
    return { matched: false, matchCount: 0, groups: {}, groupsArr: [], highlighted: highlighted }
  }

  let re
  try {
    re = new RegExp(pattern, f)
  } catch (err) {
    highlighted.push({ value: input, hit: false })
    return {
      matched: false,
      matchCount: 0,
      groups: {},
      groupsArr: [],
      highlighted: highlighted,
      error: err.message || String(err)
    }
  }

  let matched = false
  let matchCount = 0
  let firstMatch = null

  if (f.indexOf('g') >= 0) {
    let cursor = 0
    while (cursor < input.length) {
      re.lastIndex = cursor
      const exec = re.exec(input)
      if (!exec) break
      matched = true
      matchCount += 1
      if (!firstMatch) firstMatch = exec
      if (exec.index === re.lastIndex) re.lastIndex += 1
      const start = exec.index
      const end = start + exec[0].length
      if (start > cursor) highlighted.push({ value: input.slice(cursor, start), hit: false })
      highlighted.push({ value: input.slice(start, end), hit: true })
      cursor = end
    }
    if (cursor < input.length) highlighted.push({ value: input.slice(cursor), hit: false })
  } else {
    const exec = re.exec(input)
    matched = !!exec
    if (exec) {
      matchCount = 1
      firstMatch = exec
      const start = exec.index
      const end = start + exec[0].length
      if (start > 0) highlighted.push({ value: input.slice(0, start), hit: false })
      highlighted.push({ value: input.slice(start, end), hit: true })
      if (end < input.length) highlighted.push({ value: input.slice(end), hit: false })
    } else {
      highlighted.push({ value: input, hit: false })
    }
  }

  return {
    matched: matched,
    matchCount: matchCount,
    groups: (firstMatch && firstMatch.groups) || {},
    groupsArr: firstMatch ? Array.prototype.slice.call(firstMatch, 1) : [],
    highlighted: highlighted
  }
}

// ==================== Hash ====================

/** djb2 哈希（8 位十六进制） */
function djb2 (s) {
  let h = 5381
  for (let i = 0; i < s.length; i++) {
    h = ((h << 5) + h + s.charCodeAt(i)) >>> 0
  }
  return h.toString(16).padStart(8, '0')
}

const CRC_TABLE = (function () {
  const t = []
  for (let n = 0; n < 256; n++) {
    let c = n
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1)
    t[n] = c >>> 0
  }
  return t
})()

/** CRC32（8 位十六进制） */
function crc32 (s) {
  let c = 0xffffffff
  for (let i = 0; i < s.length; i++) {
    c = CRC_TABLE[(c ^ s.charCodeAt(i)) & 0xff] ^ (c >>> 8)
  }
  return ((c ^ 0xffffffff) >>> 0).toString(16).padStart(8, '0')
}

// ==================== 文本 Diff ====================

/**
 * 基于 LCS 的逐字符 diff
 * @returns {Array<{type:'eq'|'add'|'del', text:string}>}
 */
function diffText (a, b) {
  const m = a.length
  const n = b.length

  // dp[i][j] = a[i..] 与 b[j..] 的 LCS 长度
  const dp = []
  for (let i = 0; i <= m; i++) {
    const row = []
    for (let j = 0; j <= n; j++) row.push(0)
    dp.push(row)
  }
  for (let i = m - 1; i >= 0; i--) {
    for (let j = n - 1; j >= 0; j--) {
      dp[i][j] = a[i] === b[j]
        ? dp[i + 1][j + 1] + 1
        : Math.max(dp[i + 1][j], dp[i][j + 1])
    }
  }

  const out = []
  let i = 0
  let j = 0
  while (i < m && j < n) {
    if (a[i] === b[j]) {
      const buf = []
      while (i < m && j < n && a[i] === b[j]) {
        buf.push(a[i])
        i += 1
        j += 1
      }
      out.push({ type: 'eq', text: buf.join('') })
    } else if (dp[i + 1][j] >= dp[i][j + 1]) {
      out.push({ type: 'del', text: a[i] })
      i += 1
    } else {
      out.push({ type: 'add', text: b[j] })
      j += 1
    }
  }
  if (i < m) out.push({ type: 'del', text: a.slice(i) })
  if (j < n) out.push({ type: 'add', text: b.slice(j) })
  return out
}

// ==================== Cron ====================

const CRON_FIELDS = [
  { min: 0, max: 59 }, // 分
  { min: 0, max: 23 }, // 时
  { min: 1, max: 31 }, // 日
  { min: 1, max: 12 }, // 月
  { min: 0, max: 6 }   // 周
]

function tokenValid (t, min, max) {
  return t.split(',').every(function (tk) {
    if (tk === '*') return true

    const stepMatch = tk.match(/^(.+)\/(\d+)$/)
    if (stepMatch) {
      const range = stepMatch[1]
      const step = stepMatch[2]
      if (parseInt(step, 10) < 1) return false
      if (range === '*') return true
      const segs = range.split('-').map(Number)
      if (segs.length === 1) return segs[0] >= min && segs[0] <= max
      if (segs.length === 2) return segs[0] >= min && segs[1] <= max && segs[0] <= segs[1]
      return false
    }

    const segs = tk.split('-').map(Number)
    if (segs.length === 1) return segs[0] >= min && segs[0] <= max
    if (segs.length === 2) return segs[0] >= min && segs[1] <= max && segs[0] <= segs[1]
    return false
  })
}

function matchToken (t, value) {
  return t.split(',').some(function (tk) {
    if (tk === '*') return true

    const stepMatch = tk.match(/^(.+)\/(\d+)$/)
    if (stepMatch) {
      const range = stepMatch[1]
      const sN = parseInt(stepMatch[2], 10)
      if (range === '*') return value % sN === 0
      if (range.indexOf('-') >= 0) {
        const segs = range.split('-').map(Number)
        const inRange = value >= segs[0] && value <= segs[1]
        return inRange && (value - segs[0]) % sN === 0
      }
    }

    if (tk.indexOf('-') >= 0) {
      const segs = tk.split('-').map(Number)
      return value >= segs[0] && value <= segs[1]
    }
    return value === parseInt(tk, 10)
  })
}

/**
 * 解析 5 段 cron，返回接下来 5 次触发时间
 * @returns {{ok:boolean, description?:string, nextRuns?:string[], error?:string}}
 */
function describeCron (expr) {
  const parts = String(expr).trim().split(/\s+/)
  if (parts.length !== 5) {
    return { ok: false, error: 'cron 需要 5 段：分 时 日 月 周' }
  }
  for (let i = 0; i < 5; i++) {
    if (!tokenValid(parts[i], CRON_FIELDS[i].min, CRON_FIELDS[i].max)) {
      return { ok: false, error: '第 ' + (i + 1) + ' 段格式或范围不正确' }
    }
  }

  const nextRuns = []
  const start = new Date()
  let cursor = new Date(Math.floor((start.getTime() + 60000) / 60000) * 60000)
  let safety = 0

  while (nextRuns.length < 5 && safety < 60000) {
    safety += 1
    const hit = matchToken(parts[0], cursor.getMinutes()) &&
      matchToken(parts[1], cursor.getHours()) &&
      matchToken(parts[2], cursor.getDate()) &&
      matchToken(parts[3], cursor.getMonth() + 1) &&
      matchToken(parts[4], cursor.getDay())
    if (hit) nextRuns.push(fmtDate(cursor))
    cursor = new Date(cursor.getTime() + 60000)
  }

  return { ok: true, description: parts.join(' '), nextRuns: nextRuns }
}

module.exports = {
  base64Encode: base64Encode,
  base64Decode: base64Decode,
  urlEncode: urlEncode,
  urlDecode: urlDecode,
  jsonTransform: jsonTransform,
  tsNow: tsNow,
  tsToDate: tsToDate,
  dateToTs: dateToTs,
  fmtDate: fmtDate,
  uuid: uuid,
  randomString: randomString,
  hexToRgb: hexToRgb,
  rgbToHex: rgbToHex,
  testRegex: testRegex,
  djb2: djb2,
  crc32: crc32,
  diffText: diffText,
  describeCron: describeCron,
  md2html: md2html
}
