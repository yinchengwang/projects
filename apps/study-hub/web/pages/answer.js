"use strict";
/**
 * answer.js —— 作答页：看题面 → 写代码 → 提交判题 → 写错题订正
 *
 * 判题接口 POST /api/attempt 由 M3 提供；M3 还没上线时这里给出中文提示，不白屏。
 * 订正写回 POST /api/correction 由 M4 提供；写回只改 learning 仓的工作区，
 * **app 不会自动提交**，diff 摆在这里给用户看，提交与否由用户决定。
 *
 * M1/M3 的渲染逻辑（原题入口、题面缺失说明、样例用例、判题结果面板、题库表格）
 * 一起搬到这个文件里：app.js 只剩公共工具与 tab 调度。
 *
 * 层次（类名照 web/style.css 的约定，构件来自 web/ui.js）：
 *   .page-head     页头：眉标 + 大标题 + 副标，每页第一眼
 *   .panel.hero    主卡：这一页现在该做的那个动作（写代码、提交判题）
 *   .panel         普通卡：细节与上下文（题目信息、题面、样例用例）
 *   .block         卡内小节：题面 / 样例用例 / 判题结果 / 错题订正
 *   .stat-grid     数字块：用例数、耗时这类「几个数」不再挤成键值行
 *   .notice        题面缺失时的说明块（状态、去处、下一步）
 *
 * 判题契约（要显示给用户看）：
 *   提交的是完整的 stdin→stdout 程序，必须自己写 main()。
 *   系统不会替你生成入口，也不会注入 Solution 包装。
 */

