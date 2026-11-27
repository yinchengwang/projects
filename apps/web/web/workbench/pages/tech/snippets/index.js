/**
 * 代码片段 · 列表
 *
 * 排序：收藏优先，其次按创建时间倒序。
 * 长按卡片可收藏 / 取消收藏 / 删除。
 */

const ui = require('../../../../../shared/ui.js')
const platform = require('../../../../../shared/platform.js')
const { withTheme } = require('../../../../../shared/core/with-theme.js')
const stores = require('../../../../../shared/workbench/stores.js')
const constants = require('../../../../../shared/core/constants.js')
const dateUtil = require('../../../../../shared/core/date.js')

module.exports = withTheme({
  data: {
    title: '代码片段',
    items: [],
    count: 0,
    favoriteCount: 0
  },

  render: function (state) {
    const rows = (state.items || []).map(function (item) {
      const title = item.title || item.question || item.content || item.name || item.code || '未命名'
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
    if (!rows.length) rows.push(ui.empty('暂无代码片段', '可创建一条记录开始整理。'))
    const children = [ui.pageHead({ title: state.title || '代码片段', sub: '按语言整理可复用代码。' })]
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
    const list = stores.snippets.getAll().slice()

    list.sort(function (a, b) {
      if (!!a.favorite !== !!b.favorite) return a.favorite ? -1 : 1
      return new Date(b.createdAt || 0).getTime() - new Date(a.createdAt || 0).getTime()
    })

    const items = list.map(function (s) {
      const lang = constants.language(s.language)
      const lineCount = String(s.code || '').split('\n').length
      return {
        id: s.id,
        title: s.title,
        color: lang.color,
        languageLabel: lang.label,
        desc: s.description || '',
        time: dateUtil.formatRelative(s.createdAt),
        lineCount: lineCount,
        tags: (s.tags || []).slice(0, 3),
        favorite: !!s.favorite
      }
    })

    this.setData({
      items: items,
      count: items.length,
      favoriteCount: items.filter(function (i) { return i.favorite }).length
    })
  },

  onOpen (e) {
    platform.navigateTo('/workbench/' + e.currentTarget.dataset.id)
  },

  onCreate () {
    platform.navigateTo('/workbench/pages/tech/snippets/detail')
  },

  onLongPress (e) {
    const id = e.currentTarget.dataset.id
    const item = stores.snippets.find(id)
    if (!item) return

    const self = this
    platform.showActionSheet({
      itemList: [item.favorite ? '取消收藏' : '收藏', '删除'],
      success: function (res) {
        if (res.tapIndex === 0) {
          stores.snippets.update(id, { favorite: !item.favorite })
          self.refresh()
          platform.showToast({ title: item.favorite ? '已取消收藏' : '已收藏', icon: 'none' })
          return
        }
        platform.showModal({
          title: '删除片段？',
          content: '「' + item.title + '」删除后无法恢复。',
          confirmText: '删除',
          confirmColor: '#E5484D',
          success: function (m) {
            if (!m.confirm) return
            stores.snippets.remove(id)
            self.refresh()
            platform.showToast({ title: '已删除', icon: 'success' })
          }
        })
      }
    })
  },

  onShareAppMessage () {
    return { title: 'DevBench · 代码片段', path: '/pages/tech/snippets/index' }
  }
})
