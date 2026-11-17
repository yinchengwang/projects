"use strict"
/**
 * p1-web-settings.test.js —— 会话设置页
 *
 * 测三件事：
 *   1. 纯函数算得对（权重、请求体、校验、环境行）
 *   2. 界面层次在（页头 / 主卡 / 静默卡 / 数字块）
 *   3. 改值 → 点保存 → PUT 的请求体是新值；填错就不发请求
 */

const test = require("node:test")
const assert = require("node:assert")

const stub = require("./helpers/dom-stub.js")

// 必须在 require web/app.js 之前把 document 铺好
const dom = stub.createDocument()
global.document = dom.document
global.window = global

require("../web/app.js")
const settingsPage = require("../web/pages/settings.js")

/** 铺一个假的 fetch：按路径前缀返回预设响应 */
function stubFetch(routes) {
  global.fetch = async function (url, options) {
    const key = Object.keys(routes).find(function (prefix) {
      return String(url).indexOf(prefix) === 0
    })
    if (!key) {
      return {
        ok: false,
        status: 404,
        json: async function () {
          return { error: "没有这条路由" }
        },
      }
    }
    const body = routes[key]
    if (typeof body === "function") return body(url, options)
    return {
      ok: true,
      status: 200,
      json: async function () {
        return body
      },
    }
  }
}

