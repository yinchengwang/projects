"use strict"
/**
 * app.js —— 前端逻辑（M3：今日页 + 题库页 + 作答页 + 判题结果面板）
 *
 * 原生 JS，无构建步骤。后续里程碑在这里接复习 / 轮次 / 诊断页。
 *
 * 判题契约（要显示给用户看）：
 *   提交的是完整的 stdin→stdout 程序，必须自己写 main()。
 *   系统不会替你生成入口，也不会注入 Solution 包装。
 */

/** verdict → 面板色调（与 judge/runner.js 的 VERDICT_PANEL.tone 对应） */
const TONE_BY_VERDICT = {
  pass: "ok",
  wrong_answer: "bad",
  timeout: "warn",
  compile_error: "bad",
  runtime_error: "bad",
}

/** 面板里期望/实际最多显示多少字符 */
const PANEL_TEXT_LIMIT = 400

/** 前端状态：当前正在作答的题目与语言 */
const state = {
  problemId: null,
  language: "cpp",
  // 按题号存草稿：切页签、切题目再切回来，用户写的代码不能丢
  draft: {},
  // 按题号存上一次的判题响应：切页签回来还能看到结果面板
  result: {},
}

/** 当前视图：today / bank / practice */
let currentView = "today"

/**
 * 渲染序号。每次 render() 自增，用来挡掉异步竞态：
 * 后发起的 render 序号更大，先发起的 await 回来后发现序号变了就直接放弃，
 * 不会把过期的面板追加到已经切走的视图里。
 */
let renderSeq = 0

/** 极简 DOM 构造器 */
function el(tag, attrs, children) {
  const node = document.createElement(tag)
  if (attrs) {
    Object.keys(attrs).forEach(function (key) {
      if (key === "class") node.className = attrs[key]
      else if (key === "text") node.textContent = attrs[key]
      else node.setAttribute(key, attrs[key])
    })
  }
  ;(children || []).forEach(function (child) {
    node.appendChild(child)
  })
  return node
}

/** 取 JSON，失败时抛出带中文说明的错误 */
async function getJson(url) {
  const res = await fetch(url)
  const body = await res.json()
  if (!res.ok) {
    throw new Error((body && body.error) || "请求失败：" + res.status)
  }
  return body
}

/** 提交 JSON，失败时抛出带中文说明的错误 */
async function postJson(url, body) {
  const res = await fetch(url, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  })
  const data = await res.json()
  if (!res.ok) {
    throw new Error((data && data.error) || "请求失败：" + res.status)
  }
  return data
}

/** 截断过长文本并注明被截了多少 */
function truncate(text, limit) {
  const s = String(text === null || text === undefined ? "" : text)
  if (s.length <= limit) return s
  return s.slice(0, limit) + "\n…（已截断，原文共 " + s.length + " 字符）"
}

/**
 * 把判题结果整理成结果面板的数据（纯函数，不碰 DOM，便于单测）
 * @param {object} result POST /api/attempt 的响应
 */
function describePanel(result) {
  const r = result || {}
  const verdict = String(r.verdict || "")
  const failedCase = r.failedCase || null
  return {
    verdict: verdict,
    tone: TONE_BY_VERDICT[verdict] || "",
    title: String(r.message || ""),
    rows: [
      {
        label: "用例",
        value: String(r.passedCases || 0) + " / " + String(r.totalCases || 0) + " 通过",
      },
      { label: "耗时", value: String(r.durationMs || 0) + " ms" },
    ],
    failed: failedCase
      ? {
          index: failedCase.index,
          input: truncate(failedCase.input, PANEL_TEXT_LIMIT),
          expected: truncate(failedCase.expected, PANEL_TEXT_LIMIT),
          actual: truncate(failedCase.actual, PANEL_TEXT_LIMIT),
        }
      : null,
    compilerOutput: truncate(r.compilerOutput || "", PANEL_TEXT_LIMIT * 3),
  }
}

/** 渲染顶栏页签 */
function renderTabs() {
  const nav = document.getElementById("tabs")
  nav.textContent = ""
  const views = [
    { key: "today", label: "今日" },
    { key: "bank", label: "题库" },
    { key: "practice", label: "作答" },
  ]
  views.forEach(function (item) {
    const btn = document.createElement("button")
    btn.textContent = item.label
    if (item.key === currentView) btn.className = "active"
    btn.addEventListener("click", function () {
      currentView = item.key
      render()
    })
    nav.appendChild(btn)
  })
}

