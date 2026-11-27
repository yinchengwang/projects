/**
 * 面试题库 · 详情 / 编辑
 *
 * 复习计数：点「标记已复习」把 reviewCount +1 并记录日期。
 */

const ui = require('../../../../../shared/ui.js')
const platform = require('../../../../../shared/platform.js')
const { withTheme } = require('../../../../../shared/core/with-theme.js')
const stores = require('../../../../../shared/workbench/stores.js')
const constants = require('../../../../../shared/core/constants.js')
const dateUtil = require('../../../../../shared/core/date.js')

module.exports = withTheme({
  data: {
    title: '题目',
    isEdit: false,
    questionId: '',

    question: '',
    answer: '',
    category: 'frontend',
    difficulty: 'med',
    starred: false,
    reviewCount: 0,
    lastReviewedText: '',

    categories: [],
    difficulties: []
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
  onLoad (query) {
    const id = (query && query.id) || ''
    const isEdit = !!id
    const item = isEdit ? stores.interview.find(id) : null

    const category = item ? item.category : 'frontend'
    const difficulty = item ? item.difficulty : 'med'

    this.setData({
      isEdit: isEdit,
      questionId: id,
      question: item ? item.question : '',
      answer: item ? item.answer : '',
      category: category,
      difficulty: difficulty,
      starred: item ? !!item.starred : false,
      reviewCount: item ? (item.reviewCount || 0) : 0,
      lastReviewedText: item && item.lastReviewed
        ? dateUtil.formatRelative(item.lastReviewed)
        : '尚未复习',
      categories: this.buildCategories(category),
      difficulties: this.buildDifficulties(difficulty)
    })
  },

  buildCategories (activeKey) {
    return constants.TECH_CATEGORIES.map(function (c) {
      return { key: c.key, label: c.label, color: c.color, active: c.key === activeKey }
    })
  },

  buildDifficulties (activeKey) {
    return constants.DIFFICULTIES.map(function (d) {
      return { key: d.key, label: d.label, color: d.color, active: d.key === activeKey }
    })
  },

  onQuestion (e) {
    this.setData({ question: e.detail.value })
  },

  onAnswer (e) {
    this.setData({ answer: e.detail.value })
  },

  onCategory (e) {
    const key = e.currentTarget.dataset.key
    this.setData({ category: key, categories: this.buildCategories(key) })
  },

  onDifficulty (e) {
    const key = e.currentTarget.dataset.key
    this.setData({ difficulty: key, difficulties: this.buildDifficulties(key) })
  },

  onToggleStar () {
    this.setData({ starred: !this.data.starred })
  },

  /** 标记已复习：计数 +1，记录日期（需先保存过） */
  onMarkReviewed () {
    if (!this.data.isEdit) {
      platform.showToast({ title: '请先保存题目', icon: 'none' })
      return
    }
    const next = (this.data.reviewCount || 0) + 1
    const today = new Date().toISOString()
    stores.interview.update(this.data.questionId, {
      reviewCount: next,
      lastReviewed: today
    })
    this.setData({
      reviewCount: next,
      lastReviewedText: '刚刚'
    })
    platform.showToast({ title: '已记录复习', icon: 'success' })
  },

  onSave () {
    const question = String(this.data.question || '').trim()
    const answer = String(this.data.answer || '').trim()

    if (!question) {
      platform.showToast({ title: '请输入题目', icon: 'none' })
      return
    }
    if (!answer) {
      platform.showToast({ title: '请输入答案', icon: 'none' })
      return
    }

    const payload = {
      question: question,
      answer: answer,
      category: this.data.category,
      difficulty: this.data.difficulty,
      starred: this.data.starred
    }

    if (this.data.isEdit) {
      stores.interview.update(this.data.questionId, payload)
    } else {
      stores.interview.add(Object.assign({ reviewCount: 0 }, payload))
    }

    platform.showToast({ title: '已保存', icon: 'success' })
    setTimeout(function () { platform.navigateBack() }, 400)
  },

  onDelete () {
    if (!this.data.isEdit) return
    const self = this
    platform.showModal({
      title: '删除题目？',
      content: '删除后无法恢复。',
      confirmText: '删除',
      confirmColor: '#E5484D',
      success: function (res) {
        if (!res.confirm) return
        stores.interview.remove(self.data.questionId)
        platform.showToast({ title: '已删除', icon: 'success' })
        setTimeout(function () { platform.navigateBack() }, 300)
      }
    })
  },

  onShareAppMessage () {
    return { title: 'DevBench · 面试题库', path: '/pages/tech/interview/index' }
  }
})
