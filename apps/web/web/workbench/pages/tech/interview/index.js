/**
 * 面试题库 · 列表
 *
 * 排序：收藏优先，其次按创建时间倒序。
 * 长按卡片可收藏 / 取消收藏 / 删除。
 */

const ui = require('../../../../../shared/ui.js')
const platform = require('../../../../../shared/platform.js')
const { withTheme } = require('../../../../../shared/core/with-theme.js')
const stores = require('../../../../../shared/workbench/stores.js')
const constants = require('../../../../../shared/core/constants.js')

module.exports = withTheme({
  data: {
    title: '面试题库',
    items: [],
    count: 0,
    starredCount: 0
  },

  render: function (state) {
    const rows = (state.items || []).map(function (item) {
      const title = item.title || item.question || item.content || item.name || item.question || '未命名'
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
    if (!rows.length) rows.push(ui.empty('暂无面试题', '可创建一条记录开始整理。'))
    const children = [ui.pageHead({ title: state.title || '面试题库', sub: '复习记录与难度一览。' })]
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
    const list = stores.interview.getAll().slice()

    list.sort(function (a, b) {
      if (!!a.starred !== !!b.starred) return a.starred ? -1 : 1
      return new Date(b.createdAt || 0).getTime() - new Date(a.createdAt || 0).getTime()
    })

    const items = list.map(function (q) {
      const cat = constants.techCategory(q.category)
      const diff = constants.difficulty(q.difficulty)
      return {
        id: q.id,
        question: q.question,
        color: cat.color,
        categoryLabel: cat.label,
        difficultyLabel: diff.label,
        difficultyColor: diff.color,
        reviewCount: q.reviewCount || 0,
        starred: !!q.starred
      }
    })

    this.setData({
      items: items,
      count: items.length,
      starredCount: items.filter(function (i) { return i.starred }).length
    })
  },

  onOpen (e) {
    platform.navigateTo('/workbench/' + e.currentTarget.dataset.id)
  },

  onCreate () {
    platform.navigateTo('/workbench/pages/tech/interview/detail')
  },

  onLongPress (e) {
    const id = e.currentTarget.dataset.id
    const item = stores.interview.find(id)
    if (!item) return

    const self = this
    platform.showActionSheet({
      itemList: [item.starred ? '取消收藏' : '收藏', '删除'],
      success: function (res) {
        if (res.tapIndex === 0) {
          stores.interview.update(id, { starred: !item.starred })
          self.refresh()
          platform.showToast({ title: item.starred ? '已取消收藏' : '已收藏', icon: 'none' })
          return
        }
        platform.showModal({
          title: '删除题目？',
          content: '删除后无法恢复。',
          confirmText: '删除',
          confirmColor: '#E5484D',
          success: function (m) {
            if (!m.confirm) return
            stores.interview.remove(id)
            self.refresh()
            platform.showToast({ title: '已删除', icon: 'success' })
          }
        })
      }
    })
  },

  onShareAppMessage () {
    return { title: 'DevBench · 面试题库', path: '/pages/tech/interview/index' }
  }
})
