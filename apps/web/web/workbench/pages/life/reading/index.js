/**
 * 读书
 *
 * 排序：在读 → 想读 → 搁置 → 已读。
 * 点卡片打开编辑面板，可直接改当前页码（读完自动转已读）。
 */

const ui = require('../../../../../shared/ui.js')
const platform = require('../../../../../shared/platform.js')
const { withTheme } = require('../../../../../shared/core/with-theme.js')
const stores = require('../../../../../shared/workbench/stores.js')
const constants = require('../../../../../shared/core/constants.js')
const fmt = require('../../../../../shared/core/format.js')

const STATUS_ORDER = { reading: 0, wishlist: 1, paused: 2, finished: 3 }

module.exports = withTheme({
  data: {
    title: '读书',
    items: [],
    count: 0,
    readingCount: 0,
    finishedCount: 0,

    editing: false,
    isEdit: false,
    bookId: '',
    formTitle: '',
    formAuthor: '',
    formTotal: '',
    formCurrent: '',
    formStatus: 'reading',
    formRating: 0,
    formNotes: '',
    statuses: [],
    ratingOptions: [1, 2, 3, 4, 5]
  },

  render: function (state) {
    const rows = (state.items || []).map(function (item) {
      const title = item.title || item.question || item.content || item.name || item.title || '未命名'
      const meta = [item.categoryLabel, item.language, item.statusLabel, item.time, item.difficulty].filter(Boolean).join(' · ')
      const card = ui.el('article', { class: 'wb-record-card' }, [
        ui.el('strong', { text: title }),
        ui.el('p', { class: 'wb-muted', text: meta }),
        ui.el('p', { class: 'wb-record-preview', text: item.preview || item.description || item.answer || item.code || item.content || '' }),
        ui.el('div', { class: 'wb-chip-row' }, (item.tags || []).map(function (tag) { return ui.pill(tag) }))
      ])
      if (item.id && typeof this.onOpen === 'function') card.addEventListener('click', function () { this.onOpen({ currentTarget: { dataset: { id: item.id } } }) }.bind(this))
      return card
    }.bind(this))
    if (!rows.length) rows.push(ui.empty('暂无读书', '可创建一条记录开始整理。'))
    const children = [ui.pageHead({ title: state.title || '读书', sub: '跟踪阅读进度与书单。' })]
    if (typeof this.onCreate === 'function') children.push(ui.el('button', { class: 'btn primary', text: '新建', onclick: this.onCreate.bind(this) }))
    children.push(ui.el('div', { class: 'wb-record-list' }, rows))
    if (state.editing || state.open || state.showForm) children.push(ui.panel({ title: '编辑内容', children: [ui.el('p', { class: 'wb-muted', text: '表单字段可通过下方详情页操作。' })] }))
    return ui.el('div', { class: 'wb-page wb-' }, children)
  },
  onLoad () {
    this.refresh()
  },

  onShow () {
    this.refresh()
  },

  refresh () {
    const list = stores.reading.getAll().slice()

    list.sort(function (a, b) {
      const oa = STATUS_ORDER[a.status] === undefined ? 9 : STATUS_ORDER[a.status]
      const ob = STATUS_ORDER[b.status] === undefined ? 9 : STATUS_ORDER[b.status]
      if (oa !== ob) return oa - ob
      return new Date(b.createdAt || 0).getTime() - new Date(a.createdAt || 0).getTime()
    })

    const items = list.map(function (r) {
      const status = constants.readingStatus(r.status)
      const total = r.totalPages || 0
      const current = r.currentPage || 0
      return {
        id: r.id,
        title: r.title,
        author: r.author || '',
        color: status.color,
        statusLabel: status.label,
        status: r.status,
        current: current,
        total: total,
        percent: fmt.percent(current, total),
        rating: r.rating || 0,
        progressText: total > 0 ? current + ' / ' + total + ' 页' : '未设总页数'
      }
    })

    this.setData({
      items: items,
      count: items.length,
      readingCount: items.filter(function (i) { return i.status === 'reading' }).length,
      finishedCount: items.filter(function (i) { return i.status === 'finished' }).length
    })
  },

  // ==================== 面板 ====================

  onCreate () {
    this.openPanel(null)
  },

  onOpen (e) {
    const item = stores.reading.find(e.currentTarget.dataset.id)
    if (item) this.openPanel(item)
  },

  openPanel (item) {
    const isEdit = !!item
    const status = item ? item.status : 'reading'

    this.setData({
      editing: true,
      isEdit: isEdit,
      bookId: isEdit ? item.id : '',
      formTitle: isEdit ? item.title : '',
      formAuthor: isEdit ? (item.author || '') : '',
      formTotal: isEdit && item.totalPages ? String(item.totalPages) : '',
      formCurrent: isEdit ? String(item.currentPage || 0) : '0',
      formStatus: status,
      formRating: isEdit ? (item.rating || 0) : 0,
      formNotes: isEdit ? (item.notes || '') : '',
      statuses: this.buildStatuses(status)
    })
  },

  onClosePanel () {
    this.setData({ editing: false })
  },

  buildStatuses (activeKey) {
    return constants.READING_STATUS.map(function (s) {
      return { key: s.key, label: s.label, color: s.color, active: s.key === activeKey }
    })
  },

  onFormTitle (e) { this.setData({ formTitle: e.detail.value }) },
  onFormAuthor (e) { this.setData({ formAuthor: e.detail.value }) },
  onFormTotal (e) { this.setData({ formTotal: e.detail.value }) },
  onFormCurrent (e) { this.setData({ formCurrent: e.detail.value }) },
  onFormNotes (e) { this.setData({ formNotes: e.detail.value }) },

  onFormStatus (e) {
    const key = e.currentTarget.dataset.key
    this.setData({ formStatus: key, statuses: this.buildStatuses(key) })
  },

  onFormRating (e) {
    this.setData({ formRating: Number(e.currentTarget.dataset.star) })
  },

  onSave () {
    const title = String(this.data.formTitle || '').trim()
    if (!title) {
      platform.showToast({ title: '请输入书名', icon: 'none' })
      return
    }

    const total = Number(this.data.formTotal) || 0
    let current = Number(this.data.formCurrent) || 0
    if (total > 0) current = Math.min(current, total)
    if (current < 0) current = 0

    let status = this.data.formStatus
    // 页数读满自动转已读
    if (total > 0 && current >= total) status = 'finished'

    const payload = {
      title: title,
      author: String(this.data.formAuthor || '').trim(),
      totalPages: total,
      currentPage: current,
      status: status,
      rating: this.data.formRating,
      notes: String(this.data.formNotes || '').trim()
    }

    // 读满即视为完成，无论新建还是编辑都要记完成时间
    if (status === 'finished') payload.finishedAt = new Date().toISOString()

    if (this.data.isEdit) {
      stores.reading.update(this.data.bookId, payload)
    } else {
      stores.reading.add(payload)
    }

    this.setData({ editing: false })
    this.refresh()
    platform.showToast({ title: '已保存', icon: 'success' })
  },

  onDelete () {
    if (!this.data.isEdit) return
    const self = this
    platform.showModal({
      title: '删除这本书？',
      content: '删除后无法恢复。',
      confirmText: '删除',
      confirmColor: '#E5484D',
      success: function (res) {
        if (!res.confirm) return
        stores.reading.remove(self.data.bookId)
        self.setData({ editing: false })
        self.refresh()
        platform.showToast({ title: '已删除', icon: 'success' })
      }
    })
  },

  onShareAppMessage () {
    return { title: 'DevBench · 读书', path: '/pages/life/reading/index' }
  }
})
