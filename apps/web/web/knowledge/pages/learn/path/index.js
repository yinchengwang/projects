"use strict"
/**
 * 学习路径 —— 路径切换 / 步骤展开 / 子步骤勾选
 */

const { withTheme } = require("../../../../../shared/core/with-theme.js")
const stores = require("../../../../../shared/knowledge/stores.js")
const khc = require("../../../../../shared/knowledge/kh-constants.js")
const ui = require("../../../../../shared/ui.js")
const platform = require("../../../../../shared/platform.js")
const syncTheme = require("../../../theme-bridge.js").syncTheme

module.exports = withTheme({
  data: {
    title: "学习路径",
    paths: [],
    activeId: "",
    steps: []
  },

  render: function (state) {
    const pathChips = (state.paths || []).map(function (p) {
      const button = ui.el("button", {
        class: "kh-tab" + (state.activeId === p.id ? " is-active" : ""),
        text: p.icon + " " + p.name
      })
      button.addEventListener("click", function () {
        this.onPickPath({ currentTarget: { dataset: { id: p.id } } })
      }.bind(this))
      return button
    })

    const rows = (state.paths || []).map(function (p) {
      return ui.el("div", { class: "kh-list-row" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: p.name }),
          ui.progressBar(p.percent, { color: "#7F77DD" })
        ]),
        ui.el("span", { class: "kh-list-sub", text: p.done + " / " + p.total + " · " + p.difficulty })
      ])
    })

    const stepNodes = (state.steps || []).map(function (step) {
      const subs = (step.subSteps || []).map(function (ss) {
        const button = ui.el("button", { class: "kh-substep" + (ss.done ? " is-done" : "") }, [
          ui.el("span", { class: "kh-substep-mark", text: ss.done ? "✓" : "○" }),
          ui.el("span", { text: ss.title }),
          ui.pill(ss.typeLabel, "accent"),
          ui.el("span", { class: "kh-list-sub", text: ss.hours + "h" })
        ])
        button.addEventListener("click", function () {
          this.onToggleSub({ currentTarget: { dataset: { step: step.id, sub: ss.id } } })
        }.bind(this))
        return button
      })

      return ui.block({
        title: "第 " + (step.index + 1) + " 步 · " + step.title,
        children: [
          ui.el("p", { class: "reason", text: step.description }),
          ui.progressBar(step.percent, { color: "#0284C7", label: step.done + " / " + step.total }),
          ui.el("div", { class: "kh-substeps" }, subs)
        ]
      })
    })

    const reset = ui.el("button", {
      class: "kh-btn danger",
      text: "重置进度",
      onclick: function () { this.onReset() }.bind(this)
    })

    return ui.el("div", { class: "kh-page" }, [
      ui.pageHead({ eyebrow: "Reading Radar", title: "学习路径", sub: "分步进阶，勾掉子步骤即完成" }),
      ui.panel({ title: "全部路径", children: rows.length ? rows : [ui.empty("还没有学习路径", "种子数据缺失")] }),
      ui.el("div", { class: "kh-tabs" }, pathChips),
      ui.panel({
        title: "步骤",
        children: stepNodes.length ? stepNodes.concat([ui.el("div", { class: "kh-actions" }, [reset])]) : [ui.empty("这条路径没有步骤", "换一条路径看看")]
      })
    ])
  },

  onMount: function (rootElement) {
    this.rootElement = rootElement || this.rootElement
    this.refresh()
    syncTheme(this)
  },

  onLoad () {
    this.refresh()
  },

  onShow () {
    this.refresh()
  },

  refresh () {
    const all = stores.learningPaths.getAll()
    if (!all.length) {
      this.setData({ paths: [], activeId: "", steps: [] })
      return
    }
    const activeId = stores.learningPaths.find(this.data.activeId) ? this.data.activeId : all[0].id
    const active = stores.learningPaths.find(activeId)

    const paths = all.map(function (p) {
      const prog = stores.learningPaths.getPathProgress(p.id)
      return {
        id: p.id, name: p.name, icon: p.icon,
        difficulty: p.difficulty,
        percent: prog.percent,
        done: prog.done, total: prog.total
      }
    })

    const steps = (active.steps || []).map(function (s, i) {
      const prog = stores.learningPaths.getStepProgress(activeId, s.id)
      return {
        index: i,
        id: s.id,
        title: s.title,
        description: s.description,
        difficulty: s.difficulty,
        estimatedHours: s.estimatedHours,
        percent: prog.percent,
        done: prog.done,
        total: prog.total,
        resources: s.resources || [],
        subSteps: (s.subSteps || []).map(function (ss) {
          const meta = khc.subStepType(ss.type)
          return {
            id: ss.id,
            title: ss.title,
            typeLabel: meta.label,
            typeColor: meta.color,
            hours: ss.estimatedHours,
            done: !!active.completedSubSteps[s.id + ":" + ss.id]
          }
        })
      }
    })

    this.setData({ paths: paths, activeId: activeId, steps: steps })
  },

  onPickPath (e) {
    this.setData({ activeId: e.currentTarget.dataset.id })
    this.refresh()
  },

  onToggleSub (e) {
    const ds = e.currentTarget.dataset
    stores.learningPaths.toggleSubStep(this.data.activeId, ds.step, ds.sub)
    this.refresh()
  },

  onReset () {
    const self = this
    platform.showModal({
      title: "重置进度",
      content: "将清空该路径的所有子步骤完成记录"
    }).then(function (res) {
      if (!res.confirm) return
      stores.learningPaths.resetPath(self.data.activeId)
      self.refresh()
    })
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 学习路径", path: "/knowledge/pages/learn/path/index" }
  }
})
