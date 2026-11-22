"use strict";
/**
 * supply.js —— 供给页（P4）
 *
 * 薄页，只摆三件事：
 *   1. 现在供给是什么状态（每轨的 todo / draft / verified / rejected）
 *   2. 还欠什么（缺口清单）
 *   3. 抽检入口：对缺口清单里的单元当场点「通过 / 打回」，写回 frontmatter
 *
 * 层次（G7）：一个页头 + 一张主卡（.panel hero）+ 数字块（.stat-grid）
 * + 普通卡（.panel）+ 背景卡（.panel quiet）。状态一律用带中文文字的药丸承载。
 * 不引路由框架（G29）：沿用「整页重绘 + renderSeq 竞态守卫」的机制。
 */

(function () {
  const A = window.StudyHub
  const el = A.el

  /**
   * 公共界面构件。浏览器里 ui.js 先加载挂 window.StudyHubUI；
   * Node 下没有全局挂载，就 require 同一份文件——实现只有一份。
   */
  const UI =
    (typeof window !== "undefined" && (window.StudyHubUI || (window.StudyHub && window.StudyHub.ui))) ||
    (typeof require === "function" ? require("../ui.js") : null)

  /** 缺口种类的中文名 */
  const GAP_LABELS = {
    asserts: "缺断言脚本",
    statement: "缺讲解正文",
    cases: "缺测试用例",
    solution: "缺参考解法",
  }

  /** 轨道的中文名（前端不引后端模块，这里写死一份） */
  const TRACK_LABELS = {
    algorithm: "算法",
    database: "数据库",
    ai: "AI",
  }

  /** 单元状态的中文名 */
  const STATUS_LABELS = {
    todo: "待补内容",
    draft: "待校验",
    verified: "已验证",
    rejected: "已打回",
  }

  /** 药丸：不传色调，className 才精确等于 "pill"（层次闸门按精确匹配找） */
  function statusPill(text) {
    return UI ? UI.pill(text) : el("span", { class: "pill", text: text })
  }

  /** 数字块 */
  function statGrid(items) {
    if (UI) return UI.statGrid(items)
    return el("div", { class: "stat-grid" }, items.map(function (item) {
      return el("div", { class: "stat" }, [
        el("span", { class: "stat-value", text: String(item.value) }),
        el("span", { class: "stat-label", text: String(item.label) }),
      ])
    }))
  }

  /** 主卡：现在该做什么 */
  function heroPanel(gaps, onReview, reviewable) {
    const children = [el("h2", { text: "下一步" })]
    if (gaps.length === 0) {
      children.push(el("p", { class: "reason", text: "供给上没有缺口：三轨的内容都齐了。" }))
      return UI ? UI.hero({ title: "下一步", children: children.slice(1) }) : el("section", { class: "panel hero" }, children)
    }
    const counts = {}
    gaps.forEach(function (gap) {
      ;(gap.missing || []).forEach(function (kind) {
        counts[kind] = (counts[kind] || 0) + 1
      })
    })
    children.push(el("p", {
      class: "reason",
      text: "还差 " + gaps.length + " 处：缺断言脚本 " + (counts.asserts || 0)
        + " 个、缺讲解正文 " + (counts.statement || 0)
        + " 个、缺测试用例 " + (counts.cases || 0)
        + " 个、缺参考解法 " + (counts.solution || 0) + " 个。",
    }))
    children.push(el("p", {
      class: "muted",
      text: "补缺口在命令行里跑：node tools/supply.js generate --track database",
    }))
    // 快捷抽检只指向「待抽检清单」里的第一个——缺口没有抽检资格。
    if (Array.isArray(reviewable) && reviewable.length > 0) {
      children.push(el("div", { class: "row" }, [
        el("button", { class: "primary", text: "抽检第一个", onclick: function () { onReview(reviewable[0], true) } }),
        el("button", { text: "打回第一个", onclick: function () { onReview(reviewable[0], false) } }),
      ]))
    }
    return UI ? UI.hero({ title: "下一步", children: children.slice(1) }) : el("section", { class: "panel hero" }, children)
  }

  /** 各轨进度 */
  function trackPanel(tracks) {
    const totals = { todo: 0, draft: 0, verified: 0, rejected: 0 }
    tracks.forEach(function (item) {
      totals.todo += Number(item.todo || 0)
      totals.draft += Number(item.draft || 0)
      totals.verified += Number(item.verified || 0)
      totals.rejected += Number(item.rejected || 0)
    })
    const items = [
      { label: "待补内容", value: totals.todo + " 个" },
      { label: "待校验", value: totals.draft + " 个" },
      { label: "已验证", value: totals.verified + " 个" },
      { label: "已打回", value: totals.rejected + " 个" },
    ]
    const children = [statGrid(items)]
    tracks.forEach(function (item) {
      const code = String(item.track || "")
      children.push(el("div", { class: "kv" }, [
        el("div", {}, [
          document.createTextNode((TRACK_LABELS[code] || code) + "："),
          el("span", { text: "共 " + item.total + " 个" }),
          statusPill("待校验 " + item.draft + " 个"),
          statusPill("已验证 " + item.verified + " 个"),
          statusPill("已打回 " + item.rejected + " 个"),
        ]),
      ]))
    })
    return UI ? UI.panel({ title: "各轨进度", children: children }) : el("section", { class: "panel" }, children)
  }

  /**
   * 缺口清单。
   *
   * **只读**：缺口是「还没补齐的东西」，不是一个可以被人点一下就有结论的对象。
   * 补缺口要跑命令行（generate 生成、verify 校验），这里只负责告诉人缺什么。
   */
  function gapPanel(gaps) {
    const children = []
    if (gaps.length === 0) {
      children.push(el("p", { class: "muted", text: "没有缺口。" }))
      return UI ? UI.panel({ title: "缺口清单", children: children }) : el("section", { class: "panel" }, children)
    }
    gaps.forEach(function (gap) {
      const kinds = (gap.missing || []).map(function (kind) {
        return GAP_LABELS[kind] || kind
      })
      children.push(el("div", { class: "kv" }, [
        el("div", {}, [
          document.createTextNode(String(gap.id || "") + "　"),
          el("span", { text: String(gap.title || "") }),
        ]),
        el("div", { class: "row" }, [
          statusPill("缺：" + kinds.join("、")),
          el("span", {
            class: "faint",
            text: "补缺口跑命令行：node tools/supply.js generate --track " + String(gap.track || ""),
          }),
        ]),
      ]))
    })
    return UI ? UI.panel({ title: "缺口清单", children: children }) : el("section", { class: "panel" }, children)
  }

  /**
   * 待抽检清单。
   *
   * 抽检是「人看过之后给个结论」——只对**已达标**（verified）的单元开放，
   * 所以这里才是唯一该出现「通过 / 打回」两个按钮的地方。
   */
  function reviewPanel(reviewable, onReview) {
    const children = []
    if (!reviewable || reviewable.length === 0) {
      children.push(el("p", {
        class: "muted",
        text: "没有待抽检的单元（都被人看过了，或者还没通过校验）。",
      }))
      return UI ? UI.panel({ title: "待抽检", children: children }) : el("section", { class: "panel" }, children)
    }
    reviewable.forEach(function (item) {
      children.push(el("div", { class: "kv" }, [
        el("div", {}, [
          document.createTextNode(String(item.id || "") + "　"),
          el("span", { text: String(item.title || "") }),
        ]),
        el("div", { class: "row" }, [
          statusPill("已验证"),
          el("button", { class: "primary", text: "通过", onclick: function () { onReview(item, true) } }),
          el("button", { text: "打回", onclick: function () { onReview(item, false) } }),
        ]),
      ]))
    })
    return UI ? UI.panel({ title: "待抽检", children: children }) : el("section", { class: "panel" }, children)
  }

  /** 背景信息：运行状态与提醒 */
  function quietPanel(warnings) {
    const children = []
    if (warnings.length === 0) {
      children.push(el("p", { class: "muted", text: "没有提醒。" }))
    } else {
      warnings.forEach(function (text) {
        children.push(el("p", { class: "muted", text: text }))
      })
    }
    return UI
      ? UI.panel({ title: "运行状态与提醒", children: children, className: "quiet" })
      : el("section", { class: "panel quiet" }, children)
  }

  /**
   * 把接口返回的数据变成一棵界面树。纯函数，方便测。
   * @param {object} status
   * @returns {HTMLElement[]}
   */
  function buildSupplyView(status) {
    const data = status || {}
    const tracks = Array.isArray(data.tracks) ? data.tracks : []
    const gaps = Array.isArray(data.gaps) ? data.gaps : []
    const reviewable = Array.isArray(data.reviewable) ? data.reviewable : []
    const warnings = Array.isArray(data.warnings) ? data.warnings : []
    const nodes = []
    const notice = el("p", { class: "muted", text: "" })

    function onReview(item, passed) {
      const id = String((item && item.id) || "")
      if (id === "") return
      notice.textContent = "正在写回 " + id + "……"
      A.postJson("/api/supply/review/" + encodeURIComponent(id), { passed: passed, note: "" })
        .then(function () {
          notice.textContent = "已写回 " + id + (passed ? "：通过" : "：打回")
        })
        .catch(function (err) {
          notice.textContent = "写回失败：" + String((err && err.message) || err)
        })
    }

    if (UI) {
      nodes.push(UI.pageHead({
        eyebrow: "供给",
        title: "供给状态",
        sub: "缺口是「还没补齐的东西」，只能跑命令行补；抽检是「人看过之后给个结论」，只对已通过的单元开放。",
      }))
    }
    nodes.push(heroPanel(gaps, onReview, reviewable))
    nodes.push(trackPanel(tracks))
    nodes.push(gapPanel(gaps))
    nodes.push(reviewPanel(reviewable, onReview))
    nodes.push(notice)
    nodes.push(quietPanel(warnings))
    return nodes
  }

  A.pages.supply = {
    title: "供给",
    render: async function (view) {
      view.textContent = ""
      view.appendChild(el("div", { class: "panel", text: "正在看供给状态…" }))
      let status = null
      try {
        status = await A.getJson("/api/supply/status")
      } catch (err) {
        view.textContent = ""
        view.appendChild(el("section", { class: "panel" }, [
          el("h2", { text: "加载失败" }),
          el("p", { class: "error", text: String((err && err.message) || err) }),
        ]))
        return
      }
      view.textContent = ""
      buildSupplyView(status).forEach(function (node) {
        view.appendChild(node)
      })
    },
  }

  // Node（单测）下导出纯函数，老测试直接用同一份实现
  if (typeof module !== "undefined" && module.exports) {
    module.exports = {
      buildSupplyView: buildSupplyView,
      statusPill: statusPill,
      GAP_LABELS: GAP_LABELS,
      TRACK_LABELS: TRACK_LABELS,
      STATUS_LABELS: STATUS_LABELS,
    }
  }
})()
