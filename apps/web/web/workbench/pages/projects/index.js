/**
 * 项目集
 *
 * 排序：进行中 → 开发中 → 已归档，同状态内按创建时间倒序。
 * 点卡片打开编辑面板，长按删除。
 */

const { withTheme } = require('../../../../shared/core/with-theme.js')
const stores = require('../../../../shared/workbench/stores.js')
const constants = require('../../../../shared/core/constants.js')
const ui = require('../../../../shared/ui.js')
const platform = require('../../../../shared/platform.js')

const STATUS_ORDER = { active: 0, wip: 1, archived: 2 }

module.exports = withTheme({
  data: {
    title: '项目集',
    items: [],
    count: 0,
    activeCount: 0,

    editing: false,
    isEdit: false,
    projectId: '',
    formName: '',
    formDesc: '',
    formStack: '',
    formHighlights: '',
    formStatus: 'active',
    formStars: 0,
    formRepo: '',
    formDemo: '',
    statuses: [],
    starOptions: [1, 2, 3, 4, 5]
  },

  render: function (state) {
    const rows = (state.items || []).map(function (item) {
      const open = ui.el('button', { class: 'wb-project-open', text: item.name + ' · ' + item.statusLabel })
      open.addEventListener('click', function () { this.onOpen({ currentTarget: { dataset: { id: item.id } } }) }.bind(this))
      const meta = ui.el('p', { class: 'wb-muted', text: item.description || '暂无简介' })
      const chips = ui.el('div', { class: 'wb-chip-row' }, (item.techStack || []).map(function (v) { return ui.pill(v) }))
      return ui.el('article', { class: 'wb-project-card' }, [open, meta, chips, ui.el('p', { class: 'wb-muted', text: (item.highlights || []).join(' · ') })])
    }.bind(this))
    if (!rows.length) rows.push(ui.empty('还没有项目', '添加一个项目，整理技术栈与成果。'))
    const children = [
      ui.pageHead({ title: state.title, sub: state.count + ' 个项目 · ' + state.activeCount + ' 个进行中' }),
      ui.el('button', { class: 'btn primary', text: '新建项目', onclick: function () { this.onCreate() }.bind(this) }),
      ui.el('div', { class: 'wb-project-list' }, rows)
    ]
    if (state.editing) {
      const fields = [
        ['项目名称', 'formName', state.formName, 'text'],
        ['项目简介', 'formDesc', state.formDesc, 'text'],
        ['技术栈（逗号分隔）', 'formStack', state.formStack, 'text'],
        ['亮点（每行一项）', 'formHighlights', state.formHighlights, 'text'],
        ['仓库链接', 'formRepo', state.formRepo, 'url'],
        ['演示链接', 'formDemo', state.formDemo, 'url']
      ].map(function (f) {
        const input = ui.el('input', { type: f[3], value: f[2] || '', placeholder: f[0], class: 'wb-input' })
        input.addEventListener('input', function (event) { this.setData({ [f[1]]: event.target.value }) }.bind(this))
        return ui.el('label', { class: 'wb-field' }, [ui.el('span', { text: f[0] }), input])
      }.bind(this))
      fields.push(ui.el('button', { class: 'btn primary', text: '保存项目', onclick: function () { this.onSave() }.bind(this) }))
      fields.push(ui.el('button', { class: 'btn', text: '取消', onclick: function () { this.onClosePanel() }.bind(this) }))
      children.push(ui.panel({ title: state.isEdit ? '编辑项目' : '新建项目', className: 'wb-editor', children: fields }))
    }
    return ui.el('div', { class: 'wb-page wb-projects' }, children)
  },
  onLoad () {
    this.refresh()
  },

  onShow () {
    this.refresh()
  },

  refresh () {
    const list = stores.projects.getAll().slice()

    list.sort(function (a, b) {
      const oa = STATUS_ORDER[a.status] === undefined ? 9 : STATUS_ORDER[a.status]
      const ob = STATUS_ORDER[b.status] === undefined ? 9 : STATUS_ORDER[b.status]
      if (oa !== ob) return oa - ob
      return new Date(b.createdAt || 0).getTime() - new Date(a.createdAt || 0).getTime()
    })

    const items = list.map(function (p) {
      const status = constants.projectStatus(p.status)
      return {
        id: p.id,
        name: p.name,
        description: p.description || '',
        color: status.color,
        statusLabel: status.label,
        status: p.status,
        techStack: (p.techStack || []).slice(0, 4),
        highlights: (p.highlights || []).slice(0, 2),
        stars: p.stars || 0,
        repoUrl: p.repoUrl || '',
        demoUrl: p.demoUrl || ''
      }
    })

    this.setData({
      items: items,
      count: items.length,
      activeCount: items.filter(function (i) { return i.status === 'active' }).length
    })
  },

  // ==================== 面板 ====================

  onCreate () {
    this.openPanel(null)
  },

  onOpen (e) {
    const item = stores.projects.find(e.currentTarget.dataset.id)
    if (item) this.openPanel(item)
  },

  openPanel (item) {
    const isEdit = !!item
    const status = item ? item.status : 'active'

    this.setData({
      editing: true,
      isEdit: isEdit,
      projectId: isEdit ? item.id : '',
      formName: isEdit ? item.name : '',
      formDesc: isEdit ? (item.description || '') : '',
      formStack: isEdit ? (item.techStack || []).join(', ') : '',
      formHighlights: isEdit ? (item.highlights || []).join('\n') : '',
      formStatus: status,
      formStars: isEdit ? (item.stars || 0) : 0,
      formRepo: isEdit ? (item.repoUrl || '') : '',
      formDemo: isEdit ? (item.demoUrl || '') : '',
      statuses: this.buildStatuses(status)
    })
  },

  onClosePanel () {
    this.setData({ editing: false })
  },

  buildStatuses (activeKey) {
    return constants.PROJECT_STATUS.map(function (s) {
      return { key: s.key, label: s.label, color: s.color, active: s.key === activeKey }
    })
  },

  onFormName (e) { this.setData({ formName: e.detail.value }) },
  onFormDesc (e) { this.setData({ formDesc: e.detail.value }) },
  onFormStack (e) { this.setData({ formStack: e.detail.value }) },
  onFormHighlights (e) { this.setData({ formHighlights: e.detail.value }) },
  onFormRepo (e) { this.setData({ formRepo: e.detail.value }) },
  onFormDemo (e) { this.setData({ formDemo: e.detail.value }) },

  onFormStatus (e) {
    const key = e.currentTarget.dataset.key
    this.setData({ formStatus: key, statuses: this.buildStatuses(key) })
  },

  onFormStars (e) {
    this.setData({ formStars: Number(e.currentTarget.dataset.star) })
  },

  onSave () {
    const name = String(this.data.formName || '').trim()
    if (!name) {
      platform.showToast({ title: '请输入项目名', icon: 'none' })
      return
    }

    const stack = String(this.data.formStack || '')
      .split(/[,，、]+/)
      .map(function (s) { return s.trim() })
      .filter(function (s) { return !!s })

    const highlights = String(this.data.formHighlights || '')
      .split(/\n+/)
      .map(function (s) { return s.trim() })
      .filter(function (s) { return !!s })

    const payload = {
      name: name,
      description: String(this.data.formDesc || '').trim(),
      techStack: stack,
      highlights: highlights,
      status: this.data.formStatus,
      stars: this.data.formStars,
      repoUrl: String(this.data.formRepo || '').trim(),
      demoUrl: String(this.data.formDemo || '').trim()
    }

    if (this.data.isEdit) {
      stores.projects.update(this.data.projectId, payload)
    } else {
      stores.projects.add(payload)
    }

    this.setData({ editing: false })
    this.refresh()
    platform.showToast({ title: '已保存', icon: 'success' })
  },

  onDelete () {
    if (!this.data.isEdit) return
    const self = this
    platform.showModal({
      title: '删除这个项目？',
      content: '删除后无法恢复。',
      confirmText: '删除',
      confirmColor: '#E5484D',
      success: function (res) {
        if (!res.confirm) return
        stores.projects.remove(self.data.projectId)
        self.setData({ editing: false })
        self.refresh()
        platform.showToast({ title: '已删除', icon: 'success' })
      }
    })
  },

  onLongPress (e) {
    const id = e.currentTarget.dataset.id
    const item = stores.projects.find(id)
    if (!item) return
    const self = this
    platform.showModal({
      title: '删除项目？',
      content: item.name,
      confirmText: '删除',
      confirmColor: '#E5484D',
      success: function (res) {
        if (!res.confirm) return
        stores.projects.remove(id)
        self.refresh()
        platform.showToast({ title: '已删除', icon: 'success' })
      }
    })
  },

  /** 复制仓库或演示链接 */
  onCopyLink (e) {
    const url = e.currentTarget.dataset.url
    if (!url) return
    platform.setClipboardData({
      data: url,
      success: function () { platform.showToast({ title: '已复制链接', icon: 'success' }) }
    })
  },

  onShareAppMessage () {
    return { title: 'DevBench · 项目集', path: '/pages/projects/index' }
  }
})
