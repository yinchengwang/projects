/**
 * 健身
 *
 * 顶部汇总本周次数 / 时长 / 消耗，下方按日期倒序列出记录。
 * 点卡片编辑，长按删除。
 */

const ui = require('../../../../../shared/ui.js')
const platform = require('../../../../../shared/platform.js')
const { withTheme } = require('../../../../../shared/core/with-theme.js')
const stores = require('../../../../../shared/workbench/stores.js')
const constants = require('../../../../../shared/core/constants.js')
const dateUtil = require('../../../../../shared/core/date.js')
const fmt = require('../../../../../shared/core/format.js')

module.exports = withTheme({
  data: {
    title: '健身',
    items: [],
    count: 0,
    weekCount: 0,
    weekMinutes: 0,
    weekCalories: 0,

    editing: false,
    isEdit: false,
    recordId: '',
    formDate: '',
    formType: 'cardio',
    formName: '',
    formDuration: '',
    formCalories: '',
    formNotes: '',
    types: []
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
  onLoad () {
    this.refresh()
  },

  onShow () {
    this.refresh()
  },

  refresh () {
    const list = stores.fitness.getAll().slice()

    list.sort(function (a, b) {
      if (a.date !== b.date) return a.date < b.date ? 1 : -1
      return new Date(b.createdAt || 0).getTime() - new Date(a.createdAt || 0).getTime()
    })

    const week = dateUtil.weekRange()
    let weekCount = 0
    let weekMinutes = 0
    let weekCalories = 0

    list.forEach(function (r) {
      if (r.date >= week.start && r.date <= week.end) {
        weekCount += 1
        weekMinutes += (r.durationMin || 0)
        weekCalories += (r.calories || 0)
      }
    })

    const items = list.map(function (r) {
      const type = constants.fitnessType(r.type)
      return {
        id: r.id,
        name: r.name,
        date: r.date,
        dateLabel: dateUtil.humanDay(r.date),
        typeLabel: type.label,
        duration: fmt.duration(r.durationMin || 0),
        calories: r.calories || 0,
        notes: r.notes || ''
      }
    })

    this.setData({
      items: items,
      count: items.length,
      weekCount: weekCount,
      weekMinutes: weekMinutes,
      weekCalories: weekCalories
    })
  },

  // ==================== 面板 ====================

  onCreate () {
    this.openPanel(null)
  },

  onOpen (e) {
    const item = stores.fitness.find(e.currentTarget.dataset.id)
    if (item) this.openPanel(item)
  },

  openPanel (item) {
    const isEdit = !!item
    const type = item ? item.type : 'cardio'

    this.setData({
      editing: true,
      isEdit: isEdit,
      recordId: isEdit ? item.id : '',
      formDate: isEdit ? item.date : dateUtil.todayStr(),
      formType: type,
      formName: isEdit ? item.name : '',
      formDuration: isEdit ? String(item.durationMin || '') : '',
      formCalories: isEdit && item.calories ? String(item.calories) : '',
      formNotes: isEdit ? (item.notes || '') : '',
      types: this.buildTypes(type)
    })
  },

  onClosePanel () {
    this.setData({ editing: false })
  },

  buildTypes (activeKey) {
    return constants.FITNESS_TYPES.map(function (t) {
      return { key: t.key, label: t.label, active: t.key === activeKey }
    })
  },

  onFormDate (e) {
    this.setData({ formDate: e.detail.value })
  },

  onFormType (e) {
    const key = e.currentTarget.dataset.key
    this.setData({ formType: key, types: this.buildTypes(key) })
  },

  onFormName (e) { this.setData({ formName: e.detail.value }) },
  onFormDuration (e) { this.setData({ formDuration: e.detail.value }) },
  onFormCalories (e) { this.setData({ formCalories: e.detail.value }) },
  onFormNotes (e) { this.setData({ formNotes: e.detail.value }) },

  onSave () {
    const name = String(this.data.formName || '').trim()
    const duration = Number(this.data.formDuration) || 0

    if (!name) {
      platform.showToast({ title: '请输入运动名称', icon: 'none' })
      return
    }
    if (duration <= 0) {
      platform.showToast({ title: '请输入运动时长', icon: 'none' })
      return
    }

    const payload = {
      date: this.data.formDate || dateUtil.todayStr(),
      type: this.data.formType,
      name: name,
      durationMin: duration,
      calories: Number(this.data.formCalories) || 0,
      notes: String(this.data.formNotes || '').trim()
    }

    if (this.data.isEdit) {
      stores.fitness.update(this.data.recordId, payload)
    } else {
      stores.fitness.add(payload)
    }

    this.setData({ editing: false })
    this.refresh()
    platform.showToast({ title: '已保存', icon: 'success' })
  },

  onDelete () {
    if (!this.data.isEdit) return
    const self = this
    platform.showModal({
      title: '删除这条记录？',
      content: '删除后无法恢复。',
      confirmText: '删除',
      confirmColor: '#E5484D',
      success: function (res) {
        if (!res.confirm) return
        stores.fitness.remove(self.data.recordId)
        self.setData({ editing: false })
        self.refresh()
        platform.showToast({ title: '已删除', icon: 'success' })
      }
    })
  },

  onLongPress (e) {
    const id = e.currentTarget.dataset.id
    const item = stores.fitness.find(id)
    if (!item) return
    const self = this
    platform.showModal({
      title: '删除这条记录？',
      content: item.date + ' · ' + item.name,
      confirmText: '删除',
      confirmColor: '#E5484D',
      success: function (res) {
        if (!res.confirm) return
        stores.fitness.remove(id)
        self.refresh()
        platform.showToast({ title: '已删除', icon: 'success' })
      }
    })
  },

  onShareAppMessage () {
    return { title: 'DevBench · 健身', path: '/pages/life/fitness/index' }
  }
})
