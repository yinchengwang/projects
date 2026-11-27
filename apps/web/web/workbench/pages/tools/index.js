/**
 * 工具箱 —— 10 个离线开发工具
 *
 * 交互：搜索过滤 → 4 列网格 → 点选展开全屏面板（输入在上、结果在下、一键复制）。
 * 除 diff 外均随输入即时计算；diff 限制单侧 2000 字符，避免 O(mn) 卡顿。
 */

const ui = require('../../../../shared/ui.js')
const platform = require('../../../../shared/platform.js')
const { withTheme } = require('../../../../shared/core/with-theme.js')
const ct = require('../../../../shared/workbench/codetools.js')

const TOOLS = [
  { id: 'json', label: 'JSON', symbol: '{}', keywords: 'json 格式化 美化 压缩 转义' },
  { id: 'base64', label: 'Base64', symbol: '64', keywords: 'base64 编码 解码' },
  { id: 'url', label: 'URL', symbol: '://', keywords: 'url 编码 解码 encode' },
  { id: 'timestamp', label: '时间戳', symbol: 'ts', keywords: '时间戳 时间 日期 转换' },
  { id: 'uuid', label: 'UUID', symbol: 'id', keywords: 'uuid 唯一 随机' },
  { id: 'regex', label: '正则', symbol: '.*', keywords: '正则 regex 匹配 测试' },
  { id: 'color', label: '颜色', symbol: '#', keywords: '颜色 hex rgb 转换' },
  { id: 'hash', label: 'Hash', symbol: 'H', keywords: 'hash 哈希 摘要 crc djb2' },
  { id: 'diff', label: 'Diff', symbol: '±', keywords: 'diff 对比 差异 比较' },
  { id: 'cron', label: 'Cron', symbol: '*', keywords: 'cron 定时 表达式 调度' }
]

const JSON_MODES = [
  { key: 'beautify', label: '美化' },
  { key: 'minify', label: '压缩' },
  { key: 'escape', label: '转义' },
  { key: 'unescape', label: '反转义' }
]

const BASE64_MODES = [
  { key: 'encode', label: '编码' },
  { key: 'decode', label: '解码' }
]

const URL_MODES = [
  { key: 'encode', label: '编码' },
  { key: 'decode', label: '解码' }
]

const UUID_COUNTS = [1, 5, 10]
const FLAG_OPTIONS = ['g', 'i', 'm', 's']
const DIFF_LIMIT = 2000

