/**
 * 闪念
 *
 * 顶部一行快速输入（回车即存），下方时间线。
 * 排序：置顶优先 → 未归档优先 → 创建时间倒序。
 */

const { withTheme } = require('../../../../shared/core/with-theme.js')
const stores = require('../../../../shared/workbench/stores.js')
const dateUtil = require('../../../../shared/core/date.js')
const ui = require('../../../../shared/ui.js')
const platform = require('../../../../shared/platform.js')

module.exports = withTheme({
  data: {
    title: '闪念',
    quickText: '',
    items: [],
    count: 0,
    pinnedCount: 0,

    editing: false,
    insightId: '',
    formContent: '',
    formTags: '',
    formPinned: false,
    formArchived: false
  },

  render: function (state) {
    const quick = ui.el('div', { class: 'wb-quick-add' }, [
      ui.el('input', { class: 'wb-input', value: state.quickText || '', placeholder: '记下一条闪念…' }),
      ui.el('button', { class: 'btn primary', text: '记下' })
    ])
    const input = quick.children[0]
    input.addEventListener('input', function (e) { this.onQuickInput({ detail: { value: e.target.value } }) }.bind(this))
    quick.children[1].addEventListener('click', this.onQuickAdd.bind(this))
    const rows = (state.items || []).map(function (item) {
      const card = ui.el('article', { class: 'wb-insight-card' }, [
        ui.el('button', { class: 'wb-insight-content', text: item.content }),
        ui.el('p', { class: 'wb-muted', text: item.time + (item.pinned ? ' · 置顶' : '') + (item.archived ? ' · 已归档' : '') }),
        ui.el('div', { class: 'wb-chip-row' }, (item.tags || []).map(function (tag) { return ui.pill('#' + tag) }))
      ])
      card.children[0].addEventListener('click', function () { this.onOpen({ currentTarget: { dataset: { id: item.id } } }) }.bind(this))
      card.addEventListener('contextmenu', function (e) { e.preventDefault(); this.onLongPress({ currentTarget: { dataset: { id: item.id } } }) }.bind(this))
      return card
    }.bind(this))
    if (!rows.length) rows.push(ui.empty('还没有闪念', '随手记下想法，之后再整理。'))
    const children = [ui.pageHead({ title: state.title, sub: state.count + ' 条记录 · ' + state.pinnedCount + ' 条置顶' }), quick, ui.el('div', { class: 'wb-insight-list' }, rows)]
    if (state.editing) {
      const textarea = ui.el('textarea', { class: 'wb-textarea' })
      textarea.value = state.formContent || ''
      textarea.addEventListener('input', function (e) { this.onFormContent({ detail: { value: e.target.value } }) }.bind(this))
      const tagInput = ui.el('input', { class: 'wb-input', value: state.formTags || '', placeholder: '标签，逗号分隔' })
      tagInput.addEventListener('input', function (e) { this.onFormTags({ detail: { value: e.target.value } }) }.bind(this))
      children.push(ui.panel({ title: '编辑闪念', children: [textarea, tagInput,
        ui.el('button', { class: 'btn primary', text: '保存', onclick: this.onSave.bind(this) }),
        ui.el('button', { class: 'btn danger', text: '删除', onclick: this.onDelete.bind(this) }),
        ui.el('button', { class: 'btn', text: '关闭', onclick: this.onClosePanel.bind(this) })
      ] }))
    }
    return ui.el('div', { class: 'wb-page wb-insights' }, children)
  },
  onLoad () {
    this.refresh()
  },

  onShow () {
    this.refresh()
  },

  refresh () {
    const list = stores.insights.getAll().slice()

    list.sort(function (a, b) {
      if (!!a.pinned !== !!b.pinned) return a.pinned ? -1 : 1
      if (!!a.archived !== !!b.archived) return a.archived ? 1 : -1
      return new Date(b.createdAt || 0).getTime() - new Date(a.createdAt || 0).getTime()
    })

    const items = list.map(function (i) {
      return {
        id: i.id,
        content: i.content,
        tags: i.tags || [],
        pinned: !!i.pinned,
        archived: !!i.archived,
        time: dateUtil.formatRelative(i.createdAt)
      }
    })

    this.setData({
      items: items,
      count: items.length,
      pinnedCount: items.filter(function (i) { return i.pinned }).length
    })
  },

  // ==================== 快速输入 ====================

  onQuickInput (e) {
    this.setData({ quickText: e.detail.value })
  },

  onQuickAdd () {
    const text = String(this.data.quickText || '').trim()
    if (!text) {
      platform.showToast({ title: '写点什么', icon: 'none' })
      return
    }
    stores.insights.add({ content: text, tags: [], pinned: false, archived: false })
    this.setData({ quickText: '' })
    this.refresh()
    platform.showToast({ title: '已记下', icon: 'success' })
  },

  // ==================== 长按操作 ====================

  onLongPress (e) {
    const id = e.currentTarget.dataset.id
    const item = stores.insights.find(id)
    if (!item) return Promise.resolve(null)

    const self = this
    return platform.showActionSheet({
      itemList: [
        item.pinned ? '取消置顶' : '置顶',
        item.archived ? '取消归档' : '归档',
        '删除'
      ]
    }).then(function (res) {
      if (!res) return null
      if (res.tapIndex === 0) {
        stores.insights.update(id, { pinned: !item.pinned })
        self.refresh()
        return null
      }
      if (res.tapIndex === 1) {
        stores.insights.update(id, { archived: !item.archived })
        self.refresh()
        return null
      }
      if (res.tapIndex !== 2) return null
      return platform.showModal({
        title: '删除这条闪念？',
        content: '删除后无法恢复。',
        confirmText: '删除',
        confirmColor: '#E5484D'
      }).then(function (m) {
        if (!m || !m.confirm) return null
        stores.insights.remove(id)
        self.refresh()
        platform.showToast({ title: '已删除', icon: 'success' })
        return null
      })
    })
  },

  // ==================== 编辑面板 ====================

  onOpen (e) {
    const item = stores.insights.find(e.currentTarget.dataset.id)
    if (!item) return
    this.setData({
      editing: true,
      insightId: item.id,
      formContent: item.content,
      formTags: (item.tags || []).join(', '),
      formPinned: !!item.pinned,
      formArchived: !!item.archived
    })
  },

  onClosePanel () {
    this.setData({ editing: false })
  },

  onFormContent (e) { this.setData({ formContent: e.detail.value }) },
  onFormTags (e) { this.setData({ formTags: e.detail.value }) },

  onTogglePinned () {
    this.setData({ formPinned: !this.data.formPinned })
  },

  onToggleArchived () {
    this.setData({ formArchived: !this.data.formArchived })
  },

  onSave () {
    const content = String(this.data.formContent || '').trim()
    if (!content) {
      platform.showToast({ title: '内容不能为空', icon: 'none' })
      return
    }

    const tags = String(this.data.formTags || '')
      .split(/[,，、\s]+/)
      .map(function (t) { return t.trim().replace(/^#/, '') })
      .filter(function (t) { return !!t })

    stores.insights.update(this.data.insightId, {
      content: content,
      tags: tags,
      pinned: this.data.formPinned,
      archived: this.data.formArchived
    })

    this.setData({ editing: false })
    this.refresh()
    platform.showToast({ title: '已保存', icon: 'success' })
  },

  onDelete () {
    const self = this
    platform.showModal({
      title: '删除这条闪念？',
      content: '删除后无法恢复。',
      confirmText: '删除',
      confirmColor: '#E5484D'
    }).then(function (res) {
      if (!res.confirm) return
      stores.insights.remove(self.data.insightId)
      self.setData({ editing: false })
      self.refresh()
      platform.showToast({ title: '已删除', icon: 'success' })
    })
  },

  onShareAppMessage () {
    return { title: 'DevBench · 闪念', path: '/pages/insights/index' }
  }
})
