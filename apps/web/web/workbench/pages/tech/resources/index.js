/**
 * 资源收藏
 *
 * 无独立详情页，编辑通过内联面板完成（fixed 覆盖层）。
 * 排序：未归档优先，其次按星级倒序，再按创建时间。
 */

const ui = require('../../../../../shared/ui.js')
const platform = require('../../../../../shared/platform.js')
const { withTheme } = require('../../../../../shared/core/with-theme.js')
const stores = require('../../../../../shared/workbench/stores.js')
const constants = require('../../../../../shared/core/constants.js')

module.exports = withTheme({
  data: {
    title: '资源收藏',
    items: [],
    count: 0,
    archivedCount: 0,

    // 编辑面板
    editing: false,
    isEdit: false,
    resourceId: '',
    formTitle: '',
    formUrl: '',
    formType: 'article',
    formCategory: 'frontend',
    formStars: 3,
    formNote: '',
    formArchived: false,

    types: [],
    categories: [],
    starOptions: [1, 2, 3, 4, 5]
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
    if (!rows.length) rows.push(ui.empty('暂无资源收藏', '可创建一条记录开始整理。'))
    const children = [ui.pageHead({ title: state.title || '资源收藏', sub: '汇总技术资源与链接。' })]
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
    const list = stores.resources.getAll().slice()

    list.sort(function (a, b) {
      if (!!a.archived !== !!b.archived) return a.archived ? 1 : -1
      if ((b.stars || 0) !== (a.stars || 0)) return (b.stars || 0) - (a.stars || 0)
      return new Date(b.createdAt || 0).getTime() - new Date(a.createdAt || 0).getTime()
    })

    const items = list.map(function (r) {
      const cat = constants.techCategory(r.category)
      const type = constants.resourceType(r.type)
      return {
        id: r.id,
        title: r.title,
        color: cat.color,
        typeLabel: type.label,
        categoryLabel: cat.label,
        url: r.url || '',
        note: r.note || '',
        stars: r.stars || 0,
        archived: !!r.archived
      }
    })

    this.setData({
      items: items,
      count: items.length,
      archivedCount: items.filter(function (i) { return i.archived }).length
    })
  },

  // ==================== 卡片操作 ====================

  onOpen (e) {
    const id = e.currentTarget.dataset.id
    const item = stores.resources.find(id)
    if (!item) return

    const self = this
    const actions = ['编辑']
    if (item.url) actions.push('复制链接')
    actions.push(item.archived ? '取消归档' : '归档')
    actions.push('删除')

    platform.showActionSheet({
      itemList: actions,
      success: function (res) {
        const label = actions[res.tapIndex]

        if (label === '编辑') {
          self.openPanel(item)
          return
        }
        if (label === '复制链接') {
          wx.setClipboardData({
            data: item.url,
            success: function () { platform.showToast({ title: '已复制链接', icon: 'success' }) }
          })
          return
        }
        if (label === '归档' || label === '取消归档') {
          stores.resources.update(id, { archived: !item.archived })
          self.refresh()
          platform.showToast({ title: item.archived ? '已取消归档' : '已归档', icon: 'none' })
          return
        }
        platform.showModal({
          title: '删除资源？',
          content: '「' + item.title + '」删除后无法恢复。',
          confirmText: '删除',
          confirmColor: '#E5484D',
          success: function (m) {
            if (!m.confirm) return
            stores.resources.remove(id)
            self.refresh()
            platform.showToast({ title: '已删除', icon: 'success' })
          }
        })
      }
    })
  },

  // ==================== 编辑面板 ====================

  onCreate () {
    this.openPanel(null)
  },

  openPanel (item) {
    const isEdit = !!item
    const type = item ? item.type : 'article'
    const category = item ? item.category : 'frontend'

    this.setData({
      editing: true,
      isEdit: isEdit,
      resourceId: isEdit ? item.id : '',
      formTitle: isEdit ? item.title : '',
      formUrl: isEdit ? (item.url || '') : '',
      formType: type,
      formCategory: category,
      formStars: isEdit ? (item.stars || 3) : 3,
      formNote: isEdit ? (item.note || '') : '',
      formArchived: isEdit ? !!item.archived : false,
      types: this.buildTypes(type),
      categories: this.buildCategories(category)
    })
  },

  onClosePanel () {
    this.setData({ editing: false })
  },

  buildTypes (activeKey) {
    return constants.RESOURCE_TYPES.map(function (t) {
      return { key: t.key, label: t.label, active: t.key === activeKey }
    })
  },

  buildCategories (activeKey) {
    return constants.TECH_CATEGORIES.map(function (c) {
      return { key: c.key, label: c.label, color: c.color, active: c.key === activeKey }
    })
  },

  onFormTitle (e) {
    this.setData({ formTitle: e.detail.value })
  },

  onFormUrl (e) {
    this.setData({ formUrl: e.detail.value })
  },

  onFormNote (e) {
    this.setData({ formNote: e.detail.value })
  },

  onFormType (e) {
    const key = e.currentTarget.dataset.key
    this.setData({ formType: key, types: this.buildTypes(key) })
  },

  onFormCategory (e) {
    const key = e.currentTarget.dataset.key
    this.setData({ formCategory: key, categories: this.buildCategories(key) })
  },

  onFormStars (e) {
    this.setData({ formStars: Number(e.currentTarget.dataset.star) })
  },

  onToggleArchived () {
    this.setData({ formArchived: !this.data.formArchived })
  },

  onSave () {
    const title = String(this.data.formTitle || '').trim()
    if (!title) {
      platform.showToast({ title: '请输入标题', icon: 'none' })
      return
    }

    const payload = {
      title: title,
      url: String(this.data.formUrl || '').trim(),
      type: this.data.formType,
      category: this.data.formCategory,
      stars: this.data.formStars,
      note: String(this.data.formNote || '').trim(),
      archived: this.data.formArchived
    }

    if (this.data.isEdit) {
      stores.resources.update(this.data.resourceId, payload)
    } else {
      stores.resources.add(payload)
    }

    this.setData({ editing: false })
    this.refresh()
    platform.showToast({ title: '已保存', icon: 'success' })
  },

  onShareAppMessage () {
    return { title: 'DevBench · 资源收藏', path: '/pages/tech/resources/index' }
  }
})
