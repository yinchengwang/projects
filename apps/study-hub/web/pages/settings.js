"use strict"
/**
 * settings.js —— 会话设置页（#/settings）
 *
 * 四块内容：
 *   1. 会话长度（主卡）——每次学几个单元，1~10
 *   2. 跨轨权重——每轨一个权重 + 一个开关，关掉的轨不参与排课
 *   3. 复习次数阈值 K——学 K 个单元就该回顾一次到期卡
 *   4. 环境自检（静默卡）——Python / NumPy 在不在
 *
 * 密钥纪律：这一页**只显示布尔事实**（有没有配 LLM 密钥），
 * 绝不显示密钥本身——密钥也不在 GET /api/settings 的响应里。
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

  /** 轨代码 → 中文标签 */
  const TRACK_LABEL = { algorithm: "算法", database: "数据库", ai: "AI" }

  /**
   * 归一化设置里的轨清单——缺字段也不炸。
   * @param {Array<object>} tracks
   * @returns {Array<{code: string, label: string, weight: number, enabled: boolean, unit_count: number}>}
   */
  function normalizeTracks(tracks) {
    return (tracks || []).map(function (track) {
      const t = track || {}
      const code = String(t.code || "")
      return {
        code: code,
        label: String(t.label || TRACK_LABEL[code] || code),
        weight: Number(t.weight || 0),
        enabled: t.enabled === false ? false : true,
        unit_count: Number(t.unit_count || 0),
      }
    })
  }

  /**
   * 三轨权重加起来是多少（四舍五入到 4 位，避开浮点尾巴）。
   * @param {Array<object>} tracks
   * @returns {number}
   */
  function weightSum(tracks) {
    let sum = 0
    ;(tracks || []).forEach(function (track) {
      sum += Number((track && track.weight) || 0)
    })
    return Math.round(sum * 10000) / 10000
  }

  /**
   * 表单状态 → 请求体（契约 §8.2.2 的形状）。
   * @param {{session_count: number, count_trigger_k: number, weights: {[code: string]: number}, enabled: {[code: string]: boolean}}} fields
   * @returns {{session_count: number, count_trigger_k: number, track_weights: {[code: string]: number}, track_enabled: {[code: string]: boolean}}}
   */
  function buildPayload(fields) {
    const f = fields || {}
    const weights = {}
    const enabled = {}
    Object.keys(f.weights || {}).forEach(function (code) {
      weights[code] = Number(f.weights[code])
    })
    Object.keys(f.enabled || {}).forEach(function (code) {
      enabled[code] = f.enabled[code] === true
    })
    return {
      session_count: Number(f.session_count),
      count_trigger_k: Number(f.count_trigger_k),
      track_weights: weights,
      track_enabled: enabled,
    }
  }

  /**
   * 校验请求体。返回空数组表示没问题。
   * 报错文案与服务端一致——前端先拦一道，不让非法值打回去。
   * @param {object} payload
   * @returns {string[]}
   */
  function validatePayload(payload) {
    const errors = []
    const p = payload || {}
    const count = Number(p.session_count)
    if (!Number.isInteger(count) || count < 1 || count > 10) {
      errors.push("会话长度必须是 1~10 的整数")
    }
    const k = Number(p.count_trigger_k)
    if (!Number.isInteger(k) || k < 1) {
      errors.push("次数阈值 K 必须是大于等于 1 的整数")
    }
    const weights = p.track_weights || {}
    Object.keys(weights).forEach(function (code) {
      const w = Number(weights[code])
      if (!Number.isFinite(w) || w < 0) {
        errors.push("轨权重必须是不小于 0 的数字")
      }
    })
    const enabled = p.track_enabled || {}
    Object.keys(enabled).forEach(function (code) {
      if (typeof enabled[code] !== "boolean") {
        errors.push("轨开关必须是布尔值")
      }
    })
    return errors
  }

  /**
   * 环境自检 → 两行（Python / NumPy）。
   * @param {object} env
   * @returns {Array<{key: string, label: string, ok: boolean, detail: string}>}
   */
  function envRows(env) {
    const e = env || {}
    const defs = [
      { key: "python", label: "Python 解释器" },
      { key: "numpy", label: "NumPy" },
    ]
    return defs.map(function (def) {
      const item = e[def.key] || {}
      return {
        key: def.key,
        label: def.label,
        ok: item.ok === true,
        detail: String(item.detail || "没检测到"),
      }
    })
  }

  /** 出错时的提示（不崩页面） */
  function errorPanel(title, err) {
    return el("section", { class: "panel" }, [
      el("h2", { text: title }),
      el("p", { class: "error", text: String((err && err.message) || err) }),
    ])
  }

  /**
   * 保存设置。校验不过就不发请求，直接把中文错摆出来。
   * @param {HTMLElement} view
   * @param {HTMLElement} statusNode 提示行
   * @param {object} fields 表单状态
   * @returns {Promise<void>}
   */
  async function save(view, statusNode, fields) {
    const payload = buildPayload(fields)
    const errors = validatePayload(payload)
    if (errors.length > 0) {
      statusNode.className = "error"
      statusNode.textContent = errors.join("；")
      return
    }
    try {
      await A.putJson("/api/settings", payload)
    } catch (err) {
      statusNode.className = "error"
      statusNode.textContent = String((err && err.message) || err)
      return
    }
    statusNode.className = "ok-text"
    statusNode.textContent = "已保存，下一次会话按新设置排。"
  }

  /**
   * 渲染设置页。
   * @param {HTMLElement} view
   * @returns {Promise<void>}
   */
  async function render(view) {
    view.textContent = ""
    view.appendChild(el("div", { class: "panel", text: "正在加载设置…" }))
    let settings = null
    try {
      settings = await A.getJson("/api/settings")
    } catch (err) {
      view.textContent = ""
      view.appendChild(errorPanel("加载失败", err))
      return
    }
    const cfg = settings || {}
    const tracks = normalizeTracks(cfg.tracks)
    const sessionCfg = cfg.session || {}
    const reviewCfg = cfg.review || {}

    const fields = {
      session_count: Number(sessionCfg.default_count || 3),
      count_trigger_k: Number(reviewCfg.count_trigger_k || 5),
      weights: {},
      enabled: {},
    }
    tracks.forEach(function (track) {
      fields.weights[track.code] = track.weight
      fields.enabled[track.code] = track.enabled
    })

    const statusNode = el("p", { class: "muted", text: "" })

    view.textContent = ""
    view.appendChild(
      UI.pageHead({
        eyebrow: "学习中枢",
        title: "设置",
        sub: "会话长度、跨轨权重、复习次数阈值——改完保存，下一次会话按新设置排。",
      })
    )

    /* ---------- 会话长度：主卡 ---------- */

    const countInput = el("input", {
      type: "number",
      min: String(sessionCfg.min_count || 1),
      max: String(sessionCfg.max_count || 10),
      "data-field": "session_count",
      class: "setting-input",
    })
    countInput.value = String(fields.session_count)
    countInput.addEventListener("change", function () {
      fields.session_count = Number(countInput.value)
    })

    view.appendChild(
      UI.hero({
        title: "每次学几个",
        sub: "会话长度按单元数计，不按天数。",
        children: [
          UI.statGrid([
            { label: "当前", value: fields.session_count + " 个单元", tone: "accent" },
            {
              label: "可选范围",
              value: (sessionCfg.min_count || 1) + "~" + (sessionCfg.max_count || 10) + " 个",
            },
            { label: "生效时机", value: "下一次会话" },
          ]),
          el("div", { class: "setting-row" }, [
            el("span", { class: "setting-name", text: "每次学几个" }),
            countInput,
            el("span", { class: "setting-hint", text: "1~10 个单元，排一条队列。" }),
          ]),
          el("div", { class: "row" }, [
            el("button", {
              class: "primary",
              text: "保存设置",
              onclick: function () {
                save(view, statusNode, fields)
              },
            }),
          ]),
          statusNode,
        ],
      })
    )

    /* ---------- 跨轨权重：普通卡 ---------- */

    const trackChildren = tracks.map(function (track) {
      const weightInput = el("input", {
        type: "number",
        step: "0.05",
        min: "0",
        max: "1",
        "data-field": "weight:" + track.code,
        class: "setting-input",
      })
      weightInput.value = String(track.weight)
      weightInput.addEventListener("change", function () {
        fields.weights[track.code] = Number(weightInput.value)
      })

      const toggle = el("input", {
        type: "checkbox",
        "data-field": "enabled:" + track.code,
        class: "setting-input",
      })
      toggle.checked = track.enabled
      toggle.addEventListener("change", function () {
        fields.enabled[track.code] = toggle.checked === true
      })

      return el("div", { class: "setting-row" }, [
        el("span", { class: "setting-name", text: track.label }),
        weightInput,
        toggle,
        el("span", {
          class: "setting-hint",
          text: track.enabled
            ? "权重 " + track.weight + " · 可学 " + track.unit_count + " 个"
            : "已关闭，不参与排课",
        }),
      ])
    })
    trackChildren.push(
      el("p", {
        class: "faint",
        text: "权重合计 " + weightSum(tracks) + "。权重是相对值，不用凑成 1。",
      })
    )
    view.appendChild(UI.panel({ title: "跨轨权重", children: trackChildren }))

    /* ---------- 复习次数阈值：普通卡 ---------- */

    const kInput = el("input", {
      type: "number",
      min: "1",
      step: "1",
      "data-field": "count_trigger_k",
      class: "setting-input",
    })
    kInput.value = String(fields.count_trigger_k)
    kInput.addEventListener("change", function () {
      fields.count_trigger_k = Number(kInput.value)
    })
    view.appendChild(
      UI.panel({
        title: "复习次数阈值",
        children: [
          el("div", { class: "setting-row" }, [
            el("span", { class: "setting-name", text: "学几个就复习" }),
            kInput,
            el("span", {
              class: "setting-hint",
              text: "每学 K 个单元，到期的复习卡就该回顾一次；日期到期也算。",
            }),
          ]),
        ],
      })
    )

    /* ---------- 环境自检：静默卡 ---------- */

    const envChildren = envRows(cfg.env).map(function (row) {
      return el("div", { class: "setting-row" }, [
        el("span", { class: "setting-name", text: row.label }),
        UI.pill(row.ok ? "已就绪" : "缺失", row.ok ? "ok" : "bad"),
        el("span", { class: "setting-hint", text: row.detail }),
      ])
    })
    envChildren.push(
      el("p", {
        class: "faint",
        text: "缺 NumPy 时 AI 轨整轨跳过，不影响算法轨与数据库轨。",
      })
    )
    view.appendChild(UI.panel({ title: "环境自检", className: "quiet", children: envChildren }))
  }

  A.pages.settings = {
    title: "设置",
    render: render,
  }

  // Node（单测）下导出纯函数与渲染函数——老测试直接用同一份实现
  if (typeof module !== "undefined" && module.exports) {
    module.exports = {
      normalizeTracks: normalizeTracks,
      weightSum: weightSum,
      buildPayload: buildPayload,
      validatePayload: validatePayload,
      envRows: envRows,
      render: render,
    }
  }
})()
