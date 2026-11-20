"use strict"
/**
 * unit.js —— 单元学习页：左栏读、右栏做
 *
 * 一个单元有两种形态（契约 §1）：
 *   kind: "card"    知识卡 —— 左栏讲解，右栏跑 demo、看断言结果
 *   kind: "problem" 算法题 —— 左栏题面，右栏代码编辑区 + 判题结果
 *
 * 页面只读单元；唯一的写动作是「完成本单元」（POST /api/units/:id/complete）。
 *
 * 题面是服务端给的 statement_html（契约 §8.1）。
 * DOM 桩没有 innerHTML（契约 §9.4），所以**不用 innerHTML**：
 * 先把 HTML 解析成节点描述树（parseHtmlNodes，纯函数），再逐节点拼 DOM。
 *
 * 分工：
 *   纯函数（buildUnitView / parseHtmlNodes / decodeEntities / splitDemoOutput）——可单测
 *   渲染层（renderUnit / renderPage）——只做 DOM 装配
 */

;(function () {
  /** 全局对象：P1 起是 window.StudyHub；过渡期兼容 window.AlgoPlan */
  const A =
    (typeof window !== "undefined" && (window.StudyHub || window.AlgoPlan)) ||
    (typeof globalThis !== "undefined" && (globalThis.StudyHub || globalThis.AlgoPlan)) ||
    {}

  const el = A.el || (typeof require === "function" ? require("../ui.js").el : null)

  /** 公共界面构件。浏览器里 ui.js 先加载；Node 下 require 同一份文件——实现只有一份。 */
  const UI =
    (typeof window !== "undefined" &&
      (window.AlgoPlanUI || (window.StudyHub && window.StudyHub.ui) || (window.AlgoPlan && window.AlgoPlan.ui))) ||
    (typeof require === "function" ? require("../ui.js") : null)

  /** 轨道 → 中文名 */
  const TRACK_LABEL = { algorithm: "算法", database: "数据库", ai: "AI" }

  /** 单元状态 → 中文名（契约 §1.4 的四个取值） */
  const STATUS_LABEL = { todo: "待学", draft: "草稿", verified: "已验证", rejected: "已驳回" }

  /** 单元状态 → 药丸色调 */
  const STATUS_TONE = { todo: "", draft: "warn", verified: "ok", rejected: "bad" }

  /** verdict → 中文说法：颜色之外再来一份文字，不靠颜色单独表意 */
  const VERDICT_TEXT = {
    pass: "通过",
    wrong_answer: "答案错误",
    timeout: "超时",
    compile_error: "编译失败",
    runtime_error: "运行时错误",
  }

  /** verdict → 药丸色调 */
  const VERDICT_TONE = {
    pass: "ok",
    wrong_answer: "bad",
    timeout: "warn",
    compile_error: "bad",
    runtime_error: "bad",
  }

  /** demo 回显的定界行（契约 §3.5 冻结，不许改） */
  const DEMO_OUT_START = "─── demo 输出开始 ───"
  const DEMO_OUT_END = "─── demo 输出结束 ───"

  /** 自闭合标签：没有结束标签 */
  const VOID_TAGS = {
    br: true, hr: true, img: true, input: true, meta: true, link: true, col: true, source: true,
  }

  /** HTML 实体表（只收我们自己那套 markdown 转换器会吐出来的那些） */
  const ENTITIES = {
    amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", nbsp: " ", "#39": "'", "#x27": "'",
  }

  /** 页面内状态：当前单元与上一次结果跨渲染保留（切页回来不丢） */
  const pageState = {
    unit: null,
    statementHtml: "",
    code: "",
    language: "python",
    result: null,
    resultBox: null,
    llmText: "",
    doneText: "",
  }

  /**
   * 解 HTML 实体。
   * @param {string} text
   * @returns {string}
   */
  function decodeEntities(text) {
    return String(text === null || text === undefined ? "" : text).replace(
      /&(#?[a-zA-Z0-9]+);/g,
      function (all, name) {
        if (name.charAt(0) === "#") {
          const hex = name.charAt(1) === "x" || name.charAt(1) === "X"
          const code = hex ? parseInt(name.slice(2), 16) : parseInt(name.slice(1), 10)
          if (Number.isFinite(code) && code > 0 && code <= 0x10ffff) {
            try {
              return String.fromCodePoint(code)
            } catch (e) {
              return all
            }
          }
          return all
        }
        const key = name.toLowerCase()
        return Object.prototype.hasOwnProperty.call(ENTITIES, key) ? ENTITIES[key] : all
      }
    )
  }

  /**
   * 解析标签属性。
   * @param {string} raw
   * @returns {Object<string,string>}
   */
  function parseAttrs(raw) {
    const attrs = {}
    const re = /([a-zA-Z_:][-a-zA-Z0-9_:.]*)\s*=\s*("([^"]*)"|'([^']*)'|([^\s"'>]+))/g
    let m
    while ((m = re.exec(String(raw || ""))) !== null) {
      let value = ""
      if (m[3] !== undefined) value = m[3]
      else if (m[4] !== undefined) value = m[4]
      else if (m[5] !== undefined) value = m[5]
      attrs[m[1].toLowerCase()] = decodeEntities(value)
    }
    return attrs
  }

  /**
   * 把一小段 HTML 解析成节点描述树。
   * 只认我们自己那套 markdown 转换器会吐出来的标签；认不出的标签照样成节点，
   * **内容一个字都不丢**（契约 §8.1：未知语法原样输出）。
   * @param {string} html
   * @returns {Array<{tag: string, attrs: object, children: Array}>}
   */
  function parseHtmlNodes(html) {
    const text = String(html === null || html === undefined ? "" : html)
    const root = { tag: "#root", attrs: {}, children: [] }
    const stack = [root]
    const re = /<!--[\s\S]*?-->|<\/?([a-zA-Z][a-zA-Z0-9]*)((?:\s[^<>]*?)?)\/?>/g
    let last = 0
    let m

    function pushText(parent, raw) {
      const value = decodeEntities(raw)
      if (value === "") return
      parent.children.push({ tag: "#text", attrs: {}, children: [], text: value })
    }

    while ((m = re.exec(text)) !== null) {
      const chunk = text.slice(last, m.index)
      if (chunk) pushText(stack[stack.length - 1], chunk)
      last = re.lastIndex
      const raw = m[0]
      if (raw.slice(0, 4) === "<!--") continue
      const closing = raw.charAt(1) === "/"
      const tag = String(m[1] || "").toLowerCase()
      if (closing) {
        for (let i = stack.length - 1; i > 0; i -= 1) {
          if (stack[i].tag === tag) {
            stack.length = i
            break
          }
        }
        continue
      }
      const node = { tag: tag, attrs: parseAttrs(m[2] || ""), children: [] }
      stack[stack.length - 1].children.push(node)
      if (!VOID_TAGS[tag]) stack.push(node)
    }

    const tail = text.slice(last)
    if (tail) pushText(stack[stack.length - 1], tail)
    return root.children
  }

  /**
   * 把节点描述树拼成 DOM 节点数组。
   * @param {Array} nodes
   * @returns {Array}
   */
  function renderNodes(nodes) {
    const out = []
    ;(nodes || []).forEach(function (node) {
      if (!node) return
      if (node.tag === "#text") {
        out.push(document.createTextNode(node.text || ""))
        return
      }
      const attrs = {}
      Object.keys(node.attrs || {}).forEach(function (key) {
        attrs[key] = node.attrs[key]
      })
      out.push(el(node.tag, attrs, renderNodes(node.children)))
    })
    return out
  }

  /**
   * 从脚本输出里把 demo 的回显切出来（契约 §3.5 的两行定界）。
   * 切不出来就原样返回——老脚本没有回显也不该因此炸掉。
   * @param {string} stdout
   * @returns {{demoOutput: string, rest: string, hasMarkers: boolean}}
   */
  function splitDemoOutput(stdout) {
    const text = String(stdout === null || stdout === undefined ? "" : stdout)
    const i = text.indexOf(DEMO_OUT_START)
    const j = text.indexOf(DEMO_OUT_END)
    if (i === -1 || j === -1 || j < i) {
      return { demoOutput: "", rest: text, hasMarkers: false }
    }
    const demoOutput = text
      .slice(i + DEMO_OUT_START.length, j)
      .replace(/^\r?\n/, "")
      .replace(/\s+$/, "")
    const rest = (text.slice(0, i) + text.slice(j + DEMO_OUT_END.length)).trim()
    return { demoOutput: demoOutput, rest: rest, hasMarkers: true }
  }

  /**
   * 把单元整理成页面要用的视图数据（纯函数）。
   * @param {object} unit
   * @param {string} statementHtml
   * @param {{total?: number, sample?: object[]}} [cases] 接口返回的用例信息（契约 §8.1.1）
   * @returns {object}
   */
  function buildUnitView(unit, statementHtml, cases) {
    const u = unit || {}
    const entry = u.entry || {}
    const track = String(u.track || "")
    const kind = String(u.kind || "")
    const isProblem = kind === "problem"
    const status = String(u.status || "todo")
    const html = String(statementHtml === null || statementHtml === undefined ? "" : statementHtml)
    // 断言条数读 cases.total：接口只暴露前 2 条样例，unit.asserts 也是被裁过的，
    // 两者都不是「这道题一共几条断言」。卡类没有 cases 字段，回落到 unit.asserts.length。
    const totalCases = cases && Number.isFinite(Number(cases.total)) ? Number(cases.total) : null
    return {
      id: String(u.id || ""),
      title: String(u.title || "未命名单元"),
      track: track,
      trackLabel: TRACK_LABEL[track] || "未知轨道",
      kind: kind,
      kindLabel: isProblem ? "算法题" : "知识卡",
      isProblem: isProblem,
      graded: String(entry.graded || (isProblem ? "user" : "script")),
      status: status,
      statusLabel: STATUS_LABEL[status] || status,
      statusTone: STATUS_TONE[status] || "",
      difficulty: String(u.difficulty || ""),
      topics: Array.isArray(u.topics) ? u.topics.map(String) : [],
      subtracks: Array.isArray(u.subtracks) ? u.subtracks.map(String) : [],
      demoPath: String(entry.demo || ""),
      scriptPath: String(entry.script || ""),
      language: String(entry.language || (isProblem ? "cpp" : "python")),
      assertCount: totalCases === null ? (Array.isArray(u.asserts) ? u.asserts.length : 0) : totalCases,
      hasStatement: html.trim() !== "",
      statementNodes: parseHtmlNodes(html),
    }
  }

  /**
   * 画结果区：verdict 徽章 + 中文说明 + demo 输出 + 没过的断言。
   * @param {HTMLElement} box
   * @param {object} [result] 判题结果；不给就画 pageState.result
   */
  function paintResult(box, result) {
    if (!box) return
    box.textContent = ""
    const res = result === undefined ? pageState.result : result

    if (pageState.doneText) {
      box.appendChild(el("p", { class: "muted", text: pageState.doneText }))
    }
    if (pageState.llmText) {
      box.appendChild(el("div", { class: "llm-out", text: pageState.llmText }))
    }
    if (!res) {
      box.appendChild(UI.empty("还没跑过", "点上面的按钮跑一次，这里会显示 verdict 与 demo 的输出。"))
      return
    }

    const tone = VERDICT_TONE[res.verdict] || ""
    box.appendChild(
      el("div", { class: "row" }, [
        UI.pill(VERDICT_TEXT[res.verdict] || String(res.verdict), tone),
        el("span", {
          class: "muted",
          text: String(res.passed) + "/" + String(res.total) + " 条通过 · " + String(res.durationMs) + " 毫秒",
        }),
      ])
    )
    box.appendChild(el("p", { class: "verdict-message " + tone, text: String(res.message || "") }))

    const split = splitDemoOutput(res.stdout)
    if (split.hasMarkers) {
      box.appendChild(
        UI.block({ title: "demo 输出", children: [el("pre", { class: "unit-output", text: split.demoOutput })] })
      )
    }
    if (split.rest) {
      box.appendChild(
        UI.block({ title: "脚本输出", children: [el("pre", { class: "unit-output", text: split.rest })] })
      )
    }
    if (res.failed) {
      box.appendChild(
        UI.block({
          title: "没过的断言",
          children: [
            el("p", {
              class: "muted",
              text:
                "第 " +
                String(Number(res.failed.index) + 1) +
                " 条（" +
                String(res.failed.type) +
                "）：" +
                String(res.failed.reason),
            }),
          ],
        })
      )
    }
    if (res.stderr) {
      box.appendChild(
        UI.block({ title: "错误输出", children: [el("pre", { class: "unit-output", text: String(res.stderr) })] })
      )
    }
  }

  /**
   * 跑一次判题（卡片跑断言脚本，算法题跑用户代码）。
   */
  async function onJudge() {
    const unit = pageState.unit
    if (!unit) return
    const body = {}
    if (pageState.isProblem) {
      body.code = pageState.code
      body.language = pageState.language
    }
    try {
      pageState.result = await A.postJson("/api/units/" + encodeURIComponent(unit.id) + "/judge", body)
    } catch (err) {
      pageState.result = {
        verdict: "runtime_error",
        passed: 0,
        total: 0,
        durationMs: 0,
        message: String((err && err.message) || err),
        failed: null,
        stdout: "",
        stderr: "",
      }
    }
    paintResult(pageState.resultBox)
  }

  /**
   * 问 LLM。LLM 关掉时接口也是 200 + 中文降级文案，这里照样显示。
   */
  async function onAsk() {
    const unit = pageState.unit
    if (!unit) return
    try {
      const res = await A.postJson("/api/units/" + encodeURIComponent(unit.id) + "/ask", {})
      pageState.llmText = String((res && res.text) || "")
    } catch (err) {
      pageState.llmText = "问不了：" + String((err && err.message) || err)
    }
    paintResult(pageState.resultBox)
  }

  /**
   * 标记「完成本单元」。
   */
  async function onComplete() {
    const unit = pageState.unit
    if (!unit) return
    try {
      const res = await A.postJson("/api/units/" + encodeURIComponent(unit.id) + "/complete", {})
      pageState.doneText = "已标记完成（" + String((res && res.lastAt) || "") + "）"
    } catch (err) {
      pageState.doneText = "标记失败：" + String((err && err.message) || err)
    }
    paintResult(pageState.resultBox)
  }

  /**
   * 渲染单元页。
   * @param {object} view buildUnitView 的产物
   * @param {object} unit 原始单元
   * @param {string} [statementHtml] 题面 HTML（给了就重新解析）
   * @returns {HTMLElement}
   */
  function renderUnit(view, unit, statementHtml) {
    const v = view || {}
    const nodes = typeof statementHtml === "string" ? parseHtmlNodes(statementHtml) : (v.statementNodes || [])

    const head = UI.pageHead({
      eyebrow: "单元 · " + String(v.trackLabel || "未知轨道"),
      title: String(v.title || "未命名单元"),
      sub:
        String(v.kindLabel || "单元") +
        " · " +
        String(v.statusLabel || "待学") +
        " · " +
        String(v.assertCount || 0) +
        " 条断言",
    })

    // ── 左栏：读 ──
    const readKids = []
    if (v.hasStatement) {
      readKids.push.apply(readKids, renderNodes(nodes))
    } else {
      readKids.push(
        UI.empty("这个单元还没有正文", "去 learning 仓的单元文件里补一份讲解，页面会自己显示出来。")
      )
    }
    readKids.push(
      el("div", { class: "row" }, [
        el("button", { class: "btn", id: "unit-ask", text: "问 LLM", onclick: onAsk }),
      ])
    )
    const readCol = el("div", { class: "unit-col unit-col-read" }, [
      UI.panel({ title: "讲解", children: readKids }),
      UI.panel({
        title: "元信息",
        className: "quiet",
        children: [
          UI.kv([
            ["单元 ID", String(v.id || "")],
            ["轨道", String(v.trackLabel || "")],
            ["形态", String(v.kindLabel || "")],
            ["状态", String(v.statusLabel || "")],
            ["难度", String(v.difficulty || "未标")],
            ["知识点", (v.topics || []).join("、") || "无"],
            ["子轨", (v.subtracks || []).join("、") || "无"],
            ["demo 路径", String(v.demoPath || "无")],
            ["断言脚本", String(v.scriptPath || "无")],
          ]),
        ],
      }),
    ])

    // ── 右栏：做 ──
    const doKids = []
    doKids.push(
      UI.statGrid([
        { label: "断言条数", value: String(v.assertCount || 0) },
        { label: "知识点", value: String((v.topics || []).length) },
        { label: "状态", value: String(v.statusLabel || "待学"), tone: String(v.statusTone || "") },
      ])
    )

    if (v.isProblem) {
      const editor = el("textarea", { id: "unit-code", class: "code-editor" })
      editor.value = String(pageState.code || "")
      doKids.push(el("div", { class: "unit-code-wrap" }, [editor]))
      const picker = el("select", { id: "unit-language" }, [
        el("option", { value: "cpp", text: "C++" }),
        el("option", { value: "python", text: "Python" }),
      ])
      picker.value = String(pageState.language || "python")
      doKids.push(
        el("div", { class: "row" }, [
          picker,
          el("button", { class: "btn", id: "unit-judge", text: "提交判题", onclick: onJudge }),
        ])
      )
    } else {
      doKids.push(
        el("div", { class: "row" }, [
          el("button", { class: "btn", id: "unit-run", text: "运行断言", onclick: onJudge }),
          el("button", { class: "btn", id: "unit-rerun", text: "重新运行", onclick: onJudge }),
        ])
      )
    }
    doKids.push(
      el("div", { class: "row" }, [
        el("button", { class: "btn", id: "unit-complete", text: "完成本单元", onclick: onComplete }),
      ])
    )

    const resultBox = el("div", { class: "unit-result", id: "unit-result" })
    pageState.resultBox = resultBox
    paintResult(resultBox)
    doKids.push(resultBox)

    const heroSub = v.isProblem
      ? "写完代码点「提交判题」，右栏会显示 verdict 与没过的用例。"
      : "点「运行断言」跑一遍 demo，右栏会显示 verdict、demo 的输出与没过的断言。"

    const doCol = el("div", { class: "unit-col unit-col-do" }, [
      UI.hero({ title: "动手做", sub: heroSub, children: doKids }),
    ])

    return el("div", { class: "unit-page" }, [
      head,
      el("div", { class: "unit-layout" }, [readCol, doCol]),
    ])
  }

  /**
   * 页面入口：拉单元 → 建视图 → 画。
   * @param {HTMLElement} holder
   * @param {{id?: string}} params 来自 hash 路由的 params
   */
  async function renderPage(holder, params) {
    const p = params || {}
    const id = String(p.id || "")
    holder.textContent = ""
    holder.appendChild(el("div", { class: "panel", text: "正在打开单元…" }))

    if (!id) {
      holder.textContent = ""
      holder.appendChild(UI.pageHead({ eyebrow: "单元", title: "单元", sub: "地址里没有单元编号。" }))
      holder.appendChild(UI.empty("地址不完整", "从单元库里点一个单元进来。"))
      return
    }

    let payload = null
    try {
      payload = await A.getJson("/api/units/" + encodeURIComponent(id))
    } catch (err) {
      holder.textContent = ""
      holder.appendChild(
        UI.pageHead({ eyebrow: "单元", title: "单元没打开", sub: "下面是具体原因。" })
      )
      holder.appendChild(
        el("section", { class: "panel" }, [
          el("h2", { text: "加载失败" }),
          el("p", { class: "error", text: String((err && err.message) || err) }),
        ])
      )
      return
    }

    const unit = (payload && payload.unit) || {}
    const html = String((payload && payload.statement_html) || "")
    const cases = (payload && payload.cases) || null
    pageState.unit = unit
    pageState.statementHtml = html
    pageState.isProblem = unit.kind === "problem"
    pageState.view = buildUnitView(unit, html, cases)

    holder.textContent = ""
    holder.appendChild(renderUnit(pageState.view, unit, html))
  }

  if (A && A.pages) {
    A.pages.unit = { title: "单元", render: renderPage }
  }

  if (typeof module !== "undefined" && module.exports) {
    module.exports = {
      buildUnitView: buildUnitView,
      renderUnit: renderUnit,
      parseHtmlNodes: parseHtmlNodes,
      decodeEntities: decodeEntities,
      splitDemoOutput: splitDemoOutput,
      paintResult: paintResult,
      render: renderPage,
      TRACK_LABEL: TRACK_LABEL,
      STATUS_LABEL: STATUS_LABEL,
      VERDICT_TEXT: VERDICT_TEXT,
      DEMO_OUT_START: DEMO_OUT_START,
      DEMO_OUT_END: DEMO_OUT_END,
    }
  }
})()
