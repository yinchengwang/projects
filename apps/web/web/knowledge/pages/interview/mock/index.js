"use strict"
/**
 * 模拟面试 —— 配置 → 答题（倒计时）→ 评分报告 → 历史
 *
 * 评分为本地规则算法（原版未接真实 AI）：
 *   字数 < 20 → -20；无结构 → -10；否则按字数分档 90/75/60。
 */

const { withTheme } = require("../../../../../shared/core/with-theme.js")
const stores = require("../../../../../shared/knowledge/stores.js")
const seedData = require("../../../../../shared/knowledge/seed-data.js")
const date = require("../../../../../shared/core/date.js")
const ui = require("../../../../../shared/ui.js")
const platform = require("../../../../../shared/platform.js")
const syncTheme = require("../../../theme-bridge.js").syncTheme

const ROLES = ["后端开发", "前端开发", "算法工程师", "全栈工程师"]
const LEVELS = ["初级", "中级", "高级"]

/**
 * 本地规则评分（逐字对齐数据模型清单 C-9）。
 * @param {string} answer
 * @returns {{score: number, feedback: string, suggestions: string[]}}
 */
function evaluate (answer) {
  const text = String(answer || "").trim()
  let score = 100
  const suggestions = []

  if (text.length < 20) {
    score -= 20
    suggestions.push("回答过于简短，补充具体细节")
  }
  // 结构化信号：编号、分点、关键词连接
  const hasStructure = /\d+[.、)]|[•\-*]|首先|其次|然后|最后|综上/.test(text)
  if (!hasStructure) {
    score -= 10
    suggestions.push("建议分点作答，结构更清晰")
  }

  if (score >= 90) score = 90
  else if (score >= 75) score = 75
  else if (score >= 60) score = 60
  else score = Math.max(score, 40)

  const feedback = score >= 90
    ? "要点完整，条理清晰。"
    : (score >= 75 ? "主体正确，可再补充边界情况。" : "存在明显缺失，建议重新组织。")

  return { score: score, feedback: feedback, suggestions: suggestions }
}

