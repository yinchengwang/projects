"use strict"
/**
 * session.js —— 会话队列式首页（#/"session"）
 *
 * 这一页回答一个问题：现在该学什么、按什么顺序、还剩多少。
 * 版式按设计文档 §8.2「首页（会话队列式）」：
 *   顶部提示「上次还有 N 个没做完」
 *   主卡：当前会话的队列清单——已完成的打勾、当前的标黄、没开始的置灰
 *   下方：会话信息 + 三轨概况（背景信息）
 *
 * P1 只排不判：队列里的「完成这个」只把单元标记为完成，不跑断言。
 * 单元学习页（左读右做）是 P2 的事，这一期没有——所以主卡里给的是
 * 「完成这个」而不是「打开这一题」。
 */

;(function () {
  const A = window.StudyHub || window.AlgoPlan
  const el = A.el

  /**
   * 公共界面构件。浏览器里 ui.js 先加载挂 window.AlgoPlanUI；
   * Node 下没有全局挂载，就 require 同一份文件——实现只有一份。
   */
  const UI =
    (typeof window !== "undefined" && (window.AlgoPlanUI || (A && A.ui))) ||
    (typeof require === "function" ? require("../ui.js") : null)

  /** 队列状态 → 符号 / 中文 / 色调 / 样式类 */
  const STATE_TEXT = {
    done: { mark: "✓", label: "已完成", cls: "queue-done", tone: "ok" },
    current: { mark: "▶", label: "正在学", cls: "queue-current", tone: "warn" },
    pending: { mark: "○", label: "还没开始", cls: "queue-pending", tone: "" },
    skipped: { mark: "—", label: "已跳过", cls: "queue-skipped", tone: "" },
  }

  /** 轨代码 → 中文标签 */
  const TRACK_LABEL = { algorithm: "算法", database: "数据库", ai: "AI" }

  /**
   * 会话行 → 给界面看的描述。
   *
   * 纯函数：不碰 DOM，能单测。title 取不到时回落成 unit_id——
   * 单元不在 units 表里也是这个回落。
   *
   * @param {object} item 队列项 `{ unit_id, seq, state, title, track }`
   * @param {number} index 下标（seq 缺失时用它兜底）
   * @returns {{seq: number, unit_id: string, title: string, track: string, state: string, mark: string, label: string, tone: string, cls: string}}
   */
  function describeQueueItem(item, index) {
    const it = item || {}
    const state = String(it.state || "pending")
    const conf = STATE_TEXT[state] || STATE_TEXT.pending
    const seq = Number(it.seq)
    return {
      seq: seq > 0 ? seq : Number(index) + 1,
      unit_id: String(it.unit_id || ""),
      title: String(it.title || it.unit_id || ""),
      track: String(it.track || ""),
      state: state,
      mark: conf.mark,
      label: conf.label,
      tone: conf.tone,
      cls: conf.cls,
    }
  }

  /**
   * 队列 → 行数组。
   * @param {Array<object>} queue
   * @returns {Array<object>}
   */
  function buildQueueRows(queue) {
    return (queue || []).map(function (item, index) {
      return describeQueueItem(item, index)
    })
  }

  /**
   * 队列概况：总共几个、做完几个、正在学几个、还剩几个。
   * @param {Array<object>} rows
   * @returns {{total: number, done: number, current: number, left: number}}
   */
  function queueSummary(rows) {
    const list = rows || []
    let done = 0
    let current = 0
    list.forEach(function (row) {
      if (row.state === "done") done += 1
      else if (row.state === "current") current += 1
    })
    return { total: list.length, done: done, current: current, left: list.length - done }
  }

  /**
   * 每轨这次排了几个、该轨一共能学几个。
   * 关掉的轨也摆出来（planned 照实数），标成「已关闭」。
   * @param {Array<object>} tracks 设置里的轨清单
   * @param {Array<object>} rows 队列行
   * @returns {Array<{code: string, label: string, planned: number, unit_count: number, enabled: boolean}>}
   */
  function trackSummary(tracks, rows) {
    const queue = rows || []
    return (tracks || []).map(function (track) {
      const t = track || {}
      const code = String(t.code || "")
      let planned = 0
      queue.forEach(function (row) {
        if (row.track === code) planned += 1
      })
      return {
        code: code,
        label: String(t.label || TRACK_LABEL[code] || code),
        planned: planned,
        unit_count: Number(t.unit_count || 0),
        enabled: t.enabled === false ? false : true,
      }
    })
  }

  /**
   * 顶部提示文案。服务端给了 notice 就用它，没给就自己算。
   * @param {object} payload GET /api/session 的响应
   * @param {{total: number, done: number, current: number, left: number}} summary
   * @returns {string}
   */
  function noticeText(payload, summary) {
    const given = String((payload && payload.notice) || "")
    if (given !== "") return given
    const left = summary ? Number(summary.left || 0) : 0
    return left > 0 ? "还有 " + left + " 个没做完" : ""
  }

  /**
   * 页面状态：三选一，别用 `queue.length === 0` 一把梭。
   *
   *   null              → 还没有会话（库里一条都没有）
   *   "empty-pool"      → 有会话但一个单元都没排出来（候选池空）
   *   "done"            → 会话还在、队列也还在，但全做完了
   *   "active"          → 还有没做完的
   *
   * **为什么不能用 `queue.length === 0` 判「学完了」**：
   * `GET /api/session` 返回的是**最新一条会话**（不管它关没关），`queue` 是**全部**队列项
   * （含 `state: "done"` 的）。所以「全做完」时 queue 仍有 N 条 done 项，
   * `length === 0` 永远不会成立 —— 那条件写出来就是死分支。
   * （契约 §8.2 明写了这条口径。）
   *
   * @param {object} payload GET /api/session 的响应
   * @param {{total: number, done: number, current: number, left: number}} summary
   * @returns {"idle"|"empty-pool"|"done"|"active"}
   */
  function pageState(payload, summary) {
    const p = payload || {}
    if (!p.session) return "idle"
    const total = summary ? Number(summary.total || 0) : 0
    const left = summary ? Number(summary.left || 0) : 0
    if (total === 0) return "empty-pool"
    if (left === 0) return "done"
    return "active"
  }

  /**
   * 把服务端的 warnings 摊平成可显示的句子。
   * 后端有好几处只把话说在 warnings 里（候选池空、复用了旧会话、numpy 缺失跳过），
   * 不显示就等于没说 —— 用户会以为按钮坏了。
   * @param {object} payload 任意接口响应
   * @returns {string[]}
   */
  function warningsText(payload) {
    const list = payload && Array.isArray(payload.warnings) ? payload.warnings : []
    return list
      .map(function (w) {
        return String(w || "").trim()
      })
      .filter(function (w) {
        return w !== ""
      })
  }

  /**
   * 会话里的轨道列表——库里存的是 JSON 字符串，接口也可能给数组，两种都收。
   * @param {unknown} value
   * @returns {string[]}
   */
  function parseTracks(value) {
    if (Array.isArray(value)) {
      return value.map(function (v) {
        return String(v)
      })
    }
    if (typeof value === "string" && value !== "") {
      try {
        const parsed = JSON.parse(value)
        if (Array.isArray(parsed)) {
          return parsed.map(function (v) {
            return String(v)
          })
        }
      } catch (err) {
        return value
          .split(",")
          .map(function (v) {
            return v.trim()
          })
          .filter(function (v) {
            return v !== ""
          })
      }
    }
    return []
  }

  /** 一行队列 */
  function queueRowNode(row, view) {
    const children = [
      el("span", { class: "queue-mark", text: row.mark }),
      el("span", { class: "queue-title", text: row.title }),
      el("span", { class: "queue-meta", text: TRACK_LABEL[row.track] || row.track || "" }),
      UI.pill(row.label, row.tone),
    ]
    if (row.state === "current") {
      children.push(
        el("button", {
          class: "queue-action",
          text: "完成这个",
          onclick: function () {
            completeUnit(view, row.unit_id)
          },
        })
      )
    }
    return el("div", { class: "queue-item " + row.cls }, children)
  }

  /** 主卡：当前会话的队列 */
  function queueHero(payload, rows, summary, view) {
    const children = [
      UI.statGrid([
        { label: "本次共", value: summary.total + " 个", tone: "accent" },
        { label: "已完成", value: summary.done + " 个", tone: "ok" },
        { label: "正在学", value: summary.current + " 个", tone: summary.current > 0 ? "warn" : "" },
        { label: "还剩", value: summary.left + " 个" },
      ]),
      el("div", { class: "queue" }, rows.map(function (row) {
        return queueRowNode(row, view)
      })),
    ]
    const actions = [
      el("button", {
        text: "暂停会话",
        onclick: function () {
          pauseSession(view)
        },
      }),
    ]
    children.push(el("div", { class: "row" }, actions))
    return UI.hero({
      title: "本次会话的队列",
      sub: "按跨轨权重排好的一条队列——已完成打勾、正在学标黄、没开始置灰。",
      children: children,
    })
  }

  /** 普通卡：会话本身的信息 */
  function sessionPanel(payload) {
    const session = (payload && payload.session) || {}
    const tracks = parseTracks(session.tracks).map(function (code) {
      return TRACK_LABEL[code] || code
    })
    return UI.panel({
      title: "会话信息",
      children: [
        UI.kv([
          ["会话编号", String(session.id || "")],
          ["开始时间", String(session.started_at || "")],
          ["计划单元", String(session.planned_count || 0) + " 个"],
          ["已完成", String(session.done_count || 0) + " 个"],
          ["涉及轨道", tracks.length > 0 ? tracks.join("、") : "—"],
        ]),
      ],
    })
  }

  /** 静默卡：三轨概况——背景信息，不抢主卡的注意力 */
  function trackPanel(tracks) {
    const items = tracks.map(function (track) {
      return {
        label: track.label,
        value: track.planned + " 个",
        hint: track.enabled ? "可学 " + track.unit_count + " 个" : "已关闭",
        tone: track.enabled ? "" : "warn",
      }
    })
    return UI.panel({
      title: "三轨概况",
      className: "quiet",
      children: [
        UI.statGrid(items),
        el("p", {
          class: "faint",
          text: "这里摆的是「这次排了什么、各轨还能学多少」；掌握度要到诊断页看。",
        }),
      ],
    })
  }

  /** 还没有会话（或这次的队列全空了）时的空态：给一个开始按钮 */
  function emptyHero(settings, view, title, sub) {
    const sessionCfg = (settings && settings.session) || {}
    const defaultCount = Number(sessionCfg.default_count || 3)
    let totalUnits = 0
    ;((settings && settings.tracks) || []).forEach(function (track) {
      totalUnits += Number((track && track.unit_count) || 0)
    })
    return UI.hero({
      title: title,
      sub: sub,
      children: [
        UI.statGrid([
          { label: "默认每次", value: defaultCount + " 个单元", tone: "accent" },
          { label: "可学单元", value: totalUnits + " 个" },
          { label: "已排队列", value: "0 个" },
        ]),
        el("div", { class: "row" }, [
          el("button", {
            class: "primary",
            text: "开始一次学习",
            onclick: function () {
              startSession(view, defaultCount)
            },
          }),
        ]),
      ],
    })
  }

  /** 出错时的提示（不崩页面） */
  function errorPanel(title, err) {
    return el("section", { class: "panel" }, [
      el("h2", { text: title }),
      el("p", { class: "error", text: String((err && err.message) || err) }),
    ])
  }

  /** 开始一次会话 */
  async function startSession(view, count) {
    let started = null
    try {
      started = await A.postJson("/api/session/start", { n: count })
    } catch (err) {
      view.textContent = ""
      view.appendChild(errorPanel("开始失败", err))
      return
    }
    // 排不出单元时 start 返回 `session: null` + warnings（不是错误码）。
    // 这条响应**不能丢**：warnings 里那句「没有可学的单元，建议去供给流水线补内容」
    // 是用户唯一能看到的解释。丢掉它再看一次 GET /api/session，页面就停在同一屏、
    // 用户只会觉得按钮坏了。把它带到下一屏去。
    const warnings = started && Array.isArray(started.warnings) ? started.warnings : []
    await render(view, warnings)
  }

  /** 把当前单元标记为完成（P1 只排不判） */
  async function completeUnit(view, unitId) {
    try {
      await A.postJson("/api/session/submit", { unit_id: unitId })
    } catch (err) {
      view.textContent = ""
      view.appendChild(errorPanel("提交失败", err))
      return
    }
    await render(view)
  }

  /** 暂停会话：队列留着，下次接着学 */
  async function pauseSession(view) {
    try {
      await A.postJson("/api/session/pause", {})
    } catch (err) {
      view.textContent = ""
      view.appendChild(errorPanel("暂停失败", err))
      return
    }
    await render(view)
  }

  /**
   * 渲染首页。
   * @param {HTMLElement} view
   * @param {string[]} [extraWarnings] 上一次操作带过来的 warnings（如 start 排不出单元时那句）
   * @returns {Promise<void>}
   */
  async function render(view, extraWarnings) {
    view.textContent = ""
    view.appendChild(el("div", { class: "panel", text: "正在加载本次学习…" }))
    let payload = null
    let settings = null
    try {
      payload = await A.getJson("/api/session")
      settings = await A.getJson("/api/settings")
    } catch (err) {
      view.textContent = ""
      view.appendChild(errorPanel("加载失败", err))
      return
    }
    const data = payload || {}
    const cfg = settings || {}
    const rows = buildQueueRows(data.queue)
    const summary = queueSummary(rows)
    const tracks = trackSummary(cfg.tracks, rows)

    view.textContent = ""
    view.appendChild(
      UI.pageHead({
        eyebrow: "学习中枢",
        title: "本次学习",
        sub: "会话按单元数计，不按天——点开就开始学，中断了也不会丢。",
      })
    )

    const notice = noticeText(data, summary)
    if (notice !== "") {
      view.appendChild(
        el("div", { class: "notice" }, [
          el("p", { class: "notice-title", text: "接着上次" }),
          el("p", { class: "notice-text", text: notice }),
        ])
      )
    }

    // 服务端把话说在 warnings 里的那几种情形，必须显出来 —— 否则用户只会觉得「点了没反应」
    const warningLines = warningsText(data).concat(
      Array.isArray(extraWarnings)
        ? extraWarnings
            .map(function (w) {
              return String(w || "").trim()
            })
            .filter(function (w) {
              return w !== ""
            })
        : []
    )
    if (warningLines.length > 0) {
      view.appendChild(
        el("div", { class: "notice" }, [
          el("p", { class: "notice-title", text: "要注意" }),
        ].concat(
          warningLines.map(function (line) {
            return el("p", { class: "notice-text", text: line })
          })
        ))
      )
    }

    const state = pageState(data, summary)
    if (state === "idle") {
      view.appendChild(
        emptyHero(
          cfg,
          view,
          "还没有会话",
          "点「开始一次学习」就会按跨轨权重排一条队列——算法、数据库、AI 混着排，中断了也不会丢。"
        )
      )
    } else if (state === "empty-pool") {
      // 有会话但一个单元都没排出来：给「为什么」和「怎么办」，不是干巴巴一个按钮
      view.appendChild(
        emptyHero(
          cfg,
          view,
          "这次排不出单元",
          "候选池里没有可学的单元了。去「供给」补内容，或在设置里打开被关掉的轨，再点一次。"
        )
      )
    } else if (state === "done") {
      view.appendChild(
        emptyHero(cfg, view, "这次学完了", "这次的单元都做完了。点「开始一次学习」排下一次。")
      )
    } else {
      view.appendChild(queueHero(data, rows, summary, view))
      view.appendChild(sessionPanel(data))
    }
    view.appendChild(trackPanel(tracks))
  }

  A.pages.session = {
    title: "本次学习",
    render: render,
  }

  // Node（单测）下导出纯函数与渲染函数——老测试直接用同一份实现
  if (typeof module !== "undefined" && module.exports) {
    module.exports = {
      describeQueueItem: describeQueueItem,
      buildQueueRows: buildQueueRows,
      queueSummary: queueSummary,
      trackSummary: trackSummary,
      noticeText: noticeText,
      pageState: pageState,
      warningsText: warningsText,
      parseTracks: parseTracks,
      render: render,
    }
  }
})()
