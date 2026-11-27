"use strict"
/**
 * 题库练习 —— 5 种模式
 *
 * plan  今日计划（按 stack 轮换生成）
 * list  题目列表（按栈筛选 / 搜索）
 * quiz  答题（选择 / 判断 / 填空 / 简答）
 * wrong 错题本
 * stats 统计与各栈进度
 */

const { withTheme } = require("../../../../../shared/core/with-theme.js")
const stores = require("../../../../../shared/knowledge/stores.js")
const khc = require("../../../../../shared/knowledge/kh-constants.js")
const ui = require("../../../../../shared/ui.js")
const platform = require("../../../../../shared/platform.js")

const MODES = [
  { key: "plan", label: "今日计划" },
  { key: "list", label: "题目列表" },
  { key: "wrong", label: "错题本" },
  { key: "stats", label: "统计" }
]

module.exports = withTheme({
  data: {
    title: "题库练习",
    mode: "plan",
    modes: MODES,
    stacks: [],
    activeStack: "",
    query: "",
    // plan
    plan: null,
    planPercent: 0,
    // list
    questions: [],
    // quiz
    session: null,
    index: 0,
    picked: "",
    result: null,
    // wrong
    wrongs: [],
    // stats
    stats: null,
    progress: []
  },

  render: function (state) {
    const children = [
      ui.pageHead({ eyebrow: "Reading Radar", title: "题库练习", sub: "7 大技术栈 · 5 种题型" })
    ]

    const tabs = (state.modes || MODES).map(function (m) {
      const button = ui.el("button", {
        class: "kh-tab" + (state.mode === m.key ? " is-active" : ""),
        text: m.label
      })
      button.addEventListener("click", function () {
        this.onMode({ currentTarget: { dataset: { mode: m.key } } })
      }.bind(this))
      return button
    })
    children.push(ui.el("div", { class: "kh-tabs" }, tabs))

    if (state.mode === "quiz") children.push(this.renderQuiz(state))
    else if (state.mode === "wrong") children.push(this.renderWrong(state))
    else if (state.mode === "stats") children.push(this.renderStats(state))
    else children.push(this.renderPlan(state), this.renderList(state))

    return ui.el("div", { class: "kh-page" }, children)
  },

  /** 今日计划卡 */
  renderPlan: function (state) {
    const plan = state.plan || { stackLabel: "", done: 0, total: 0 }
    const actions = [
      ui.el("button", {
        class: "kh-btn primary",
        text: "开始练习",
        onclick: function () { this.onStartPlan() }.bind(this)
      }),
      ui.el("button", {
        class: "kh-btn",
        text: "换一批",
        onclick: function () { this.onRegeneratePlan() }.bind(this)
      })
    ]
    return ui.panel({
      title: "今日计划 · " + plan.stackLabel,
      children: [
        ui.el("p", { class: "reason", text: "计划 " + plan.total + " 题，已完成 " + plan.done + " 题" }),
        ui.progressBar(state.planPercent || 0, { color: "#6366F1", label: "今日进度" }),
        ui.el("div", { class: "kh-actions" }, actions)
      ]
    })
  },

  /** 题目列表（按栈筛选 / 搜索） */
  renderList: function (state) {
    const stackChips = (state.stacks || []).map(function (s) {
      const button = ui.el("button", {
        class: "kh-chip" + (state.activeStack === s.key ? " is-active" : ""),
        text: s.label,
        "data-color": s.color
      })
      button.addEventListener("click", function () {
        this.onPickStack({ currentTarget: { dataset: { key: s.key } } })
      }.bind(this))
      return button
    })

    const search = ui.el("input", { class: "kh-input", type: "search", placeholder: "搜索题干 / 场景 / 栈" })
    search.value = state.query || ""
    search.addEventListener("input", function (event) {
      this.onSearch({ detail: { value: event && event.target ? event.target.value : "" } })
    }.bind(this))

    const rows = (state.questions || []).map(function (q) {
      const button = ui.el("button", { class: "kh-list-row kh-row-btn" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: q.stem }),
          ui.el("div", { class: "kh-list-sub", text: q.stackLabel + " · " + q.difficulty + " · " + q.type })
        ]),
        ui.pill("练一题", "accent")
      ])
      button.addEventListener("click", function () {
        this.onAnswerOne({ currentTarget: { dataset: { id: q.id } } })
      }.bind(this))
      return button
    })

    return ui.panel({
      title: "题目列表 · " + (state.questions || []).length + " 题",
      children: [
        ui.el("div", { class: "kh-chips" }, stackChips),
        search,
        ui.el("div", { class: "kh-list" }, rows.length ? rows : [ui.empty("没有匹配的题目", "换个栈或清空搜索")])
      ]
    })
  },

  /** 答题界面 */
  renderQuiz: function (state) {
    const q = state.q
    if (!q) return ui.panel({ title: "答题", children: [ui.empty("没有题目", "回到今日计划开始一轮")] })

    const optionNodes = (state.options || []).map(function (opt) {
      const button = ui.el("button", {
        class: "kh-option" + (state.picked === opt.letter ? " is-active" : ""),
        text: opt.text
      })
      button.addEventListener("click", function () {
        this.onPickOption({ currentTarget: { dataset: { letter: opt.letter } } })
      }.bind(this))
      return button
    })

    const resultNodes = []
    if (state.result) {
      resultNodes.push(ui.pill(state.result.correct ? "答对了" : "答错了", state.result.correct ? "ok" : "bad"))
      resultNodes.push(ui.el("p", { class: "reason", text: "正确答案：" + state.result.right }))
      if (state.result.explain) {
        resultNodes.push(ui.el("p", { class: "kh-list-sub", text: state.result.explain }))
      }
    }

    const actions = [
      ui.el("button", {
        class: "kh-btn",
        text: "退出",
        onclick: function () { this.onExitQuiz() }.bind(this)
      }),
      ui.el("button", {
        class: "kh-btn primary",
        text: state.result ? "下一题" : "提交答案",
        onclick: function () {
          if (this.data.result) this.onNext()
          else this.onSubmit()
        }.bind(this)
      })
    ]

    return ui.panel({
      title: "第 " + q.position + " 题",
      children: [
        ui.el("div", { class: "kh-quiz-meta" }, [
          ui.pill(q.stackLabel, "accent"),
          ui.pill(q.difficulty, "warn"),
          ui.pill(q.type)
        ]),
        ui.el("p", { class: "kh-quiz-stem", text: q.stem }),
        ui.el("div", { class: "kh-options" }, optionNodes),
        ui.el("div", { class: "kh-result" }, resultNodes),
        ui.el("div", { class: "kh-actions" }, actions)
      ]
    })
  },

  /** 错题本 */
  renderWrong: function (state) {
    const rows = (state.wrongs || []).map(function (w) {
      return ui.el("div", { class: "kh-list-row" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: w.question ? w.question.stem : w.questionId }),
          ui.el("div", { class: "kh-list-sub", text: "上次作答：" + w.lastAnswer.userAnswer })
        ])
      ])
    })
    const start = ui.el("button", {
      class: "kh-btn primary",
      text: "练一遍错题",
      onclick: function () { this.onStartWrong() }.bind(this)
    })
    return ui.panel({
      title: "错题本 · " + rows.length + " 题",
      children: [
        ui.el("div", { class: "kh-list" }, rows.length ? rows : [ui.empty("错题本是空的", "答错的题会自动进这里")]),
        ui.el("div", { class: "kh-actions" }, [start])
      ]
    })
  },

  /** 统计与各栈进度 */
  renderStats: function (state) {
    const stats = state.stats || { totalAnswered: 0, totalCorrect: 0, streak: 0, accuracyPct: 0 }
    const rows = (state.progress || []).map(function (p) {
      return ui.el("div", { class: "kh-list-row" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: p.label }),
          ui.progressBar(p.percent, { color: p.color })
        ]),
        ui.el("span", { class: "kh-list-sub", text: p.correct + " / " + p.answered + " / " + p.total })
      ])
    })
    return ui.el("div", {}, [
      ui.statGrid([
        { label: "累计答题", value: String(stats.totalAnswered) },
        { label: "答对", value: String(stats.totalCorrect) },
        { label: "正确率", value: stats.accuracyPct + "%" },
        { label: "连续天数", value: String(stats.streak) }
      ]),
      ui.panel({
        title: "各栈进度",
        children: rows.length ? rows : [ui.empty("还没有进度", "先答几道题")]
      })
    ])
  },

  onLoad () {
    this.setData({ stacks: khc.STACK_META })
    this.refresh()
  },

  onShow () {
    this.refresh()
  },

  onMount: function (rootElement) {
    this.rootElement = rootElement || this.rootElement
    if (!this.data.stacks.length) this.setData({ stacks: khc.STACK_META })
    this.refresh()
    this.refreshTheme()
  },

  refresh () {
    const plan = stores.quizzes.getTodayPlan()
    const stats = stores.quizzes.getStats()

    this.setData({
      plan: {
        stack: plan.stack,
        stackLabel: khc.stackMeta(plan.stack).label,
        done: plan.completed || 0,
        total: plan.questionIds.length
      },
      planPercent: plan.questionIds.length ? Math.round((plan.completed || 0) / plan.questionIds.length * 100) : 0,
      questions: this.filteredQuestions(),
      wrongs: stores.quizzes.getWrongBook(),
      stats: {
        totalAnswered: stats.totalAnswered,
        totalCorrect: stats.totalCorrect,
        streak: stats.streak,
        accuracy: stats.accuracy,
        accuracyPct: Math.round(stats.accuracy * 100)
      },
      progress: khc.STACK_META.map(function (s) {
        const p = stores.quizzes.getProgress(s.key)
        return {
          key: s.key, label: s.label, color: s.color,
          total: p.total, answered: p.answered, correct: p.correct,
          percent: p.total ? Math.round(p.answered / p.total * 100) : 0
        }
      })
    })
  },

  filteredQuestions () {
    const stack = this.data.activeStack
    const q = this.data.query
    let list = q ? stores.quizzes.searchQuestions(q) : stores.quizzes.getQuestionBank()
    if (stack) list = list.filter(function (item) { return item.stack === stack })
    return list.map(function (item) {
      const meta = khc.stackMeta(item.stack)
      const diff = khc.quizDifficulty(item.difficulty)
      return {
        id: item.id,
        stem: item.stem,
        scenario: item.scenario,
        stackLabel: meta.label,
        stackColor: meta.color,
        difficulty: diff.label,
        difficultyColor: diff.color,
        type: item.type
      }
    })
  },

  /* ---- 模式切换 ---- */

  onMode (e) {
    this.setData({ mode: e.currentTarget.dataset.mode })
    this.refresh()
  },

  onPickStack (e) {
    const key = e.currentTarget.dataset.key
    this.setData({ activeStack: this.data.activeStack === key ? "" : key })
    this.setData({ questions: this.filteredQuestions() })
  },

  onSearch (e) {
    this.setData({ query: e.detail.value })
    this.setData({ questions: this.filteredQuestions() })
  },

  /* ---- 开始会话 ---- */

  onStartPlan () {
    const plan = stores.quizzes.getTodayPlan()
    this.beginSession(plan.questionIds, "plan")
  },

  onRegeneratePlan () {
    stores.quizzes.generateDailyPlan()
    this.refresh()
    platform.showToast({ title: "已换一批", icon: "none" })
  },

  onStartStack (e) {
    const key = e.currentTarget.dataset.key
    const ids = stores.quizzes.getQuestionBank()
      .filter(function (q) { return q.stack === key })
      .slice(0, 20)
      .map(function (q) { return q.id })
    if (!ids.length) {
      platform.showToast({ title: "该栈暂无题目", icon: "none" })
      return
    }
    this.beginSession(ids, "list")
  },

  onStartWrong () {
    const ids = this.data.wrongs.slice(0, 10).map(function (w) { return w.questionId })
    if (!ids.length) {
      platform.showToast({ title: "错题本是空的", icon: "none" })
      return
    }
    this.beginSession(ids, "wrong")
  },

  onAnswerOne (e) {
    this.beginSession([e.currentTarget.dataset.id], "list")
  },

  beginSession (ids, mode) {
    const qs = ids.map(function (id) { return stores.quizzes.getQuestionById(id) }).filter(Boolean)
    this.setData({ mode: "quiz", session: qs, index: 0, picked: "", result: null })
    this.renderQuestion()
  },

  /* ---- 答题 ---- */

  renderQuestion () {
    const q = this.data.session[this.data.index]
    if (!q) return
    const diff = khc.quizDifficulty(q.difficulty)
    const meta = khc.stackMeta(q.stack)

    // 选择题 / 判断题渲染成可点选项
    let options = []
    if (q.type === "选择题" && q.options) {
      options = q.options.map(function (text, i) {
        return { text: text, letter: String.fromCharCode(65 + i) }
      })
    } else if (q.type === "判断题") {
      options = [{ text: "正确", letter: "T" }, { text: "错误", letter: "F" }]
    }

    this.setData({
      picked: "",
      result: null,
      q: {
        id: q.id,
        type: q.type,
        stem: q.stem,
        scenario: q.scenario,
        code: q.code || "",
        stackLabel: meta.label,
        difficulty: diff.label,
        difficultyColor: diff.color,
        answerText: q.type === "选择题" ? q.answer : "",
        explain: q.explanation || "",
        position: (this.data.index + 1) + " / " + this.data.session.length
      },
      options: options
    })
  },

  onPickOption (e) {
    if (this.data.result) return
    this.setData({ picked: e.currentTarget.dataset.letter })
  },

  onSubmit () {
    const q = this.data.session[this.data.index]
    if (!q) return
    const picked = this.data.picked
    if (!picked) {
      platform.showToast({ title: "先选择一个答案", icon: "none" })
      return
    }

    const rec = stores.quizzes.submitAnswer({ questionId: q.id, userAnswer: picked })
    stores.daily.recordStudy(q.id, rec.correct)
    if (this.data.mode === "plan") stores.quizzes.markPlanProgress(q.id)

    this.setData({
      result: {
        correct: rec.correct,
        right: q.type === "判断题" ? (q.answer ? "正确" : "错误") : q.answer,
        explain: q.explanation || ""
      }
    })
    platform.vibrateShort({ type: rec.correct ? "medium" : "heavy" })
  },

  onNext () {
    const next = this.data.index + 1
    if (next >= this.data.session.length) {
      this.setData({ mode: "plan", session: null })
      this.refresh()
      platform.showToast({ title: "本轮完成", icon: "none" })
      return
    }
    this.setData({ index: next })
    this.renderQuestion()
  },

  onExitQuiz () {
    this.setData({ mode: "plan", session: null, index: 0 })
    this.refresh()
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 题库练习", path: "/knowledge/pages/learn/quiz/index" }
  }
})