module.exports = withTheme({
  data: {
    title: "模拟面试",
    roles: ROLES,
    levels: LEVELS,
    config: { role: "后端开发", difficulty: "中级", minutes: 20, count: 3 },
    step: "config",
    questions: [],
    index: 0,
    answers: [],
    remaining: 0,
    result: null,
    history: []
  },

  render: function (state) {
    const children = [
      ui.pageHead({ eyebrow: "Reading Radar", title: "模拟面试", sub: "配置 → 答题 → 评分报告" })
    ]

    if (state.step === "interview") children.push(this.renderInterview(state))
    else if (state.step === "result") children.push(this.renderResult(state))
    else children.push(this.renderConfig(state))

    children.push(this.renderHistory(state))
    return ui.el("div", { class: "kh-page" }, children)
  },

  /** 配置面板 */
  renderConfig: function (state) {
    const cfg = state.config || { role: ROLES[0], difficulty: LEVELS[1], minutes: 20, count: 3 }

    const roleButtons = (state.roles || ROLES).map(function (role) {
      const button = ui.el("button", {
        class: "kh-chip" + (cfg.role === role ? " is-active" : ""),
        text: role
      })
      button.addEventListener("click", function () {
        this.onPick({ currentTarget: { dataset: { field: "role", value: role } } })
      }.bind(this))
      return button
    })

    const levelButtons = (state.levels || LEVELS).map(function (level) {
      const button = ui.el("button", {
        class: "kh-chip" + (cfg.difficulty === level ? " is-active" : ""),
        text: level
      })
      button.addEventListener("click", function () {
        this.onPick({ currentTarget: { dataset: { field: "difficulty", value: level } } })
      }.bind(this))
      return button
    })

    const countButtons = [2, 3, 4].map(function (n) {
      const button = ui.el("button", {
        class: "kh-chip" + (cfg.count === n ? " is-active" : ""),
        text: n + " 题"
      })
      button.addEventListener("click", function () {
        this.onPick({ currentTarget: { dataset: { field: "count", value: String(n), type: "number" } } })
      }.bind(this))
      return button
    })

    const start = ui.el("button", {
      class: "kh-btn primary",
      text: "开始面试",
      onclick: function () { this.onStart() }.bind(this)
    })

    return ui.panel({
      title: "面试配置",
      children: [
        ui.el("p", { class: "kh-list-sub", text: "岗位" }),
        ui.el("div", { class: "kh-chips" }, roleButtons),
        ui.el("p", { class: "kh-list-sub", text: "难度" }),
        ui.el("div", { class: "kh-chips" }, levelButtons),
        ui.el("p", { class: "kh-list-sub", text: "题量" }),
        ui.el("div", { class: "kh-chips" }, countButtons),
        ui.el("div", { class: "kh-actions" }, [start])
      ]
    })
  },

  /** 答题面板 */
  renderInterview: function (state) {
    const q = (state.questions || [])[state.index]
    if (!q) return ui.panel({ title: "答题", children: [ui.empty("没有题目", "回到配置页重新开始")] })

    const area = ui.el("textarea", { class: "kh-textarea", placeholder: "在这里作答" })
    area.value = (state.answers || [])[state.index] || ""
    area.addEventListener("input", function (event) {
      this.onAnswer({ detail: { value: event.target.value } })
    }.bind(this))

    const minutes = Math.floor((state.remaining || 0) / 60)
    const seconds = String((state.remaining || 0) % 60).padStart(2, "0")

    const actions = [
      ui.el("button", { class: "kh-btn", text: "上一题", onclick: function () { this.onPrev() }.bind(this) }),
      ui.el("button", { class: "kh-btn primary", text: "下一题 / 交卷", onclick: function () { this.onNext() }.bind(this) }),
      ui.el("button", { class: "kh-btn", text: "提前交卷", onclick: function () { this.onFinish() }.bind(this) })
    ]

    return ui.panel({
      title: "第 " + (state.index + 1) + " / " + state.questions.length + " 题 · 剩余 " + minutes + ":" + seconds,
      children: [
        ui.el("p", { class: "kh-quiz-stem", text: q.question }),
        area,
        ui.el("div", { class: "kh-actions" }, actions)
      ]
    })
  },

  /** 评分报告 */
  renderResult: function (state) {
    const result = state.result || { score: 0, questions: [] }
    const rows = (result.questions || []).map(function (q) {
      const ev = q.evaluation || { score: 0, feedback: "", suggestions: [] }
      return ui.block({
        title: q.question,
        children: [
          ui.el("p", { class: "reason", text: "作答：" + (q.answer || "（未作答）") }),
          ui.pill("得分 " + ev.score, ev.score >= 75 ? "ok" : "warn"),
          ui.el("p", { class: "kh-list-sub", text: ev.feedback })
        ]
      })
    })

    const back = ui.el("button", { class: "kh-btn", text: "回到配置", onclick: function () { this.onBackConfig() }.bind(this) })

    return ui.el("div", {}, [
      ui.statGrid([{ label: "本次得分", value: String(result.score) }]),
      ui.panel({
        title: "评分报告",
        children: rows.length ? rows.concat([ui.el("div", { class: "kh-actions" }, [back])]) : [ui.empty("没有作答记录", "下次认真答几道")]
      })
    ])
  },

  /** 历史记录 */
  renderHistory: function (state) {
    const rows = (state.history || []).map(function (s) {
      const open = ui.el("button", { class: "kh-btn", text: "看报告" })
      open.addEventListener("click", function () {
        this.onOpenHistory({ currentTarget: { dataset: { id: s.id } } })
      }.bind(this))
      const del = ui.el("button", { class: "kh-btn danger", text: "删除" })
      del.addEventListener("click", function () {
        this.onDeleteHistory({ currentTarget: { dataset: { id: s.id } } })
      }.bind(this))
      return ui.el("div", { class: "kh-list-row" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: s.role + " · " + s.difficulty }),
          ui.el("div", { class: "kh-list-sub", text: (s.date || "") + " · " + (s.totalScore || 0) + " 分 · " + (s.duration || 0) + " 分钟" })
        ]),
        open,
        del
      ])
    })

    return ui.panel({
      title: "历史记录 · " + (state.history || []).length + " 场",
      children: rows.length ? rows : [ui.empty("还没有模拟面试记录", "完成一场就会出现在这里")]
    })
  },

  onMount: function (rootElement) {
    this.rootElement = rootElement || this.rootElement
    this.setData({ history: stores.mockInterviews.getAll() })
    syncTheme(this)
  },

  onLoad () {
    this.setData({ history: stores.mockInterviews.getAll() })
  },

  onShow () {
    this.setData({ history: stores.mockInterviews.getAll() })
  },

  onUnmount () {
    this.stopTimer()
  },

  onPick (e) {
    const ds = e.currentTarget.dataset
    const field = ds.field
    const value = ds.type === "number" ? Number(ds.value) : ds.value
    const patch = {}
    patch["config." + field] = value
    this.setData(patch)
  },

  /** 从面试题库按岗位抽题 */
  buildQuestions () {
    const cfg = this.data.config
    const pool = seedData.INTERVIEW_BANK.interview.concat(seedData.INTERVIEW_BANK.algorithm)
    const levelIdx = LEVELS.indexOf(cfg.difficulty)
    // 难度越高级别越靠后
    const ordered = pool.slice().sort(function (a, b) {
      const ai = LEVELS.indexOf(a.difficulty)
      const bi = LEVELS.indexOf(b.difficulty)
      return (ai >= levelIdx ? 1 : 0) - (bi >= levelIdx ? 1 : 0)
    })
    return ordered.slice(0, cfg.count).map(function (q) {
      return {
        id: q.id,
        question: q.question,
        reference: q.answer,
        answer: ""
      }
    })
  },

  onStart () {
    const questions = this.buildQuestions()
    if (!questions.length) {
      platform.showToast({ title: "题库为空", icon: "none" })
      return
    }
    this.setData({
      step: "interview",
      questions: questions,
      index: 0,
      answers: questions.map(function () { return "" }),
      remaining: this.data.config.minutes * 60
    })
    this.startTimer()
  },

  startTimer () {
    const self = this
    this.stopTimer()
    this.__timer = setInterval(function () {
      const r = self.data.remaining - 1
      if (r <= 0) {
        self.stopTimer()
        self.setData({ remaining: 0 })
        self.onFinish()
        return
      }
      self.setData({ remaining: r })
    }, 1000)
  },

  stopTimer () {
    if (this.__timer) {
      clearInterval(this.__timer)
      this.__timer = null
    }
  },

  onAnswer (e) {
    const answers = this.data.answers.slice()
    answers[this.data.index] = e.detail.value
    this.setData({ answers: answers })
  },

  onNext () {
    if (this.data.index < this.data.questions.length - 1) {
      this.setData({ index: this.data.index + 1 })
    } else {
      this.onFinish()
    }
  },

  onPrev () {
    if (this.data.index > 0) this.setData({ index: this.data.index - 1 })
  },

  onFinish () {
    this.stopTimer()
    const qs = this.data.questions
    const answers = this.data.answers

    let total = 0
    const evaluated = qs.map(function (q, i) {
      const ev = evaluate(answers[i])
      total += ev.score
      return {
        question: q.question,
        answer: answers[i] || "",
        evaluation: ev
      }
    })
    const avg = Math.round(total / (qs.length || 1))

    stores.mockInterviews.addSession({
      date: date.todayStr(),
      role: this.data.config.role,
      difficulty: this.data.config.difficulty,
      questions: evaluated,
      totalScore: avg,
      duration: this.data.config.minutes
    })

    this.setData({ step: "result", result: { score: avg, questions: evaluated } })
    this.setData({ history: stores.mockInterviews.getAll() })
  },

  onBackConfig () {
    this.stopTimer()
    this.setData({ step: "config", result: null })
  },

  onOpenHistory (e) {
    const id = e.currentTarget.dataset.id
    const s = stores.mockInterviews.getSessionById(id)
    if (!s) return
    this.setData({ step: "result", result: { score: s.totalScore, questions: s.questions, past: true } })
  },

  onDeleteHistory (e) {
    const id = e.currentTarget.dataset.id
    const self = this
    platform.showModal({
      title: "删除记录",
      content: "确定删除这场模拟面试？"
    }).then(function (res) {
      if (!res.confirm) return
      stores.mockInterviews.deleteSession(id)
      self.setData({ history: stores.mockInterviews.getAll() })
    })
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 模拟面试", path: "/knowledge/pages/interview/mock/index" }
  }
})