module.exports = withTheme({
  data: {
    title: '工具箱',
    query: '',
    filtered: TOOLS,

    active: '',
    activeLabel: '',
    activeSymbol: '',

    input: '',
    inputB: '',

    jsonModes: JSON_MODES,
    jsonMode: 'beautify',
    base64Modes: BASE64_MODES,
    base64Mode: 'encode',
    urlModes: URL_MODES,
    urlMode: 'encode',

    uuidCounts: UUID_COUNTS,
    uuidCount: 5,
    flagOptions: FLAG_OPTIONS,
    regexFlags: 'g',
    diffLimit: DIFF_LIMIT,

    output: '',
    error: '',
    copyText: '',

    tsNowMs: '',
    tsNowS: '',
    tsDate: '',
    tsMs: '',
    tsS: '',
    uuidList: [],
    regexSegments: [],
    regexCount: 0,
    diffOps: [],
    diffNote: '',
    colorSwatch: '',
    colorHex: '',
    colorRgb: '',
    colorRgba: '',
    hashDjb2: '',
    hashCrc32: '',
    cronDesc: '',
    cronNext: [],
    cronError: ''
  },

  render: function (state) {
    const search = ui.el('input', { class: 'wb-input', value: state.query || '', placeholder: '搜索工具：Base64、JSON、正则…' })
    search.addEventListener('input', function (e) { this.onSearch({ detail: { value: e.target.value } }) }.bind(this))
    const tools = (state.filtered || TOOLS).map(function (item) {
      const button = ui.el('button', { class: 'wb-tool-card' }, [
        ui.el('span', { class: 'wb-module-symbol', text: item.symbol }),
        ui.el('strong', { text: item.label }),
        ui.el('span', { class: 'wb-muted', text: item.keywords })
      ])
      button.addEventListener('click', function () { this.onChoose({ currentTarget: { dataset: { id: item.id } } }) }.bind(this))
      return button
    }.bind(this))
    const children = [
      ui.pageHead({ title: state.title, sub: '十类常用离线开发工具。' }),
      ui.el('div', { class: 'wb-search-row' }, [search, ui.el('button', { class: 'btn', text: '清空', onclick: this.onClearSearch.bind(this) })]),
      ui.el('div', { class: 'wb-tool-grid' }, tools)
    ]
    if (state.active) {
      const input = ui.el('textarea', { class: 'wb-textarea', placeholder: '输入待处理内容…' })
      input.value = state.input || ''
      input.addEventListener('input', function (e) { this.onInput({ detail: { value: e.target.value } }) }.bind(this))
      const output = ui.el('pre', { class: 'wb-output', text: state.output || state.copyText || state.error || '' })
      children.push(ui.panel({ title: state.activeLabel || '工具', className: 'wb-tool-panel', children: [input, output,
        ui.el('button', { class: 'btn primary', text: '复制结果', onclick: this.onCopy.bind(this) }),
        ui.el('button', { class: 'btn', text: '关闭', onclick: this.onClose.bind(this) })
      ] }))
    }
    return ui.el('div', { class: 'wb-page wb-tools' }, children)
  },
  onLoad () {
    this.refreshNow()
  },

  refreshNow () {
    this.setData({
      tsNowMs: String(Date.now()),
      tsNowS: String(Math.floor(Date.now() / 1000))
    })
  },

  // ==================== 搜索 ====================

  onSearch (e) {
    const raw = e.detail.value || ''
    const q = raw.trim().toLowerCase()
    const filtered = q
      ? TOOLS.filter(function (t) {
          return t.label.toLowerCase().indexOf(q) >= 0 ||
            t.id.indexOf(q) >= 0 ||
            t.keywords.indexOf(q) >= 0
        })
      : TOOLS
    this.setData({ query: raw, filtered: filtered })
  },

  onClearSearch () {
    this.setData({ query: '', filtered: TOOLS })
  },

  // ==================== 面板开关 ====================

  onOpen (e) {
    const id = e.currentTarget.dataset.id
    let tool = null
    for (let i = 0; i < TOOLS.length; i++) {
      if (TOOLS[i].id === id) tool = TOOLS[i]
    }
    if (!tool) return

    this.setData({
      active: id,
      activeLabel: tool.label,
      activeSymbol: tool.symbol,
      input: '',
      inputB: '',
      output: '',
      error: '',
      copyText: '',
      regexSegments: [],
      regexCount: 0,
      diffOps: [],
      diffNote: '',
      colorSwatch: '',
      colorHex: '',
      colorRgb: '',
      colorRgba: '',
      hashDjb2: '',
      hashCrc32: '',
      cronDesc: '',
      cronNext: [],
      cronError: ''
    })

    if (id === 'uuid') this.generateUuid()
    if (id === 'timestamp') this.refreshNow()
  },

  onClose () {
    this.setData({ active: '' })
  },

  // ==================== 输入与复制 ====================

  onInput (e) {
    this.setData({ input: e.detail.value })
    this.recompute()
  },

  onInputB (e) {
    this.setData({ inputB: e.detail.value })
    this.recompute()
  },

  onCopy (e) {
    const dataset = e && e.currentTarget && e.currentTarget.dataset
    const text = (dataset && dataset.text) || this.data.copyText
    if (!text) {
      platform.showToast({ title: '暂无可复制内容', icon: 'none' })
      return
    }
    platform.setClipboardData({
      data: String(text),
      success: function () {
        platform.showToast({ title: '已复制', icon: 'success' })
      }
    })
  },

  // ==================== 选项切换 ====================

  onJsonMode (e) {
    this.setData({ jsonMode: e.currentTarget.dataset.key })
    this.recompute()
  },

  onBase64Mode (e) {
    this.setData({ base64Mode: e.currentTarget.dataset.key })
    this.recompute()
  },

  onUrlMode (e) {
    this.setData({ urlMode: e.currentTarget.dataset.key })
    this.recompute()
  },

  onUuidCount (e) {
    this.setData({ uuidCount: Number(e.currentTarget.dataset.count) })
    this.generateUuid()
  },

  onToggleFlag (e) {
    const flag = e.currentTarget.dataset.flag
    let flags = this.data.regexFlags
    if (flags.indexOf(flag) >= 0) {
      flags = flags.split(flag).join('')
    } else {
      flags = flags + flag
    }
    this.setData({ regexFlags: flags })
    this.recompute()
  },

  onUseNow () {
    this.setData({ input: String(Date.now()) })
    this.recompute()
  },

  // ==================== 计算分发 ====================

  recompute () {
    const active = this.data.active
    if (!active) return
    const fn = this['run_' + active]
    if (typeof fn !== 'function') return
    try {
      fn.call(this)
    } catch (err) {
      this.setData({
        error: (err && err.message) || String(err),
        output: '',
        copyText: ''
      })
    }
  },

  /** JSON */
  run_json () {
    const src = this.data.input
    if (!src.trim()) {
      this.setData({ output: '', copyText: '', error: '' })
      return
    }
    const out = ct.jsonTransform(src, this.data.jsonMode, 2)
    this.setData({ output: String(out), copyText: String(out), error: '' })
  },

  /** Base64 */
  run_base64 () {
    const src = this.data.input
    if (!src) {
      this.setData({ output: '', copyText: '', error: '' })
      return
    }
    const out = this.data.base64Mode === 'encode' ? ct.base64Encode(src) : ct.base64Decode(src)
    this.setData({ output: out, copyText: out, error: '' })
  },

  /** URL */
  run_url () {
    const src = this.data.input
    if (!src) {
      this.setData({ output: '', copyText: '', error: '' })
      return
    }
    const out = this.data.urlMode === 'encode' ? ct.urlEncode(src) : ct.urlDecode(src)
    this.setData({ output: out, copyText: out, error: '' })
  },

  /** 时间戳：自动识别输入是时间戳还是日期字符串 */
  run_timestamp () {
    const src = this.data.input.trim()
    if (!src) {
      this.setData({ tsDate: '', tsMs: '', tsS: '', copyText: '', error: '' })
      return
    }

    let d = null
    if (/^\d+$/.test(src)) {
      d = ct.tsToDate(Number(src))
    } else {
      const parsed = new Date(src.replace(/-/g, '/'))
      if (!isNaN(parsed.getTime())) d = parsed
    }

    if (!d || isNaN(d.getTime())) {
      this.setData({
        tsDate: '', tsMs: '', tsS: '', copyText: '',
        error: '无法识别，请输入时间戳或 2026-10-22 09:00:00'
      })
      return
    }

    const ms = d.getTime()
    this.setData({
      tsDate: ct.fmtDate(d),
      tsMs: String(ms),
      tsS: String(Math.floor(ms / 1000)),
      copyText: ct.fmtDate(d),
      error: ''
    })
  },

  /** UUID */
  generateUuid () {
    const n = this.data.uuidCount
    const list = []
    for (let i = 0; i < n; i++) list.push(ct.uuid())
    this.setData({ uuidList: list, copyText: list.join('\n'), error: '' })
  },

  /** 正则 */
  run_regex () {
    const pattern = this.data.input
    const text = this.data.inputB
    if (!pattern) {
      this.setData({ regexSegments: [], regexCount: 0, copyText: '', error: '' })
      return
    }
    const r = ct.testRegex(pattern, text, this.data.regexFlags)
    this.setData({
      regexSegments: r.highlighted,
      regexCount: r.matchCount,
      error: r.error || '',
      copyText: r.matched ? '命中 ' + r.matchCount + ' 处' : '无命中'
    })
  },

  /** 颜色 */
  run_color () {
    const src = this.data.input.trim()
    if (!src) {
      this.setData({ colorSwatch: '', colorHex: '', colorRgb: '', colorRgba: '', copyText: '', error: '' })
      return
    }

    // HEX 输入
    if (src.charAt(0) === '#' || /^[0-9a-fA-F]{3,8}$/.test(src)) {
      const rgb = ct.hexToRgb(src)
      if (!rgb) {
        this.setData({
          colorSwatch: '', colorHex: '', colorRgb: '', colorRgba: '', copyText: '',
          error: 'HEX 格式不正确'
        })
        return
      }
      const hex = ct.rgbToHex(rgb)
      this.setData({
        colorSwatch: hex,
        colorHex: hex.toUpperCase(),
        colorRgb: 'rgb(' + rgb.r + ', ' + rgb.g + ', ' + rgb.b + ')',
        colorRgba: 'rgba(' + rgb.r + ', ' + rgb.g + ', ' + rgb.b + ', 1)',
        copyText: hex.toUpperCase(),
        error: ''
      })
      return
    }

    // rgb(...) 输入
    const m = src.match(/(\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d{1,3})/)
    if (m) {
      const rgb = { r: Number(m[1]), g: Number(m[2]), b: Number(m[3]) }
      const hex = ct.rgbToHex(rgb)
      this.setData({
        colorSwatch: hex,
        colorHex: hex.toUpperCase(),
        colorRgb: 'rgb(' + rgb.r + ', ' + rgb.g + ', ' + rgb.b + ')',
        colorRgba: 'rgba(' + rgb.r + ', ' + rgb.g + ', ' + rgb.b + ', 1)',
        copyText: hex.toUpperCase(),
        error: ''
      })
      return
    }

    this.setData({
      colorSwatch: '', colorHex: '', colorRgb: '', colorRgba: '', copyText: '',
      error: '请输入 #0284C7 或 rgb(2, 132, 199)'
    })
  },

  /** Hash */
  run_hash () {
    const src = this.data.input
    if (!src) {
      this.setData({ hashDjb2: '', hashCrc32: '', copyText: '', error: '' })
      return
    }
    this.setData({
      hashDjb2: ct.djb2(src),
      hashCrc32: ct.crc32(src),
      copyText: ct.djb2(src),
      error: ''
    })
  },

  /** Diff */
  run_diff () {
    const a = this.data.input
    const b = this.data.inputB

    if (a.length > DIFF_LIMIT || b.length > DIFF_LIMIT) {
      this.setData({
        diffOps: [],
        diffNote: '单侧文本请控制在 ' + DIFF_LIMIT + ' 字符内',
        copyText: '',
        error: ''
      })
      return
    }
    if (!a && !b) {
      this.setData({ diffOps: [], diffNote: '', copyText: '', error: '' })
      return
    }

    const ops = ct.diffText(a, b)
    const added = ops.filter(function (o) { return o.type === 'add' }).length
    const removed = ops.filter(function (o) { return o.type === 'del' }).length

    this.setData({
      diffOps: ops,
      diffNote: '新增 ' + added + ' 段 · 删除 ' + removed + ' 段',
      copyText: ops.map(function (o) {
        const sign = o.type === 'add' ? '+ ' : o.type === 'del' ? '- ' : '  '
        return sign + o.text
      }).join(''),
      error: ''
    })
  },

  /** Cron */
  run_cron () {
    const src = this.data.input.trim()
    if (!src) {
      this.setData({ cronDesc: '', cronNext: [], cronError: '', copyText: '', error: '' })
      return
    }
    const r = ct.describeCron(src)
    if (!r.ok) {
      this.setData({ cronDesc: '', cronNext: [], cronError: r.error, copyText: '', error: '' })
      return
    }
    this.setData({
      cronDesc: r.description,
      cronNext: r.nextRuns,
      cronError: '',
      copyText: r.nextRuns.join('\n'),
      error: ''
    })
  },

  onShareAppMessage () {
    return { title: 'DevBench 工具箱 · 10 个离线开发工具', path: '/pages/tools/index' }
  }
})