function findAllByClass(node, className) {
  const out = []
  function walk(n) {
    if (!n) return
    if (n.className === className) out.push(n)
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
}

function findByClass(node, className) {
  return findAllByClass(node, className)[0] || null
}

function findAllByToken(node, token) {
  const out = []
  function walk(n) {
    if (!n) return
    const parts = String(n.className || "").split(/\s+/)
    if (parts.indexOf(token) !== -1) out.push(n)
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
}

function findByAttr(node, attr, value) {
  let hit = null
  function walk(n) {
    if (hit || !n) return
    if (n.attributes && n.attributes[attr] === value) {
      hit = n
      return
    }
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return hit
}

function findButton(node, label) {
  let hit = null
  function walk(n) {
    if (hit || !n) return
    if (n.tagName === "button" && String(n.textContent || "").indexOf(label) !== -1) {
      hit = n
      return
    }
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return hit
}

function collectText(node) {
  let out = ""
  function walk(n) {
    if (!n) return
    if (n.textContent) out += " " + n.textContent
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
}

function mountView() {
  const node = dom.document.createElement("div")
  node.setAttribute("id", "view")
  return node
}

const SETTINGS = {
  session: { default_count: 3, min_count: 1, max_count: 10 },
  tracks: [
    { code: "algorithm", label: "算法", weight: 0.4, enabled: true, unit_count: 12 },
    { code: "database", label: "数据库", weight: 0.3, enabled: true, unit_count: 16 },
    { code: "ai", label: "AI", weight: 0.3, enabled: true, unit_count: 13 },
  ],
  review: { count_trigger_k: 5 },
  env: { python: { ok: true, detail: "python3 3.12" }, numpy: { ok: true, detail: "numpy 1.26" } },
  warnings: [],
}

/* ---------------- 纯函数 ---------------- */

test("normalizeTracks：把设置里的轨归一成整齐的一行", function () {
  const tracks = settingsPage.normalizeTracks(SETTINGS.tracks)
  assert.strictEqual(tracks.length, 3)
  assert.strictEqual(tracks[0].code, "algorithm")
  assert.strictEqual(tracks[0].weight, 0.4)
  assert.strictEqual(tracks[0].enabled, true)
  assert.strictEqual(tracks[2].unit_count, 13)
})

test("normalizeTracks：缺字段也不炸", function () {
  const tracks = settingsPage.normalizeTracks([{ code: "ai" }, null])
  assert.strictEqual(tracks.length, 2)
  assert.strictEqual(tracks[0].label, "AI")
  assert.strictEqual(tracks[0].weight, 0)
  assert.strictEqual(tracks[1].code, "")
})

test("buildPayload：请求体形状与契约 §8.2.2 一致", function () {
  const payload = settingsPage.buildPayload({
    session_count: 5,
    count_trigger_k: 7,
    weights: { algorithm: 0.5, database: 0.3, ai: 0.2 },
    enabled: { algorithm: true, database: false, ai: true },
  })
  assert.deepStrictEqual(Object.keys(payload).sort(), [
    "count_trigger_k",
    "session_count",
    "track_enabled",
    "track_weights",
  ])
  assert.strictEqual(payload.session_count, 5)
  assert.strictEqual(payload.count_trigger_k, 7)
  assert.strictEqual(payload.track_weights.algorithm, 0.5)
  assert.strictEqual(payload.track_enabled.database, false)
})

test("validatePayload：非法值各给一句中文", function () {
  assert.deepStrictEqual(settingsPage.validatePayload({ session_count: 3, count_trigger_k: 5 }), [])

  const badCount = settingsPage.validatePayload({ session_count: 99, count_trigger_k: 5 })
  assert.ok(badCount.join(" ").indexOf("会话长度必须是 1~10 的整数") !== -1, badCount.join("|"))

  const badK = settingsPage.validatePayload({ session_count: 3, count_trigger_k: 0 })
  assert.ok(badK.join(" ").indexOf("次数阈值 K 必须是大于等于 1 的整数") !== -1, badK.join("|"))

  const badWeight = settingsPage.validatePayload({
    session_count: 3,
    count_trigger_k: 5,
    track_weights: { algorithm: -1 },
  })
  assert.ok(badWeight.join(" ").indexOf("轨权重必须是不小于 0 的数字") !== -1, badWeight.join("|"))

  const badSwitch = settingsPage.validatePayload({
    session_count: 3,
    count_trigger_k: 5,
    track_enabled: { algorithm: "yes" },
  })
  assert.ok(badSwitch.join(" ").indexOf("轨开关必须是布尔值") !== -1, badSwitch.join("|"))
})

test("envRows：环境自检两行，缺的那个标成缺失", function () {
  const rows = settingsPage.envRows({ python: { ok: true, detail: "python3" } })
  assert.strictEqual(rows.length, 2)
  assert.strictEqual(rows[0].key, "python")
  assert.strictEqual(rows[0].ok, true)
  assert.strictEqual(rows[1].key, "numpy")
  assert.strictEqual(rows[1].ok, false)
  assert.ok(rows[1].label.length > 0)
})

test("weightSum：三轨权重加起来是多少", function () {
  assert.strictEqual(settingsPage.weightSum(SETTINGS.tracks), 1)
})

/* ---------------- 渲染 ---------------- */

test("设置页：页头一个、主卡一个、静默卡一个、数字块在", async function () {
  stubFetch({ "/api/settings": SETTINGS })
  const view = mountView()
  await settingsPage.render(view)

  assert.strictEqual(findAllByClass(view, "page-head").length, 1, "设置页有且只有一个页头")
  assert.ok(findByClass(view, "panel hero"), "会话长度要在主卡里")
  assert.ok(findByClass(view, "panel quiet"), "环境自检是背景信息，用静默卡")
  assert.ok(findByClass(view, "stat-grid"), "概况要摆成数字块")
})

test("设置页：三轨权重与开关都摆出来了", async function () {
  stubFetch({ "/api/settings": SETTINGS })
  const view = mountView()
  await settingsPage.render(view)

  const names = findAllByToken(view, "setting-name").map(function (n) {
    return n.textContent
  })
  assert.ok(names.join(" ").indexOf("算法") !== -1, "轨名要摆出来：" + names.join(","))
  assert.ok(names.join(" ").indexOf("数据库") !== -1)
  assert.ok(names.join(" ").indexOf("AI") !== -1)

  const weight = findByAttr(view, "data-field", "weight:algorithm")
  assert.ok(weight, "要有算法轨的权重输入框")
  const toggle = findByAttr(view, "data-field", "enabled:database")
  assert.ok(toggle, "要有数据库轨的开关")
})

test("设置页：环境自检用中文药丸承载状态", async function () {
  stubFetch({
    "/api/settings": {
      session: SETTINGS.session,
      tracks: SETTINGS.tracks,
      review: SETTINGS.review,
      env: { python: { ok: true, detail: "python3 3.12" }, numpy: { ok: false, detail: "没装 numpy" } },
      warnings: [],
    },
  })
  const view = mountView()
  await settingsPage.render(view)

  const pills = findAllByToken(view, "pill").map(function (p) {
    return p.textContent
  })
  assert.ok(pills.join(" ").indexOf("已就绪") !== -1, "状态要有中文文字：" + pills.join(","))
  assert.ok(pills.join(" ").indexOf("缺失") !== -1)
})

test("设置页：改会话长度后点保存，PUT 的请求体是新值", async function () {
  let put = null
  stubFetch({
    "/api/settings": function (url, options) {
      if (options && options.method === "PUT") {
        put = JSON.parse(options.body)
        return { ok: true, status: 200, json: async function () { return SETTINGS } }
      }
      return { ok: true, status: 200, json: async function () { return SETTINGS } }
    },
  })
  const view = mountView()
  await settingsPage.render(view)

  const input = findByAttr(view, "data-field", "session_count")
  assert.ok(input, "要有会话长度输入框")
  input.value = "5"
  input.dispatch("change")

  const btn = findButton(view, "保存设置")
  assert.ok(btn, "要有保存按钮")
  btn.dispatch("click")
  await new Promise(function (resolve) {
    setTimeout(resolve, 0)
  })

  assert.ok(put, "点了要发 PUT")
  assert.strictEqual(put.session_count, 5)
  assert.strictEqual(put.count_trigger_k, 5)
  assert.strictEqual(put.track_weights.algorithm, 0.4)
  assert.ok(collectText(view).indexOf("已保存") !== -1, "要给出保存成功的提示")
})

test("设置页：填错就不发请求，直接给中文错", async function () {
  let put = null
  stubFetch({
    "/api/settings": function (url, options) {
      if (options && options.method === "PUT") {
        put = JSON.parse(options.body)
        return { ok: true, status: 200, json: async function () { return SETTINGS } }
      }
      return { ok: true, status: 200, json: async function () { return SETTINGS } }
    },
  })
  const view = mountView()
  await settingsPage.render(view)

  const input = findByAttr(view, "data-field", "session_count")
  input.value = "99"
  input.dispatch("change")
  findButton(view, "保存设置").dispatch("click")
  await new Promise(function (resolve) {
    setTimeout(resolve, 0)
  })

  assert.strictEqual(put, null, "填错不该发请求")
  assert.ok(
    collectText(view).indexOf("会话长度必须是 1~10 的整数") !== -1,
    "要有中文错：" + collectText(view)
  )
})

test("设置页：接口报错时显示中文错误，不崩", async function () {
  global.fetch = async function () {
    return {
      ok: false,
      status: 500,
      json: async function () {
        return { error: "服务端出错" }
      },
    }
  }
  const view = mountView()
  await settingsPage.render(view)
  assert.ok(collectText(view).indexOf("加载失败") !== -1, "要有错误提示：" + collectText(view))
})

test("normalizeTracks：enabled 为 false 的轨要保留成 false", function () {
  // 原「把设置里的轨归一成整齐的一行」只验了 weight / unit_count / label，
  // 没验 enabled 这一位。把 `enabled: t.enabled === false ? false : true` 改成恒 true
  // 依然全绿（实测存活）—— 而设置页的开关状态正是靠它渲染。
  const rows = settingsPage.normalizeTracks([
    { code: "algorithm", label: "算法", weight: 0.4, enabled: true, unit_count: 10 },
    { code: "database", label: "数据库", weight: 0.3, enabled: false, unit_count: 5 },
  ])
  assert.strictEqual(rows[0].enabled, true)
  assert.strictEqual(rows[1].enabled, false, "关掉的轨必须仍是 false")
})

test("buildPayload：权重与开关都转成请求体要的类型", function () {
  // 原「请求体形状与契约 §8.2.2 一致」的夹具里传的就是数字与布尔，
  // 所以「Number(...)」「=== true」这两步转换根本没被执行到。
  // 传字符串数字 / 真值非布尔，才能证明转换真的发生了。
  const p = settingsPage.buildPayload({
    session_count: "4",
    count_trigger_k: "6",
    weights: { algorithm: "0.5" },
    enabled: { algorithm: "yes", database: false },
  })
  assert.strictEqual(p.session_count, 4, "要转成数字：" + typeof p.session_count)
  assert.strictEqual(p.count_trigger_k, 6)
  assert.strictEqual(p.track_weights.algorithm, 0.5)
  assert.strictEqual(p.track_enabled.algorithm, false, "非布尔真值要归一成 false")
  assert.strictEqual(p.track_enabled.database, false)
})

test("weightSum：浮点尾巴收掉，且是四舍五入而不是截断", function () {
  // 两件事一起验：
  //   ① 浮点尾巴要收（0.1+0.2 不能给 0.30000000000000004）
  //   ② 收法是 **四舍五入**（Math.round），不是截断（Math.trunc）
  // 只验 ① 抓不到「round 改 trunc」——实测 0.1+0.2 的 round 与 trunc 结果恰好都是 0.3。
  // 要抓 ② 得用一个第 5 位会引起进位的小数：0.99999 → round 得 1、trunc 得 0.9999。
  const sum = settingsPage.weightSum([
    { code: "algorithm", weight: 0.1 },
    { code: "database", weight: 0.2 },
  ])
  assert.ok(Math.abs(sum - 0.3) < 1e-9, "0.1+0.2 应收成 0.3，实际：" + String(sum))
  assert.notStrictEqual(sum, 0.1 + 0.2, "不能把浮点尾巴原样抛出来：" + String(sum))

  const carries = settingsPage.weightSum([{ code: "algorithm", weight: 0.99999 }])
  assert.strictEqual(carries, 1, "0.99999 四舍五入到 4 位应是 1，截断会得 0.9999：" + String(carries))
})
