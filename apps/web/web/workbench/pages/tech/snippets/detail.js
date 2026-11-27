/**
 * 代码片段 · 详情 / 编辑
 */

const ui = require('../../../../../shared/ui.js')
const platform = require('../../../../../shared/platform.js')
const { withTheme } = require('../../../../../shared/core/with-theme.js')
const stores = require('../../../../../shared/workbench/stores.js')
const constants = require('../../../../../shared/core/constants.js')

module.exports = withTheme({
  data: {
    title: '片段',
    isEdit: false,
    snippetId: '',

    formTitle: '',
    language: 'typescript',
    description: '',
    code: '',
    tagsText: '',
    favorite: false,
    languages: [],
    lineCount: 1
  },

  render: function (state) {
    const rows = (state.items || []).map(function (item) {
      const title = item.title || item.question || item.content || item.name || item.date || '未命名'
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
    if (!rows.length) rows.push(ui.empty('暂无心情', '可创建一条记录开始整理。'))
    const children = [ui.pageHead({ title: state.title || '心情', sub: '回顾每日心情记录。' })]
    if (typeof this.onCreate === 'function') children.push(ui.el('button', { class: 'btn primary', text: '新建', onclick: this.onCreate.bind(this) }))
    children.push(ui.el('div', { class: 'wb-record-list' }, rows))
    if (state.editing || state.open || state.showForm) children.push(ui.panel({ title: '编辑内容', children: [ui.el('p', { class: 'wb-muted', text: '表单字段可通过下方详情页操作。' })] }))
    return ui.el('div', { class: 'wb-page wb-' }, children)
  },
  onLoad (query) {
    const id = (query && query.id) || ''
    const isEdit = !!id
    const item = isEdit ? stores.snippets.find(id) : null

    this.setData({
      isEdit: isEdit,
      snippetId: id,
      formTitle: item ? item.title : '',
      language: item ? item.language : 'typescript',
      description: item ? (item.description || '') : '',
      code: item ? item.code : '',
      tagsText: item ? (item.tags || []).join(', ') : '',
      favorite: item ? !!item.favorite : false,
      languages: this.buildLanguages(item ? item.language : 'typescript'),
      lineCount: this.countLines(item ? item.code : '')
    })

    // Web 页面标题由模块页头渲染，无小程序原生导航栏需要同步。
  },

  buildLanguages (activeKey) {
    return constants.LANGUAGES.map(function (l) {
      return {
        key: l.key,
        label: l.label,
        color: l.color,
        active: l.key === activeKey
      }
    })
  },

  countLines (code) {
    return String(code || '').split('\n').length
  },

  onTitle (e) {
    this.setData({ formTitle: e.detail.value })
  },

  onDescription (e) {
    this.setData({ description: e.detail.value })
  },

  onCode (e) {
    const code = e.detail.value
    this.setData({ code: code, lineCount: this.countLines(code) })
  },

  onTags (e) {
    this.setData({ tagsText: e.detail.value })
  },

  onLanguage (e) {
    const key = e.currentTarget.dataset.key
    this.setData({
      language: key,
      languages: this.buildLanguages(key)
    })
  },

  onToggleFavorite () {
    this.setData({ favorite: !this.data.favorite })
  },

  onCopyCode () {
    const code = this.data.code
    if (!code) {
      platform.showToast({ title: '没有可复制的代码', icon: 'none' })
      return
    }
    platform.setClipboardData({ data: code }).then(function () {
      platform.showToast({ title: '已复制', icon: 'success' })
    })
  },

  parseTags (text) {
    return String(text || '')
      .split(/[,，、\s]+/)
      .map(function (t) { return t.trim().replace(/^#/, '') })
      .filter(function (t) { return !!t })
  },

  onSave () {
    const title = String(this.data.formTitle || '').trim()
    const code = String(this.data.code || '')

    if (!title) {
      platform.showToast({ title: '请输入标题', icon: 'none' })
      return
    }
    if (!code.trim()) {
      platform.showToast({ title: '请输入代码', icon: 'none' })
      return
    }

    const payload = {
      title: title,
      language: this.data.language,
      description: String(this.data.description || '').trim(),
      code: code,
      tags: this.parseTags(this.data.tagsText),
      favorite: this.data.favorite
    }

    if (this.data.isEdit) {
      stores.snippets.update(this.data.snippetId, payload)
    } else {
      stores.snippets.add(payload)
    }

    platform.showToast({ title: '已保存', icon: 'success' })
    setTimeout(function () { platform.navigateBack() }, 400)
  },

  onDelete () {
    if (!this.data.isEdit) return
    const self = this
    platform.showModal({
      title: '删除片段？',
      content: '删除后无法恢复。',
      confirmText: '删除',
      confirmColor: '#E5484D',
      success: function (res) {
        if (!res.confirm) return
        stores.snippets.remove(self.data.snippetId)
        platform.showToast({ title: '已删除', icon: 'success' })
        setTimeout(function () { platform.navigateBack() }, 300)
      }
    })
  },

  onShareAppMessage () {
    return { title: this.data.formTitle || '代码片段', path: '/pages/tech/snippets/index' }
  }
})