/** 渲染健康状态面板 */
function renderHealth(health) {
  const learning = health.learning || {}
  const bank = health.bank || {}
  const badge = learning.ok
    ? el("span", { class: "badge ok", text: "已连接 learning 仓" })
    : el("span", { class: "badge warn", text: "纯本地模式" })
  const rows = [
    el("div", {}, [document.createTextNode("Node："), el("span", { text: health.node || "?" })]),
    el("div", {}, [document.createTextNode("题库题目数："), el("span", { text: String(bank.problemCount || 0) })]),
    el("div", {}, [document.createTextNode("作答流水条数："), el("span", { text: String(health.attempts || 0) })]),
    el("div", {}, [document.createTextNode("learning 仓："), el("span", { text: learning.root || "（未连接）" })]),
  ]
  return el("section", { class: "panel" }, [
    el("h2", { text: "运行状态" }),
    el("div", { class: "kv" }, rows),
    el("p", { class: "muted", text: learning.reason || "" }),
    badge,
  ])
}

/**
 * 题目 → 原题入口。
 *
 * 有直达地址就给「去原题」，只有搜索地址就退到「去源站搜索」，
 * 两个都没有就明说没有——不装作有链接，也不给个死链。
 *
 * @param {object} item 题库条目（summary 或 problem 都行）
 * @returns {{url: string, label: string, kind: string}}
 */
function describeSourceLink(item) {
  const url = String((item && item.url) || "").trim()
  const searchUrl = String((item && item.searchUrl) || "").trim()
  if (url) return { url: url, label: "去原题", kind: "direct" }
  if (searchUrl) return { url: searchUrl, label: "去源站搜索", kind: "search" }
  return { url: "", label: "暂无链接", kind: "none" }
}

/** 造一个「跳原题」的链接元素 */
function renderSourceLink(item) {
  const link = describeSourceLink(item)
  if (link.kind === "none") {
    return el("span", { class: "muted", text: link.label })
  }
  const a = el("a", {
    class: "link",
    href: link.url,
    target: "_blank",
    rel: "noopener noreferrer",
    text: link.label,
  })
  return a
}

/** 渲染题库列表（每行带一个「作答」按钮 + 原题入口） */
function renderBank(bank) {
  const items = (bank && bank.items) || []
  const rows = items.map(function (item) {
    const topics = (item.topics || []).map(function (t) {
      return el("span", { class: "badge", text: t })
    })
    const go = el("button", { text: "作答" })
    go.addEventListener("click", function () {
      state.problemId = item.id
      currentView = "practice"
      render()
    })
    return el("tr", {}, [
      el("td", { text: item.id }),
      el("td", { text: item.title }),
      el("td", { text: item.difficulty }),
      el("td", {}, topics),
      el("td", {}, [
        el("span", {
          class: item.needsFill ? "badge warn" : "badge",
          text: item.needsFill ? "待补全" : item.status,
        }),
      ]),
      el("td", {}, [renderSourceLink(item)]),
      el("td", {}, [go]),
    ])
  })
  return el("section", { class: "panel" }, [
    el("h2", { text: "题库（" + (bank ? bank.total : 0) + "）" }),
    items.length === 0
      ? el("p", {
          class: "muted",
          text: "题库还是空的。设置 ALGO_PLAN_LEARNING_ROOT 后运行 node tools/seed-bank.js 导入样板题。",
        })
      : el("table", {}, [
          el("thead", {}, [
            el("tr", {}, [
              el("th", { text: "ID" }),
              el("th", { text: "标题" }),
              el("th", { text: "难度" }),
              el("th", { text: "知识点" }),
              el("th", { text: "状态" }),
              el("th", { text: "原题" }),
              el("th", { text: "操作" }),
            ]),
          ]),
          el("tbody", {}, rows),
        ]),
  ])
}

