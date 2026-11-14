"use strict"
/**
 * llm.js —— LLM 工作台：讲解 / 求助 / 造题 / 补全 / 设置
 *
 * 零构建、零依赖的原生 JS。整个模块挂到 window.AlgoPlanLlm。
 *
 * 页面接入约定（**不依赖 M2–M5 的内部函数名**）：
 *   - 本文件在 DOMContentLoaded 之后自动把「LLM 工作台」挂到 main#view 顶部；
 *   - app.js 每次重渲染会清空 #view，本文件用 MutationObserver 重新挂载；
 *   - 页面上出现带 data-problem-id 的元素时，自动把它当作「当前题」；
 *   - 就算没有那个属性，工作台自带的题目下拉框（从 /api/bank 拉）照样能用。
 *
 * 纪律：
 *   - 不主动泄题：讲解默认「讲思路」；「直接给代码」是另一个独立按钮，点之前要 confirm；
 *     求助四档逐级解锁，用完一档才放开下一档。
 *   - 产出可追溯：造题与补全由后端落盘，前端只显示结果与落盘位置。
 */
;(function (global) {
  /** 求助的四档：方向 → 思路 → 伪码 → 代码 */
  var HINT_LEVELS = ["方向", "思路", "伪码", "代码"]

  /** 讲解的两档：先思路，后代码 */
  var EXPLAIN_LEVELS = ["思路", "代码"]

  /** 讲解按钮上的字 */
  var EXPLAIN_LABELS = { 思路: "讲思路", 代码: "直接给代码" }

  /** 逐级解锁的进度存在 sessionStorage 里，键前缀 */
  var UNLOCK_KEY_PREFIX = "study-hub-llm-unlock-"

  /** 会话号：成本护栏按会话计数，所有请求都要带着 */
  var SESSION_ID_KEY = "study-hub-llm-session-id"

  /** 没有 sessionStorage 时（比如被 node:test 直接 require）用内存兜底 */
  var memoryStore = {}

  // ------------------------------------------------------------
  // 纯逻辑区：这一段不碰 DOM，可以被 node:test 直接测
  // ------------------------------------------------------------

  /** 档位在四档里的下标；认不出来的返回 -1 */
  function hintLevelIndex(level) {
    for (var i = 0; i < HINT_LEVELS.length; i += 1) {
      if (HINT_LEVELS[i] === level) return i
    }
    return -1
  }

  /** 这一档现在能不能用：下标不超过已解锁到的下标 */
  function isHintLevelUnlocked(unlockedIndex, level) {
    var idx = hintLevelIndex(level)
    if (idx === -1) return false
    return idx <= unlockedIndex
  }

  /**
   * 用完某一档之后，下一档解锁到哪儿。
   * 解锁进度只前进不回退；最高档用完也不越界。
   */
  function nextUnlockedIndex(currentIndex, level) {
    var idx = hintLevelIndex(level)
    if (idx === -1) return currentIndex
    var next = idx + 1
    if (next > HINT_LEVELS.length - 1) next = HINT_LEVELS.length - 1
    return next > currentIndex ? next : currentIndex
  }

  /** 讲解请求体。**默认只到思路层**；要代码必须显式传 level="代码"。 */
  function explainRequestBody(problemId, level, sid) {
    var lv = EXPLAIN_LEVELS.indexOf(level) !== -1 ? level : "思路"
    return { problemId: problemId, level: lv, sessionId: sid }
  }

  /** 求助请求体 */
  function hintRequestBody(problemId, level, sid) {
    return { problemId: problemId, level: level, sessionId: sid }
  }

  /** 取 sessionStorage；没有或被浏览器禁用就返回 null */
  function getSessionStorage() {
    try {
      return global.sessionStorage || null
    } catch (err) {
      return null
    }
  }

  /** 某题已经解锁到第几档；默认 0（只有「方向」可用） */
  function getUnlocked(problemId) {
    var key = UNLOCK_KEY_PREFIX + problemId
    var ss = getSessionStorage()
    if (ss) {
      try {
        var raw = ss.getItem(key)
        return raw === null ? 0 : (Number(raw) || 0)
      } catch (err) {
        // 落到内存兜底
      }
    }
    return Number(memoryStore[key] || 0)
  }

  /** 记下某题解锁到第几档 */
  function setUnlocked(problemId, index) {
    var key = UNLOCK_KEY_PREFIX + problemId
    var ss = getSessionStorage()
    if (ss) {
      try {
        ss.setItem(key, String(index))
        return
      } catch (err) {
        // 落到内存兜底
      }
    }
    memoryStore[key] = index
  }

  /** 清掉所有解锁进度（测试与「重置」按钮用） */
  function resetUnlocks() {
    var ss = getSessionStorage()
    if (ss) {
      try {
        for (var i = 0; i < ss.length; i += 1) {
          var k = ss.key(i)
          if (k && k.indexOf(UNLOCK_KEY_PREFIX) === 0) ss.removeItem(k)
        }
      } catch (err) {
        // 忽略
      }
    }
    for (var name in memoryStore) {
      if (Object.prototype.hasOwnProperty.call(memoryStore, name) && name.indexOf(UNLOCK_KEY_PREFIX) === 0) {
        delete memoryStore[name]
      }
    }
  }

  /** 造一个会话号 */
  function newSessionId() {
    return "s-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 8)
  }

  /** 本次会话的会话号：成本护栏按它计数 */
  function sessionId() {
    var ss = getSessionStorage()
    if (ss) {
      try {
        var id = ss.getItem(SESSION_ID_KEY)
        if (id) return id
        id = newSessionId()
        ss.setItem(SESSION_ID_KEY, id)
        return id
      } catch (err) {
        // 落到内存兜底
      }
    }
    if (!memoryStore[SESSION_ID_KEY]) {
      memoryStore[SESSION_ID_KEY] = newSessionId()
    }
    return memoryStore[SESSION_ID_KEY]
  }

  // ------------------------------------------------------------
  // DOM 区
  // ------------------------------------------------------------

  /** 造元素；attrs 里 text 设文字、on* 绑事件 */
  function el(tag, attrs, children) {
    var node = global.document.createElement(tag)
    if (attrs) {
      for (var key in attrs) {
        if (!Object.prototype.hasOwnProperty.call(attrs, key)) continue
        var value = attrs[key]
        if (value === null || value === undefined) continue
        if (key === "class") node.className = value
        else if (key === "text") node.textContent = value
        else if (key.indexOf("on") === 0 && typeof value === "function") {
          node.addEventListener(key.slice(2).toLowerCase(), value)
        } else node.setAttribute(key, value)
      }
    }
    if (children !== null && children !== undefined) {
      var list = Array.isArray(children) ? children : [children]
      for (var i = 0; i < list.length; i += 1) {
        var child = list[i]
        if (child === null || child === undefined) continue
        node.appendChild(typeof child === "string" ? global.document.createTextNode(child) : child)
      }
    }
    return node
  }

  /**
   * 公共界面构件（ui.js）：页头 / 主卡 / 普通卡 / 嵌套块 / 药丸 / 空态。
   * 没有 ui.js 时退回到本页自带的 el，保证工作台仍然能用。
   */
  var UI = global.AlgoPlanUI || null

  /** 调后端；失败时抛一个带 message 与 status 的 Error */
  function call(path, body, method) {
    var opts = {
      method: method || (body === undefined ? "GET" : "POST"),
      headers: { "content-type": "application/json" },
    }
    if (body !== undefined) opts.body = JSON.stringify(body)
    return global.fetch(path, opts).then(function (res) {
      return res.text().then(function (text) {
        var data = null
        try {
          data = text ? JSON.parse(text) : null
        } catch (err) {
          data = null
        }
        if (!res.ok) {
          var message = data && data.error
            ? String(data.error)
            : ("请求失败（HTTP " + res.status + "）")
          var failure = new Error(message)
          failure.status = res.status
          throw failure
        }
        return data
      })
    })
  }

  /** 当前状态 */
  var state = {
    problems: [],
    currentId: "",
    settings: null,
  }

  /** 主容器 */
  function getView() {
    if (!global.document) return null
    return global.document.getElementById("view")
  }

  /** 页面上带 data-problem-id 的元素 */
  function findProblemIdInView(view) {
    if (!view || !view.querySelector) return ""
    var node = view.querySelector("[data-problem-id]")
    if (!node) return ""
    return node.getAttribute("data-problem-id") || ""
  }

  /** 地址栏里的题号 */
  function findProblemIdInHash() {
    var hash = (global.location && global.location.hash) || ""
    var m = /(?:^|[#\/=])(lc|nc|jz|self|llm)-[0-9a-z]{5,12}/.exec(hash)
    return m ? m[0] : ""
  }

  /** 当前题号：优先页面上标出来的，其次地址栏，最后下拉框 */
  function currentProblemId() {
    var fromView = findProblemIdInView(getView())
    if (fromView) return fromView
    var fromHash = findProblemIdInHash()
    if (fromHash) return fromHash
    return state.currentId
  }

  /** 状态栏 */
  function setStatus(text) {
    var node = global.document.getElementById("llm-status")
    if (node) node.textContent = text || ""
  }

  // ------------------------------------------------------------
  // 讲解（先思路后代码）
  // ------------------------------------------------------------

  function onExplain(problemId, level) {
    if (!problemId) {
      setStatus("请先选一道题")
      return
    }
    if (level === "代码") {
      var ok = global.confirm
        ? global.confirm("确定直接看代码？这题基本等于抄答案了。")
        : false
      if (!ok) return
    }
    var body = explainRequestBody(problemId, level, sessionId())
    setStatus("正在讲解……")
    call("/api/llm/explain", body).then(function (data) {
      var out = global.document.getElementById("llm-explain-out")
      if (out) out.textContent = data.text
      var meta = global.document.getElementById("llm-explain-meta")
      if (meta) meta.textContent = "模型：" + data.model
      setStatus("讲解完成")
    }).catch(function (err) {
      var out = global.document.getElementById("llm-explain-out")
      if (out) out.textContent = "讲解失败：" + err.message
      setStatus("讲解失败")
    })
  }

  function renderExplainButtons(problemId) {
    var wrap = el("div", { class: "llm-buttons", id: "llm-explain-buttons" })
    EXPLAIN_LEVELS.forEach(function (level) {
      wrap.appendChild(el("button", {
        class: "btn" + (level === "代码" ? " btn-warn" : ""),
        type: "button",
        text: EXPLAIN_LABELS[level],
        onclick: function () { onExplain(problemId, level) },
      }))
    })
    return wrap
  }

  // ------------------------------------------------------------
  // 求助（四档逐级解锁）
  // ------------------------------------------------------------

  function onHint(problemId, level) {
    if (!problemId) {
      setStatus("请先选一道题")
      return
    }
    var unlocked = getUnlocked(problemId)
    if (!isHintLevelUnlocked(unlocked, level)) {
      setStatus("求助是逐级解锁的：先用「" + (HINT_LEVELS[unlocked] || "方向") + "」那一档")
      return
    }
    var body = hintRequestBody(problemId, level, sessionId())
    setStatus("正在给提示……")
    call("/api/llm/hint", body).then(function (data) {
      var next = nextUnlockedIndex(unlocked, level)
      var out = global.document.getElementById("llm-hint-out")
      if (out) out.textContent = data.text
      var meta = global.document.getElementById("llm-hint-meta")
      if (meta) meta.textContent = "模型：" + data.model + "；已解锁到「" + HINT_LEVELS[next] + "」那档"
      setUnlocked(problemId, next)
      refreshHintButtons(problemId)
      setStatus("提示已给出")
    }).catch(function (err) {
      var out = global.document.getElementById("llm-hint-out")
      if (out) out.textContent = "求助失败：" + err.message
      setStatus("求助失败")
    })
  }

  function renderHintButtons(problemId) {
    var wrap = el("div", { class: "llm-buttons", id: "llm-hint-buttons" })
    var unlocked = getUnlocked(problemId)
    HINT_LEVELS.forEach(function (level) {
      var usable = isHintLevelUnlocked(unlocked, level)
      var attrs = {
        class: "btn" + (usable ? "" : " btn-off"),
        type: "button",
        text: usable ? level : level + "（未解锁）",
        onclick: function () { onHint(problemId, level) },
      }
      if (!usable) attrs.disabled = "disabled"
      wrap.appendChild(el("button", attrs))
    })
    return wrap
  }

  /** 求助用完一档后，刷新按钮的解锁状态 */
  function refreshHintButtons(problemId) {
    var holder = global.document.getElementById("llm-hint-buttons")
    if (!holder || !holder.parentNode) return
    holder.parentNode.replaceChild(renderHintButtons(problemId), holder)
  }

  // ------------------------------------------------------------
  // 题目选择
  // ------------------------------------------------------------

  function renderProblemPicker() {
    var select = el("select", {
      id: "llm-problem",
      onchange: function (ev) {
        state.currentId = ev.target.value
        refreshForProblem()
      },
    })
    state.problems.forEach(function (p) {
      var label = p.title + "（" + p.id + "）" + (p.needsFill ? " ⚠ 缺题面或用例" : "")
      select.appendChild(el("option", { value: p.id, text: label }))
    })
    if (!state.currentId && state.problems.length > 0) {
      state.currentId = state.problems[0].id
    }
    if (state.currentId) select.value = state.currentId
    return select
  }

  /** 当前题变了，刷新讲解与求助的按钮 */
  function refreshForProblem() {
    var problemId = currentProblemId()
    var explainHolder = global.document.getElementById("llm-explain-buttons")
    if (explainHolder && explainHolder.parentNode) {
      explainHolder.parentNode.replaceChild(renderExplainButtons(problemId), explainHolder)
    }
    var hintHolder = global.document.getElementById("llm-hint-buttons")
    if (hintHolder && hintHolder.parentNode) {
      hintHolder.parentNode.replaceChild(renderHintButtons(problemId), hintHolder)
    }
  }

  // ------------------------------------------------------------
  // 造题、补全、设置
  // ------------------------------------------------------------

  /** 可选的 provider 列表；"null" 表示干脆不接 LLM */
  var PROVIDERS = ["ark", "anthropic", "null"]

  /** 从题目列表里挑出缺题面或缺用例的（needs_fill: true） */
  function needsFillList(problems) {
    var list = Array.isArray(problems) ? problems : []
    return list.filter(function (p) { return !!p && p.needsFill === true })
  }

  /** 造题请求体 */
  function generateRequestBody(track, topics, difficulty, sid) {
    return {
      track: String(track || ""),
      topics: Array.isArray(topics) ? topics.slice() : [],
      difficulty: String(difficulty || "medium"),
      sessionId: sid,
    }
  }

  /** 补全请求体 */
  function fillRequestBody(problemId, sid) {
    return { problemId: String(problemId || ""), sessionId: sid }
  }

  /**
   * 设置请求体：**只保留四个白名单字段**，
   * 任何像密钥的字段（apiKey / key / token / secret）一律丢掉（G27）。
   */
  function settingsRequestBody(patch) {
    var p = patch || {}
    var out = {}
    if (p.enabled !== undefined) out.enabled = !!p.enabled
    if (p.provider !== undefined) out.provider = String(p.provider)
    if (p.model !== undefined) out.model = String(p.model)
    if (p.baseUrl !== undefined) out.baseUrl = String(p.baseUrl)
    return out
  }

  /** 把后端报的密钥来源讲成人话 */
  function describeKeySource(settings) {
    var s = settings || {}
    if (!s.effectiveProvider || s.effectiveProvider === "null") {
      return "LLM 未启用：" + (s.reason || "没有配置密钥")
    }
    return "密钥来自 " + (s.keySource || "环境变量")
      + "；当前 provider 是 " + s.effectiveProvider
      + "，模型 " + (s.effectiveModel || "（未设置）")
  }

  /** 把输入框里的知识点拆成数组（顿号、逗号、空格都算分隔符） */
  function splitTopics(text) {
    return String(text || "")
      .split(/[,，、\s]+/)
      .filter(function (t) { return t !== "" })
  }

  /** 「来一题新的」：调后端造题，**由后端落盘进 learning/bank**（G25） */
  function onGenerate() {
    var track = global.document.getElementById("llm-gen-track")
    var difficulty = global.document.getElementById("llm-gen-difficulty")
    var topicsInput = global.document.getElementById("llm-gen-topics")
    var body = generateRequestBody(
      track ? track.value : "",
      splitTopics(topicsInput ? topicsInput.value : ""),
      difficulty ? difficulty.value : "medium",
      sessionId()
    )
    setStatus("正在造题……")
    call("/api/bank/llm-generate", body).then(function (data) {
      state.currentId = data.problem.id
      var out = global.document.getElementById("llm-gen-out")
      if (out) {
        out.textContent = "《" + data.problem.title + "》（" + data.problem.id
          + "）已生成并落盘进 learning/bank/。\n模型：" + data.model
      }
      setStatus("造题完成")
      return loadBank()
    }).then(function () {
      refreshBankViews()
    }).catch(function (err) {
      var out = global.document.getElementById("llm-gen-out")
      if (out) out.textContent = "造题失败：" + err.message
      setStatus("造题失败")
    })
  }

  /** 缺题面的题列表，每条后面跟一个「让 LLM 补全」 */
  function renderFillList() {
    var list = el("div", { class: "llm-row", id: "llm-fill-list" })
    var missing = needsFillList(state.problems)
    if (missing.length === 0) {
      list.appendChild(el("span", { class: "muted", text: "题库里没有缺题面或缺用例的题。" }))
      return list
    }
    missing.forEach(function (p) {
      list.appendChild(el("span", { class: "llm-fill-item" }, [
        el("span", { text: p.title + "（" + p.id + "）" }),
        el("button", {
          class: "btn",
          type: "button",
          text: "让 LLM 补全",
          onclick: function () { onFill(p.id) },
        }),
      ]))
    })
    return list
  }

  /** 「让 LLM 补全」：调后端补全，**由后端写回题库**（G25） */
  function onFill(problemId) {
    var body = fillRequestBody(problemId, sessionId())
    setStatus("正在补全……")
    call("/api/llm/fill", body).then(function (data) {
      state.currentId = data.problem.id
      var out = global.document.getElementById("llm-fill-out")
      if (out) {
        out.textContent = "《" + data.problem.title + "》（" + data.problem.id
          + "）已补全并写回题库。\n模型：" + data.model
      }
      setStatus("补全完成")
      return loadBank()
    }).then(function () {
      refreshBankViews()
    }).catch(function (err) {
      var out = global.document.getElementById("llm-fill-out")
      if (out) out.textContent = "补全失败：" + err.message
      setStatus("补全失败")
    })
  }

  /** 保存 LLM 设置 */
  function onSaveSettings() {
    var enabledBox = global.document.getElementById("llm-set-enabled")
    var providerSelect = global.document.getElementById("llm-set-provider")
    var modelInput = global.document.getElementById("llm-set-model")
    var patch = {}
    if (enabledBox) patch.enabled = !!enabledBox.checked
    if (providerSelect) patch.provider = providerSelect.value
    if (modelInput) patch.model = modelInput.value
    var body = settingsRequestBody(patch)
    setStatus("正在保存设置……")
    call("/api/llm/settings", body, "PUT").then(function () {
      setStatus("设置已保存")
      return loadSettings()
    }).then(function () {
      mountPanel()
    }).catch(function (err) {
      setStatus("保存失败：" + err.message)
    })
  }

  /** 造题区块 */
  function renderGenerateSection() {
    var settings = state.settings || {}
    var tracks = Array.isArray(settings.tracks) ? settings.tracks : []
    var difficulties = Array.isArray(settings.difficulties) ? settings.difficulties : ["easy", "medium", "hard"]
    var trackSelect = el("select", { id: "llm-gen-track" })
    tracks.forEach(function (t) {
      trackSelect.appendChild(el("option", { value: String(t), text: String(t) }))
    })
    var diffSelect = el("select", { id: "llm-gen-difficulty" })
    difficulties.forEach(function (d) {
      diffSelect.appendChild(el("option", { value: String(d), text: String(d) }))
    })
    return el("div", { class: "llm-block" }, [
      el("div", { class: "llm-row" }, [
        el("strong", { text: "来一题新的" }),
        el("span", { class: "muted", text: "轨道" }),
        trackSelect,
        el("span", { class: "muted", text: "难度" }),
        diffSelect,
        el("span", { class: "muted", text: "知识点（顿号或逗号隔开）" }),
        el("input", { id: "llm-gen-topics", type: "text", placeholder: "哈希表、数组" }),
        el("button", {
          class: "btn",
          type: "button",
          text: "来一题新的",
          onclick: onGenerate,
        }),
      ]),
      el("pre", { id: "llm-gen-out", class: "llm-out", text: "" }),
    ])
  }

  /** 补全区块 */
  function renderFillSection() {
    return el("div", { class: "llm-block" }, [
      el("div", { class: "llm-row" }, [
        el("strong", { text: "补全缺口" }),
        el("span", { class: "muted", text: "题面或测试用例缺失的题，可以让 LLM 反推补齐。" }),
      ]),
      renderFillList(),
      el("pre", { id: "llm-fill-out", class: "llm-out", text: "" }),
    ])
  }

  /** 设置区块 */
  function renderSettingsSection() {
    var s = state.settings || {}
    var enabledBox = el("input", { id: "llm-set-enabled", type: "checkbox" })
    if (s.enabled) enabledBox.checked = true
    var providerSelect = el("select", { id: "llm-set-provider" })
    PROVIDERS.forEach(function (p) {
      var label = p === "null" ? "关闭（不接 LLM）" : p
      providerSelect.appendChild(el("option", { value: p, text: label }))
    })
    if (s.provider) providerSelect.value = s.provider
    var modelInput = el("input", { id: "llm-set-model", type: "text", placeholder: "留空用默认模型" })
    if (s.model) modelInput.value = s.model
    return el("div", { class: "llm-block" }, [
      el("div", { class: "llm-row" }, [
        el("strong", { text: "LLM 设置" }),
        el("span", { class: "muted", text: "启用" }),
        enabledBox,
        el("span", { class: "muted", text: "provider" }),
        providerSelect,
        el("span", { class: "muted", text: "模型名" }),
        modelInput,
        el("button", { class: "btn", type: "button", text: "保存", onclick: onSaveSettings }),
      ]),
      el("div", { class: "llm-meta muted", text: describeKeySource(s) }),
      el("div", {
        class: "llm-meta muted",
        text: "密钥不进这个页面，也不进 settings 表：只从环境变量读。",
      }),
    ])
  }

  /** 造题或补全之后，只刷新题库相关的两块（讲解与求助的输出不动） */
  function refreshBankViews() {
    var picker = global.document.getElementById("llm-problem")
    if (picker && picker.parentNode) {
      picker.parentNode.replaceChild(renderProblemPicker(), picker)
    }
    var fillList = global.document.getElementById("llm-fill-list")
    if (fillList && fillList.parentNode) {
      fillList.parentNode.replaceChild(renderFillList(), fillList)
    }
  }

  // ------------------------------------------------------------
  // 面板与挂载
  // ------------------------------------------------------------

  /** 卡内嵌套块：ui.js 的 block，标题是中文小节名 */
  function makeBlock(title, children) {
    if (UI && typeof UI.block === "function") return UI.block({ title: title, children: children })
    var kids = [el("p", { class: "block-title", text: title })].concat(children || [])
    return el("div", { class: "block" }, kids)
  }

  /** 普通卡：ui.js 的 panel */
  function makePanel(title, children, className) {
    if (UI && typeof UI.panel === "function") {
      return UI.panel({ title: title, children: children, className: className })
    }
    var kids = []
    if (title) kids.push(el("h2", { text: title }))
    ;(children || []).forEach(function (child) { if (child) kids.push(child) })
    return el("section", { class: "panel" + (className ? " " + className : "") }, kids)
  }

  function renderPanel() {
    var parts = []
    parts.push(el("p", {
      class: "muted",
      text: "LLM 是可选增强：关掉也不影响排程、判题、复习与进度。",
    }))
    parts.push(el("div", { class: "llm-row" }, [
      el("span", { class: "llm-label", text: "当前题目" }),
      renderProblemPicker(),
      el("button", {
        class: "btn",
        type: "button",
        text: "刷新题库",
        onclick: function () { mount(true) },
      }),
      el("span", { id: "llm-status", class: "muted", text: "" }),
    ]))

    parts.push(makeBlock("讲解", [
      el("div", { class: "llm-row" }, [
        renderExplainButtons(currentProblemId()),
        el("span", { class: "muted", text: "默认只讲思路；「直接给代码」要自己点。" }),
      ]),
      el("pre", { id: "llm-explain-out", class: "llm-out", text: "" }),
      el("div", { id: "llm-explain-meta", class: "llm-meta muted", text: "" }),
    ]))

    parts.push(makeBlock("求助", [
      el("div", { class: "llm-row" }, [
        renderHintButtons(currentProblemId()),
        el("span", { class: "muted", text: "四档逐级解锁：用完一档才放开下一档。" }),
      ]),
      el("pre", { id: "llm-hint-out", class: "llm-out", text: "" }),
      el("div", { id: "llm-hint-meta", class: "llm-meta muted", text: "" }),
    ]))

    parts.push(renderGenerateSection())
    parts.push(renderFillSection())
    parts.push(renderSettingsSection())

    return makePanel("LLM 工作台", parts, "llm-panel")
  }

  /** 把面板挂到 main#view 顶部 */
  function mountPanel() {
    var view = getView()
    if (!view) return
    var old = global.document.getElementById("llm-panel")
    if (old && old.parentNode) old.parentNode.removeChild(old)
    view.insertBefore(renderPanel(), view.firstChild)
    if (state.settings) {
      var st = global.document.getElementById("llm-status")
      if (st && !st.textContent) {
        st.textContent = state.settings.effectiveProvider === "null"
          ? ("LLM 未启用：" + state.settings.reason)
          : ("LLM：" + state.settings.effectiveProvider + " / " + state.settings.effectiveModel)
      }
    }
  }

  /** 拉题库 */
  function loadBank() {
    return call("/api/bank").then(function (data) {
      state.problems = data && data.items ? data.items : []
      return state.problems
    }).catch(function () {
      state.problems = []
      return state.problems
    })
  }

  /** 拉 LLM 设置 */
  function loadSettings() {
    return call("/api/llm/settings").then(function (data) {
      state.settings = data
      return data
    }).catch(function () {
      state.settings = null
      return null
    })
  }

  var mounting = false

  /**
   * 挂载入口：拉设置与题库，然后挂面板。
   * refresh 为真时强制重新拉题库（「刷新题库」按钮用）。
   */
  function mount(refresh) {
    if (mounting) return
    mounting = true
    loadSettings()
      .then(function () {
        if (refresh || state.problems.length === 0) return loadBank()
        return state.problems
      })
      .then(function () {
        mounting = false
        mountPanel()
      })
      .catch(function () {
        mounting = false
      })
  }

  /** 页面上出现带 data-problem-id 的元素时，同步当前题 */
  function enhance() {
    var id = findProblemIdInView(getView())
    if (id && id !== state.currentId) {
      state.currentId = id
      refreshForProblem()
    }
  }

  var exported = {
    HINT_LEVELS: HINT_LEVELS,
    EXPLAIN_LEVELS: EXPLAIN_LEVELS,
    hintLevelIndex: hintLevelIndex,
    isHintLevelUnlocked: isHintLevelUnlocked,
    nextUnlockedIndex: nextUnlockedIndex,
    explainRequestBody: explainRequestBody,
    hintRequestBody: hintRequestBody,
    getUnlocked: getUnlocked,
    setUnlocked: setUnlocked,
    resetUnlocks: resetUnlocks,
    sessionId: sessionId,
    needsFillList: needsFillList,
    generateRequestBody: generateRequestBody,
    fillRequestBody: fillRequestBody,
    settingsRequestBody: settingsRequestBody,
    describeKeySource: describeKeySource,
  }

  if (typeof module !== "undefined" && module.exports) {
    module.exports = exported
  }
  global.AlgoPlanLlm = exported

  // 浏览器里自动挂载；被 node:test require 时这段不执行
  if (typeof global.document !== "undefined" && global.document.addEventListener) {
    global.document.addEventListener("DOMContentLoaded", function () {
      mount()
      var view = getView()
      if (view && global.MutationObserver) {
        var observer = new global.MutationObserver(function () {
          if (!global.document.getElementById("llm-panel")) {
            mount()
            return
          }
          enhance()
        })
        observer.observe(view, { childList: true, subtree: true })
      }
    })
  }
})(typeof globalThis !== "undefined" ? globalThis : this)
