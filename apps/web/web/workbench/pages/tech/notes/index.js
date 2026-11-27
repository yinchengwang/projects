/**
 * 技术笔记 · 列表
 *
 * 排序：置顶优先，其次按更新时间倒序。
 * 长按卡片可置顶 / 删除。
 */

const ui = require('../../../../../shared/ui.js')
const platform = require('../../../../../shared/platform.js')
const { withTheme } = require('../../../../../shared/core/with-theme.js')
const stores = require('../../../../../shared/workbench/stores.js')
const constants = require('../../../../../shared/core/constants.js')
const dateUtil = require('../../../../../shared/core/date.js')
const md = require('../../../../../shared/core/md.js')

module.exports = withTheme({
  data: {
    title: '技术笔记',
    items: [],
    count: 0,
    pinnedCount: 0
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
    if (!rows.length) rows.push(ui.empty('暂无笔记', '可创建一条记录开始整理。'))
    const children = [ui.pageHead({ title: state.title || '技术笔记', sub: '技术笔记、按置顶与更新时间排序。' })]
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
    const list = stores.techNotes.getAll().slice()

    list.sort(function (a, b) {
      if (!!a.pinned !== !!b.pinned) return a.pinned ? -1 : 1
      const ta = new Date(a.updatedAt || a.createdAt || 0).getTime()
      const tb = new Date(b.updatedAt || b.createdAt || 0).getTime()
      return tb - ta
    })

    const items = list.map(function (n) {
      const cat = constants.techCategory(n.category)
      return {
        id: n.id,
        title: n.title,
        color: cat.color,
        categoryLabel: cat.label,
        time: dateUtil.formatRelative(n.updatedAt || n.createdAt),
        preview: md.preview(n.content, 60),
        tags: (n.tags || []).slice(0, 3),
        pinned: !!n.pinned
      }
    })

    this.setData({
      items: items,
      count: items.length,
      pinnedCount: items.filter(function (i) { return i.pinned }).length
    })
  },

  onOpen (e) {
    platform.navigateTo('/workbench/' + e.currentTarget.dataset.id)
  },

  onCreate () {
    platform.navigateTo('/workbench/pages/tech/notes/detail')
  },

  /** 长按：置顶 / 取消置顶 / 删除 */
  onLongPress (e) {
    const id = e.currentTarget.dataset.id
    const note = stores.techNotes.find(id)
    if (!note) return

    const self = this
    const actions = [note.pinned ? '取消置顶' : '置顶', '删除']

    platform.showActionSheet({
      itemList: actions,
      itemColor: '#0F172A',
      success: function (res) {
        if (res.tapIndex === 0) {
          stores.techNotes.update(id, { pinned: !note.pinned })
          self.refresh()
          platform.showToast({ title: note.pinned ? '已取消置顶' : '已置顶', icon: 'none' })
          return
        }
        platform.showModal({
          title: '删除笔记？',
          content: '「' + note.title + '」删除后无法恢复。',
          confirmText: '删除',
          confirmColor: '#E5484D',
          success: function (m) {
            if (!m.confirm) return
            stores.techNotes.remove(id)
            self.refresh()
            platform.showToast({ title: '已删除', icon: 'success' })
          }
        })
      }
    })
  },

  onShareAppMessage () {
    return { title: 'DevBench · 技术笔记', path: '/pages/tech/notes/index' }
  }
})