/** 渲染判题结果面板 */
function renderVerdictPanel(result) {
  const p = describePanel(result)
  const children = [
    el("h2", { text: p.title || "判题结果" }),
    el("div", { class: "kv" }, p.rows.map(function (row) {
      return el("div", {}, [
        document.createTextNode(row.label + "："),
        el("span", { text: row.value }),
      ])
    })),
  ]
  if (p.verdict) {
    children.push(el("span", { class: "badge " + (p.tone || ""), text: p.verdict }))
  }
  if (p.failed) {
    children.push(el("h2", { text: "失败用例" }))
    children.push(el("div", { class: "kv" }, [
      el("div", {}, [
        document.createTextNode("第 " + p.failed.index + " 个用例输入："),
        el("span", { text: p.failed.input }),
      ]),
      el("div", {}, [
        document.createTextNode("期望："),
        el("span", { text: p.failed.expected }),
      ]),
      el("div", {}, [
        document.createTextNode("实际："),
        el("span", { text: p.failed.actual }),
      ]),
    ]))
  }
  if (p.compilerOutput) {
    children.push(el("h2", { text: "编译 / 错误原文" }))
    children.push(el("pre", { class: "raw", text: p.compilerOutput }))
  }
  children.push(
    el("p", {
      class: "muted",
      text: "提示：判题不是安全沙箱，只防跑飞与死循环，不防恶意代码。",
    })
  )
  return el("section", { class: "panel" }, children)
}

/**
 * 题面缺失时的说明块。
 *
 * 为什么不能只留一句「（题面为空）」：
 *   存量题库里大部分题只有题号和标题，正文要跳到源站读。
 *   白屏式的「为空」等于把用户堵死在门外——他既不知道要去哪读，
 *   也不知道读完该干嘛。这里把状态、去处、下一步都写明白。
 */
function renderMissingStatement(problem) {
  const link = describeSourceLink(problem)
  const source = String(problem.source || "")
  const sourceId = String(problem.sourceId || "")
  const children = [
    el("h3", { class: "notice-title", text: "题面还没进题库" }),
    el("p", {
      class: "notice-text",
      text:
        "这道题在本地题库里只有题号和标题，正文在源站上。" +
        (link.kind === "none"
          ? "它也还没有原题链接，先按下面的题号去源站搜一下。"
          : "点下面的链接去源站读题，读懂后回到这里写代码提交判题。"),
    }),
  ]

  if (source || sourceId) {
    children.push(
      el("p", {
        class: "muted",
        text: "源站题号：" + (source || "未知来源") + (sourceId ? " / " + sourceId : ""),
      })
    )
  }

  if (link.kind !== "none") {
    children.push(
      el("p", {}, [
        el("a", {
          class: "link strong",
          href: link.url,
          target: "_blank",
          rel: "noopener noreferrer",
          text: link.label,
        }),
      ])
    )
  } else {
    children.push(
      el("p", {
        class: "muted",
        text: "提示：运行 node tools/sync-links.js 可以给题库回填原题链接。",
      })
    )
  }

  return el("div", { class: "notice" }, children)
}