(function () {
  const A = window.AlgoPlan
  const el = A.el

  /**
   * 公共界面构件。浏览器里 ui.js 先加载挂 window.AlgoPlanUI；
   * Node 下没有全局挂载，就 require 同一份文件——实现只有一份。
   */
  const UI =
    (typeof window !== "undefined" && (window.AlgoPlanUI || (window.AlgoPlan && window.AlgoPlan.ui))) ||
    (typeof require === "function" ? require("../ui.js") : null)

  /** verdict → 面板色调（与 judge/runner.js 的 VERDICT_PANEL.tone 对应） */
  const TONE_BY_VERDICT = {
    pass: "ok",
    wrong_answer: "bad",
    timeout: "warn",
    compile_error: "bad",
    runtime_error: "bad",
  }

  /** verdict → 中文说法：徽章上颜色之外再来一份文字，不靠颜色单独表意 */
  const VERDICT_TEXT = {
    pass: "通过",
    wrong_answer: "答案错误",
    timeout: "超时",
    compile_error: "编译失败",
    runtime_error: "运行时错误",
  }

  /** 面板里期望/实际最多显示多少字符 */
  const PANEL_TEXT_LIMIT = 400

  /** 页面内状态：订正正文与写回结果跨渲染保留（代码草稿按题号存在 A.state.draft 里） */
  const pageState = {
    problem: null,
    verdict: null,
    correctionText: "",
    diff: "",
  }

  /** 拼子节点：空的剔掉 */
  function kids(list, more) {
    const out = []
    ;(list || []).concat(more || []).forEach(function (child) {
      if (child) out.push(child)
    })
    return out
  }

  /**
   * 构件的等价实现：UI 在位就用 UI，不在位就用等价的原生写法。
   * 两条路渲染出的结构一致，页面不会因为少一个脚本就塌掉。
   */
  const UIX = {
    pageHead: function (opts) {
      const o = opts || {}
      if (UI) return UI.pageHead(o)
      return el("section", { class: "page-head" }, [
        o.eyebrow ? el("span", { class: "eyebrow", text: o.eyebrow }) : null,
        el("h2", { class: "page-title", text: o.title }),
        o.sub ? el("p", { class: "page-sub", text: o.sub }) : null,
      ])
    },
    hero: function (opts) {
      const o = opts || {}
      if (UI) return UI.hero(o)
      return el(
        "section",
        { class: "panel hero" },
        kids(
          [
            o.title ? el("h2", { text: o.title }) : null,
            o.sub ? el("p", { class: "reason", text: o.sub }) : null,
          ],
          o.children
        )
      )
    },
    panel: function (opts) {
      const o = opts || {}
      if (UI) return UI.panel(o)
      return el(
        "section",
        { class: "panel" + (o.className ? " " + o.className : "") },
        kids([o.title ? el("h2", { text: o.title }) : null], o.children)
      )
    },
    block: function (opts) {
      const o = opts || {}
      if (UI) return UI.block(o)
      return el(
        "div",
        { class: "block" },
        kids([o.title ? el("p", { class: "block-title", text: o.title }) : null], o.children)
      )
    },
    statGrid: function (items) {
      if (UI) return UI.statGrid(items)
      return el(
        "div",
        { class: "kv" },
        (items || []).map(function (item) {
          return el("div", {}, [
            document.createTextNode(item.label + "："),
            el("span", { text: String(item.value) }),
          ])
        })
      )
    },
    pill: function (text, tone) {
      if (UI) return UI.pill(text, tone)
      return el("span", { class: "badge " + (tone || ""), text: String(text) })
    },
    empty: function (title, hint) {
      if (UI) return UI.empty(title, hint)
      return el("section", { class: "panel" }, [
        el("h2", { text: String(title) }),
        hint ? el("p", { class: "muted", text: String(hint) }) : null,
      ])
    },
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
    return el("a", {
      class: "link",
      href: link.url,
      target: "_blank",
      rel: "noopener noreferrer",
      text: link.label,
    })
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

  /** 渲染题库列表（每行带一个「作答」按钮 + 原题入口） */
  function renderBank(bank) {
    const items = (bank && bank.items) || []
    const rows = items.map(function (item) {
      const topics = (item.topics || []).map(function (t) {
        return el("span", { class: "badge", text: t })
      })
      const go = el("button", { text: "作答" })
      go.addEventListener("click", function () {
        A.state.problemId = item.id
        A.go("answer")
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
            text: "题库还是空的。设置 STUDY_HUB_LEARNING_ROOT 后运行 node tools/seed-bank.js 导入样板题。",
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

  /**
   * 题面卡：这一页的上下文。
   *
   * 分三层：卡内「题目信息」摆事实，「题面」摆正文，「样例用例」摆对答案用的样例。
   * 键值行只放三四个身份事实，数字（用例数、耗时）另走数字块。
   */
  function statementPanel(problem) {
    const cases = problem.cases || {}
    const samples = cases.sample || []
    const total = cases.total || 0

    const info = el("div", { class: "kv" }, [
      el("div", {}, [document.createTextNode("ID："), el("span", { text: problem.id })]),
      el("div", {}, [document.createTextNode("难度："), el("span", { text: problem.difficulty || "" })]),
      el("div", {}, [
        document.createTextNode("知识点："),
        el("span", { text: (problem.topics || []).join("、") }),
      ]),
      el("div", {}, [
        document.createTextNode("用例数："),
        el("span", { text: String(total) }),
      ]),
    ])

    // 数字摆成数字块：一眼看完「一共几个用例、界面给了几个样例」
    const stats = UIX.statGrid([
      { label: "总用例数", value: String(total), hint: "全部用例由服务端持有" },
      { label: "样例数", value: String(samples.length), hint: "题面下方已展示" },
    ])

    // 标题旁边常驻一个原题入口：有题面时也能跳过去核对原文
    const link = describeSourceLink(problem)
    const linkRow =
      link.kind === "none"
        ? null
        : el("p", { class: "muted" }, [
            document.createTextNode("原题："),
            el("a", {
              class: "link",
              href: link.url,
              target: "_blank",
              rel: "noopener noreferrer",
              text: link.label,
            }),
          ])

    const statement = String(problem.statement || "").trim()

    // 样例用例：接口只放行前 2 个，展示出来是给用户对答案用的
    const sampleChildren = [
      el("p", {
        class: "muted",
        text:
          "共 " +
          total +
          " 个用例，界面只展示前 " +
          samples.length +
          " 个样例（全部用例由服务端持有）。",
      }),
    ]
    samples.forEach(function (c, i) {
      sampleChildren.push(
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

    return UIX.panel({
      // 题目名已经在页头的大标题里了，这里只说清这张卡是干什么的，不重复一遍
      title: "题目",
      children: [
        UIX.block({
          title: "题目信息",
          children: [info, stats, linkRow],
        }),
        UIX.block({
          title: "题面",
          children: statement
            ? [el("pre", { class: "statement", text: statement })]
            : [renderMissingStatement(problem)],
        }),
        UIX.block({
          title: "样例用例",
          children: sampleChildren,
        }),
      ],
    })
  }

  /**
   * 代码卡：这一页现在该做的那个动作。
   *
   * 编辑器占主位，语言与判题契约是它的附属说明，结果面板贴着提交按钮出现。
   */
  function codePanel(problem) {
    const id = problem.id
    // 草稿按题号存：切页签、切题再切回来，用户写的代码还在
    const drafts = A.state.draft || (A.state.draft = {})

    const textarea = el("textarea", { id: "judge-code", class: "code", rows: "18", spellcheck: "false" })
    textarea.value = drafts[id] || ""
    textarea.addEventListener("input", function () {
      drafts[id] = textarea.value
    })

    const select = el("select", { id: "judge-language" }, [
      el("option", { value: "cpp", text: "C++" }),
      el("option", { value: "python", text: "Python" }),
    ])
    select.value = A.state.language || "cpp"
    select.addEventListener("change", function () {
      A.state.language = select.value
    })

    const result = el("div", { class: "result", id: "judge-result" })

    // 上一次的结果面板：切页签回来还在
    const lastResult = (A.state.result || {})[id]
    if (lastResult) result.appendChild(verdictBox(lastResult, problem))

    const submit = el("button", { id: "judge-submit", class: "primary", text: "提交判题" })
    submit.addEventListener("click", async function () {
      submit.disabled = true
      submit.textContent = "判题中…"
      result.textContent = ""
      drafts[id] = textarea.value
      try {
        const body = await A.postJson("/api/attempt", {
          problemId: id,
          code: textarea.value,
          language: select.value,
          mode: "solve",
        })
        pageState.verdict = body
        A.state.result[id] = body
        result.appendChild(verdictBox(body, problem))
      } catch (err) {
        result.textContent = ""
        result.appendChild(el("p", { class: "error", text: String(err.message || err) }))
        if (String(err.message || "").indexOf("404") !== -1) {
          result.appendChild(el("p", { class: "muted", text: "判题接口还没上线（M3 未完成）。" }))
        }
      } finally {
        submit.disabled = false
        submit.textContent = "提交判题"
      }
    })

    return UIX.hero({
      title: "我的代码",
      children: [
        el("div", { class: "kv" }, [el("div", {}, [document.createTextNode("语言："), select])]),
        el("p", {
          class: "muted",
          text:
            "判题契约：提交的是完整的 stdin→stdout 程序，必须自己写 main()；" +
            "系统不会替你生成入口，也不会注入 Solution 包装。",
        }),
        textarea,
        el("div", { class: "row" }, [submit]),
        result,
      ],
    })
  }

  /**
   * 判题结果面板（色调、截断、失败用例、编译原文）。
   *
   * 放在代码卡里当嵌套块：结果紧挨着提交按钮出现，
   * 用例数与耗时走数字块，其余按小节分层。
   */
  function verdictBox(body, problem) {
    const p = describePanel(body)
    const children = []

    if (p.verdict) {
      children.push(
        el("div", { class: "row" }, [
          UIX.pill(VERDICT_TEXT[p.verdict] || p.verdict, p.tone),
          el("span", { class: "badge " + (p.tone || ""), text: p.verdict }),
        ])
      )
    }
    if (p.title) {
      children.push(
        el("p", { class: "verdict-message " + (p.tone || ""), text: p.title })
      )
    }
    children.push(
      UIX.statGrid(
        p.rows.map(function (row) {
          return { label: row.label, value: row.value }
        })
      )
    )
    if (p.failed) {
      children.push(
        UIX.block({
          title: "失败用例",
          children: [
            el("div", { class: "kv" }, [
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
            ]),
          ],
        })
      )
    }
    if (p.compilerOutput) {
      children.push(
        UIX.block({
          title: "编译 / 错误原文",
          children: [el("pre", { class: "raw", text: p.compilerOutput })],
        })
      )
    }
    children.push(
      el("p", {
        class: "muted",
        text: "提示：判题不是安全沙箱，只防跑飞与死循环，不防恶意代码。",
      })
    )
    // 没通过就顺手给一块订正区：错题不写下来，下次还会错
    if (p.verdict && p.verdict !== "pass" && problem) {
      children.push(correctionPanel(problem))
    }
    return UIX.block({ title: "判题结果", children: children })
  }

  /** 错题订正：写进 learning 仓的 notes/practice/<id>-订正.md，只给 diff，不自动提交 */
  function correctionPanel(problem) {
    const textarea = el("textarea", { class: "code", rows: "5", spellcheck: "false" })
    textarea.value = pageState.correctionText
    textarea.addEventListener("input", function () {
      pageState.correctionText = textarea.value
    })
    const out = el("div", { class: "result" })

    const button = el("button", {
      text: "写错题订正",
      onclick: async function () {
        out.textContent = "写入中…"
        try {
          const body = await A.postJson("/api/correction", {
            problemId: problem.id,
            text: textarea.value,
          })
          pageState.diff = body.diff || ""
          out.textContent = ""
          out.appendChild(el("p", { class: "ok-text", text: "已写入：" + body.file }))
          out.appendChild(
            el("p", {
              class: "muted",
              text: "写回只改 learning 仓的工作区，app 不会自动提交；下面是 diff，提交与否由你决定。",
            })
          )
          out.appendChild(el("pre", { class: "diff", text: body.diff || "（没有变化）" }))
        } catch (err) {
          out.textContent = ""
          out.appendChild(el("p", { class: "error", text: String(err.message || err) }))
        }
      },
    })

    return UIX.block({
      title: "错题订正",
      children: [
        el("p", {
          class: "muted",
          text: "写下的复盘会存成 learning 仓里的 notes/practice/" + problem.id + "-订正.md。",
        }),
        textarea,
        el("div", { class: "row" }, [button]),
        out,
      ],
    })
  }

  /** 没选题目时：给个题库下拉 */
  function pickPanel() {
    const out = el("section", { class: "panel" })
    const body = el("div", { class: "result" })
    out.appendChild(el("h2", { text: "先选一道题" }))
    out.appendChild(body)
    A.getJson("/api/bank")
      .then(function (bank) {
        const items = bank.items || []
        const select = el("select", { id: "problem-select" }, items.map(function (item) {
          return el("option", { value: item.id, text: item.id + " " + item.title })
        }))
        body.appendChild(el("div", { class: "row" }, [
          select,
          el("button", {
            text: "打开",
            onclick: function () {
              A.state.problemId = select.value
              A.go("answer")
            },
          }),
        ]))
        if (items.length === 0) {
          body.appendChild(el("p", { class: "muted", text: "题库还是空的。" }))
        }
      })
      .catch(function (err) {
        body.appendChild(el("p", { class: "error", text: String(err.message || err) }))
      })
    return out
  }

  /** 打开不了这道题时的错误卡：错误文本必须留着，不能只说一句「加载失败」 */
  function errorPanel(err) {
    return el("section", { class: "panel" }, [
      el("h2", { text: "打不开这道题" }),
      el("p", { class: "error", text: String(err.message || err) }),
    ])
  }

  /**
   * 画作答页正文（页头 + 题面卡 + 代码卡）。
   * @param {object} holder 容器
   * @param {string} problemId 题目 id
   */
  async function renderPractice(holder, problemId) {
    const id = problemId || A.state.problemId
    if (!id) {
      holder.appendChild(
        UIX.panel({
          title: "作答",
          children: [el("p", { class: "muted", text: "先在「题库」里点一道题的「作答」按钮。" })],
        })
      )
      return holder
    }
    const problem = await A.getJson("/api/problem/" + encodeURIComponent(id))
    pageState.problem = problem
    // 页头在最上面：先说清这是哪道题，再说下面怎么作答
    holder.appendChild(
      UIX.pageHead({
        eyebrow: "作答",
        title: problem.title || "作答",
        sub: "读题 → 写代码 → 提交判题；没通过就把错因写进订正，diff 会摆在这里给你看。",
      })
    )
    holder.appendChild(statementPanel(problem))
    holder.appendChild(codePanel(problem))
    return holder
  }

  A.pages.answer = {
    title: "作答",
    render: async function (view) {
      view.textContent = ""
      const id = A.state.problemId
      if (!id) {
        view.appendChild(
          UIX.pageHead({
            eyebrow: "作答",
            title: "作答",
            sub: "先在题库里挑一道题，然后在这里写代码、提交判题。",
          })
        )
        view.appendChild(pickPanel())
        return
      }
      const holder = el("div", {})
      holder.appendChild(el("div", { class: "panel", text: "正在打开题目…" }))
      view.appendChild(holder)
      try {
        holder.textContent = ""
        await renderPractice(holder, id)
      } catch (err) {
        view.textContent = ""
        view.appendChild(
          UIX.pageHead({
            eyebrow: "作答",
            title: "作答",
            sub: "这道题没能加载出来，下面是具体原因。",
          })
        )
        view.appendChild(errorPanel(err))
      }
    },
  }

  // Node（单测）下导出渲染函数，老测试直接用同一份实现
  if (typeof module !== "undefined" && module.exports) {
    module.exports = {
      render: A.pages.answer.render,
      renderPractice: renderPractice,
      renderBank: renderBank,
      renderVerdictPanel: function (body) {
        return verdictBox(body, null)
      },
      renderMissingStatement: renderMissingStatement,
      renderSourceLink: renderSourceLink,
      describeSourceLink: describeSourceLink,
      describePanel: describePanel,
      truncate: truncate,
      TONE_BY_VERDICT: TONE_BY_VERDICT,
      PANEL_TEXT_LIMIT: PANEL_TEXT_LIMIT,
    }
  }
})()
