/**
 * 技术笔记 · 详情 / 编辑
 *
 * 带 id 为编辑，不带为新建。内容支持 Markdown，预览用 rich-text。
 */

const ui = require('../../../../../shared/ui.js')
const platform = require('../../../../../shared/platform.js')
const { withTheme } = require('../../../../../shared/core/with-theme.js')
const stores = require('../../../../../shared/workbench/stores.js')
const constants = require('../../../../../shared/core/constants.js')
const md = require('../../../../../shared/core/md.js')

module.exports = withTheme({
  data: {
    title: '笔记',
    isEdit: false,
    noteId: '',

    formTitle: '',
    content: '',
    category: 'frontend',
    tagsText: '',
    categories: [],

    preview: false,
    html: '',
    saving: false
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
    if (!rows.length) rows.push(ui.empty('暂无健身', '可创建一条记录开始整理。'))
    const children = [ui.pageHead({ title: state.title || '健身', sub: '记录训练项目与时长。' })]
    if (typeof this.onCreate === 'function') children.push(ui.el('button', { class: 'btn primary', text: '新建', onclick: this.onCreate.bind(this) }))
    children.push(ui.el('div', { class: 'wb-record-list' }, rows))
    if (state.editing || state.open || state.showForm) children.push(ui.panel({ title: '编辑内容', children: [ui.el('p', { class: 'wb-muted', text: '表单字段可通过下方详情页操作。' })] }))
    return ui.el('div', { class: 'wb-page wb-' }, children)
  },
  onLoad (query) {
    const id = (query && query.id) || ''
    const isEdit = !!id

    let note = null
    if (isEdit) note = stores.techNotes.find(id)

    this.setData({
      isEdit: isEdit,
      noteId: id,
      formTitle: note ? note.title : '',
      content: note ? note.content : '## 新笔记\n\n在这里书写你的想法…',
      category: note ? note.category : 'frontend',
      tagsText: note ? (note.tags || []).join(', ') : '',
      categories: this.buildCategories(note ? note.category : 'frontend')
    })
  },

  buildCategories (activeKey) {
    return constants.TECH_CATEGORIES.map(function (c) {
      return {
        key: c.key,
        label: c.label,
        color: c.color,
        active: c.key === activeKey
      }
    })
  },

  onTitle (e) {
    this.setData({ formTitle: e.detail.value })
  },

  onContent (e) {
    this.setData({ content: e.detail.value })
    if (this.data.preview) this.renderPreview()
  },

  onTags (e) {
    this.setData({ tagsText: e.detail.value })
  },

  onCategory (e) {
    const key = e.currentTarget.dataset.key
    this.setData({
      category: key,
      categories: this.buildCategories(key)
    })
  },

  onTogglePreview () {
    const next = !this.data.preview
    this.setData({ preview: next })
    if (next) this.renderPreview()
  },

  renderPreview () {
    this.setData({ html: md.render(this.data.content, this.data.isDark) })
  },

  /** 解析标签：支持逗号、顿号、空格分隔，自动去掉 # 前缀 */
  parseTags (text) {
    return String(text || '')
      .split(/[,，、\s]+/)
      .map(function (t) { return t.trim().replace(/^#/, '') })
      .filter(function (t) { return !!t })
  },

  onSave () {
    if (this.data.saving) return

    const title = String(this.data.formTitle || '').trim()
    const content = String(this.data.content || '').trim()

    if (!title) {
      platform.showToast({ title: '请输入标题', icon: 'none' })
      return
    }
    if (!content) {
      platform.showToast({ title: '请输入内容', icon: 'none' })
      return
    }

    this.setData({ saving: true })

    const tags = this.parseTags(this.data.tagsText)
    const payload = {
      title: title,
      content: content,
      category: this.data.category,
      tags: tags,
      updatedAt: new Date().toISOString()
    }

    if (this.data.isEdit) {
      stores.techNotes.update(this.data.noteId, payload)
    } else {
      stores.techNotes.add(Object.assign({ pinned: false }, payload))
    }

    platform.showToast({ title: '已保存', icon: 'success' })
    setTimeout(function () { platform.navigateBack() }, 400)
  },

  onDelete () {
    if (!this.data.isEdit) return
    const self = this
    platform.showModal({
      title: '删除笔记？',
      content: '删除后无法恢复。',
      confirmText: '删除',
      confirmColor: '#E5484D',
      success: function (res) {
        if (!res.confirm) return
        stores.techNotes.remove(self.data.noteId)
        platform.showToast({ title: '已删除', icon: 'success' })
        setTimeout(function () { platform.navigateBack() }, 300)
      }
    })
  },

  onShareAppMessage () {
    return { title: this.data.formTitle || '技术笔记', path: '/pages/tech/notes/index' }
  }
})