/** 渲染作答页：题面 + 样例 + 语言选择 + 代码框 + 提交按钮 + 结果面板 */
async function renderPractice() {
  const section = el("section", { class: "panel" })

  if (!state.problemId) {
    section.appendChild(el("h2", { text: "作答" }))
    section.appendChild(
      el("p", { class: "muted", text: "先在「题库」里点一道题的「作答」按钮。" })
    )
    return section
  }

  const problem = await getJson("/api/problem/" + encodeURIComponent(state.problemId))
  const samples = (problem.cases && problem.cases.sample) || []
  section.appendChild(el("h2", { text: problem.title + "（" + problem.id + "）" }))

  // 标题旁边常驻一个原题入口：有题面时也能跳过去核对原文
  const link = describeSourceLink(problem)
  if (link.kind !== "none") {
    section.appendChild(
      el("p", { class: "muted" }, [
        document.createTextNode("原题："),
        el("a", {
          class: "link",
          href: link.url,
          target: "_blank",
          rel: "noopener noreferrer",
          text: link.label,
        }),
      ])
    )
  }

  const statement = String(problem.statement || "").trim()
  if (statement) {
    section.appendChild(el("pre", { class: "statement", text: statement }))
  } else {
    section.appendChild(renderMissingStatement(problem))
  }

  section.appendChild(
    el("p", {
      class: "muted",
      text:
        "共 " +
        problem.cases.total +
        " 个用例，界面只展示前 " +
        samples.length +
        " 个样例（全部用例由服务端持有）。",
    })
  )

  // 样例用例：接口只放行前 2 个，展示出来是给用户对答案用的
  if (samples.length > 0) {
    section.appendChild(el("h2", { text: "样例用例" }))
    samples.forEach(function (c, i) {
      section.appendChild(
        el("pre", {
          class: "raw",
          text:
            "用例 " +
            (i + 1) +
            "\n输入：" +
            (c.input || "（空）") +
            "\n期望：" +
            (c.expect || "（空）"),
        })
      )
    })
  }

  const langSelect = el("select", { id: "judge-language" }, [
    el("option", { value: "cpp", text: "C++" }),
    el("option", { value: "python", text: "Python" }),
  ])
  langSelect.value = state.language
  langSelect.addEventListener("change", function () {
    state.language = langSelect.value
  })

  const editor = el("textarea", { id: "judge-code", rows: "18", spellcheck: "false" })
  // 草稿按题号存：切页签、切题再切回来，用户写的代码还在
  editor.value = state.draft[state.problemId] || ""
  editor.addEventListener("input", function () {
    state.draft[state.problemId] = editor.value
  })

  const submit = el("button", { id: "judge-submit", text: "提交判题" })
  const resultBox = el("div", { id: "judge-result" })

  // 上一次的结果面板：切页签回来还在
  const lastResult = state.result[state.problemId]
  if (lastResult) resultBox.appendChild(renderVerdictPanel(lastResult))

  submit.addEventListener("click", async function () {
    submit.disabled = true
    submit.textContent = "判题中…"
    resultBox.textContent = ""
    state.draft[state.problemId] = editor.value
    try {
      const body = await postJson("/api/attempt", {
        problemId: state.problemId,
        code: editor.value,
        language: state.language,
        mode: "solve",
      })
      state.result[state.problemId] = body
      resultBox.appendChild(renderVerdictPanel(body))
    } catch (err) {
      resultBox.appendChild(
        el("section", { class: "panel" }, [
          el("h2", { text: "判题失败" }),
          el("p", { class: "error", text: String(err.message || err) }),
        ])
      )
    } finally {
      submit.disabled = false
      submit.textContent = "提交判题"
    }
  })

  section.appendChild(
    el("div", { class: "kv" }, [
      el("div", {}, [document.createTextNode("语言："), langSelect]),
    ])
  )
  section.appendChild(
    el("p", {
      class: "muted",
      text:
        "判题契约：提交的是完整的 stdin→stdout 程序，必须自己写 main()；" +
        "系统不会替你生成入口，也不会注入 Solution 包装。",
    })
  )
  section.appendChild(editor)
  section.appendChild(submit)
  section.appendChild(resultBox)

  return section
}

/** 主渲染 */
async function render() {
  const seq = ++renderSeq
  const view = document.getElementById("view")
  view.textContent = ""
  renderTabs()
  try {
    if (currentView === "today") {
      const health = await getJson("/api/health")
      if (seq !== renderSeq) return
      view.appendChild(renderHealth(health))
      const bank = await getJson("/api/bank")
      if (seq !== renderSeq) return
      view.appendChild(renderBank(bank))
    } else if (currentView === "bank") {
      const bank = await getJson("/api/bank")
      if (seq !== renderSeq) return
      view.appendChild(renderBank(bank))
    } else {
      const practice = await renderPractice()
      if (seq !== renderSeq) return
      view.appendChild(practice)
    }
  } catch (err) {
    if (seq !== renderSeq) return
    view.appendChild(
      el("section", { class: "panel" }, [
        el("h2", { text: "加载失败" }),
        el("p", { class: "error", text: String(err.message || err) }),
      ])
    )
  }
}

if (typeof document !== "undefined") {
  document.addEventListener("DOMContentLoaded", function () {
    render()
  })
}

// Node 下（测试用）可以 require 本文件取纯函数与渲染函数；浏览器里不会执行这一段
if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    describePanel: describePanel,
    describeSourceLink: describeSourceLink,
    truncate: truncate,
    TONE_BY_VERDICT: TONE_BY_VERDICT,
    PANEL_TEXT_LIMIT: PANEL_TEXT_LIMIT,
    // 以下只给测试用：让 DOM 冒烟测试能驱动真实的渲染路径
    __test: {
      state: state,
      render: render,
      renderBank: renderBank,
      renderPractice: renderPractice,
      renderVerdictPanel: renderVerdictPanel,
      renderMissingStatement: renderMissingStatement,
      renderSourceLink: renderSourceLink,
      setView: function (view) {
        currentView = view
      },
      getView: function () {
        return currentView
      },
    },
  }
}
