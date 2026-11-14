"use strict"
/**
 * web-dom.test.js —— 前端渲染路径的冒烟测试
 *
 * 用 tests/helpers/dom-stub.js 的极简 DOM 桩驱动真实的 web/app.js 渲染代码，
 * 不引 jsdom（本仓零依赖铁律）。覆盖三件以前没人管的事：
 *   1. 题库行确实渲染出了「作答」按钮
 *   2. 样例用例确实被渲染出来（接口只放行前 2 个，展示出来是给用户对答案用的）
 *   3. 用户写的代码在重渲染之后还在（切页签不能把草稿冲掉）
 * 外加一条渲染竞态的守卫。
 */

const test = require("node:test")
const assert = require("node:assert")

const stub = require("./helpers/dom-stub.js")

// 必须在 require web/app.js 之前把 document / fetch 铺好
const dom = stub.createDocument()
global.document = dom.document

const panel = require("../web/app.js")
const state = panel.__test.state

/** 铺一个假的 fetch：按路径返回预设响应 */
function stubFetch(routes) {
  global.fetch = async function (url) {
    const key = Object.keys(routes).find(function (prefix) {
      return String(url).indexOf(prefix) === 0
    })
    if (!key) {
      return { ok: false, status: 404, json: async function () { return { error: "没有这条路由" } } }
    }
    const body = routes[key]
    return { ok: true, status: 200, json: async function () { return body } }
  }
}

/** 造一个带 id 的容器节点，同时塞进 document 的注册表 */
function mountContainer(id) {
  const node = dom.document.createElement("div")
  node.setAttribute("id", id)
  return node
}

/** 在树里找出所有带某个属性值的节点 */
function findAllByAttr(node, key, value) {
  const out = []
  function walk(n) {
    if (!n) return
    if (n.attributes && n.attributes[key] === value) out.push(n)
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
}

test("原题入口：有直达地址给「去原题」，只有搜索地址退到「去源站搜索」", function () {
  assert.deepStrictEqual(panel.describeSourceLink({ url: "https://x/1", searchUrl: "" }), {
    url: "https://x/1",
    label: "去原题",
    kind: "direct",
  })
  assert.deepStrictEqual(panel.describeSourceLink({ url: "", searchUrl: "https://s/2" }), {
    url: "https://s/2",
    label: "去源站搜索",
    kind: "search",
  })
  assert.deepStrictEqual(panel.describeSourceLink({ url: "", searchUrl: "" }), {
    url: "",
    label: "暂无链接",
    kind: "none",
  })
})

test("题库每一行都带一个通往原题的链接", function () {
  const bank = {
    total: 2,
    items: [
      {
        id: "lc-00001",
        title: "两数之和",
        difficulty: "easy",
        topics: ["哈希表"],
        status: "todo",
        needsFill: false,
        url: "https://leetcode.cn/problems/two-sum/",
        searchUrl: "",
      },
      {
        id: "cf-4-a",
        title: "西瓜",
        difficulty: "easy",
        topics: ["数学"],
        status: "todo",
        needsFill: true,
        url: "",
        searchUrl: "https://www.nowcoder.com/search?query=西瓜",
      },
    ],
  }
  const section = panel.__test.renderBank(bank)
  const links = findAllByAttr(section, "href", "https://leetcode.cn/problems/two-sum/")
  assert.strictEqual(links.length, 1, "题库行要有原题直链")
  const searches = findAllByAttr(section, "href", "https://www.nowcoder.com/search?query=西瓜")
  assert.strictEqual(searches.length, 1, "没有直达地址的行要给搜索入口")
})

test("题面为空时给出明确状态和跳转入口，而不是一句「（题面为空）」", async function () {
  stubFetch({
    "/api/problem/": {
      id: "lc-00009",
      title: "缺失题面的题",
      source: "leetcode",
      sourceId: "9",
      url: "https://leetcode.cn/problems/x/",
      searchUrl: "",
      statement: "",
      cases: { sample: [{ input: "1", expect: "2" }], total: 1 },
    },
  })

  state.problemId = "lc-00009"
  const section = await panel.__test.renderPractice()
  const text = section.textContent
  assert.ok(text.indexOf("题面还没进题库") !== -1, "要说明状态：" + text)
  assert.ok(text.indexOf("（题面为空）") === -1, "不该再出现空白提示：" + text)
  assert.ok(text.indexOf("leetcode / 9") !== -1, "要给出源站题号，方便自己搜：" + text)
  assert.ok(
    findAllByAttr(section, "href", "https://leetcode.cn/problems/x/").length >= 1,
    "要有跳转原题的入口"
  )
})

test("题面为空且没有链接时，说明去哪里找题", async function () {
  stubFetch({
    "/api/problem/": {
      id: "lc-00010",
      title: "既没题面也没链接",
      source: "nowcoder",
      sourceId: "10",
      url: "",
      searchUrl: "",
      statement: "",
      cases: { sample: [], total: 0 },
    },
  })

  state.problemId = "lc-00010"
  const section = await panel.__test.renderPractice()
  const text = section.textContent
  assert.ok(text.indexOf("还没有原题链接") !== -1, "要说清没有链接这件事：" + text)
  assert.ok(text.indexOf("sync-links") !== -1, "要指出回填链接的办法：" + text)
})

test("题库每一行都渲染出「作答」按钮", function () {
  const bank = {
    total: 1,
    items: [
      {
        id: "lc-00001",
        title: "两数之和",
        difficulty: "easy",
        topics: ["哈希表"],
        status: "todo",
        needsFill: false,
      },
    ],
  }
  const section = panel.__test.renderBank(bank)
  const text = section.textContent
  assert.ok(text.indexOf("两数之和") !== -1, text)
  assert.ok(text.indexOf("作答") !== -1, "题库行必须有作答按钮：" + text)
})

test("样例用例被渲染出来（不只是一句说明）", async function () {
  stubFetch({
    "/api/problem/": {
      id: "lc-00001",
      title: "两数之和",
      statement: "给定一个整数数组",
      cases: {
        sample: [
          { input: "2 7 11 15", expect: "0 1" },
          { input: "3 2 4", expect: "1 2" },
        ],
        total: 5,
      },
    },
  })

  state.problemId = "lc-00001"
  const section = await panel.__test.renderPractice()
  const text = section.textContent
  assert.ok(text.indexOf("样例用例") !== -1, text)
  assert.ok(text.indexOf("2 7 11 15") !== -1, "样例输入要显示：" + text)
  assert.ok(text.indexOf("0 1") !== -1, "样例期望要显示：" + text)
  assert.ok(text.indexOf("5") !== -1, "要说明总用例数：" + text)
})

test("用户写的代码在重渲染之后还在", async function () {
  stubFetch({
    "/api/problem/": {
      id: "lc-00002",
      title: "两数之和",
      statement: "题面",
      cases: { sample: [], total: 3 },
    },
  })

  state.problemId = "lc-00002"
  const first = await panel.__test.renderPractice()
  const editor = stub.findById(first, "judge-code")
  assert.ok(editor, "要有代码编辑框")
  editor.value = "int main(){ return 0; }"
  editor.dispatch("input")

  const second = await panel.__test.renderPractice()
  const editor2 = stub.findById(second, "judge-code")
  assert.strictEqual(editor2.value, "int main(){ return 0; }", "草稿不能被冲掉")
})

test("上一次的判题结果在重渲染之后还在", async function () {
  stubFetch({
    "/api/problem/": {
      id: "lc-00003",
      title: "两数之和",
      statement: "题面",
      cases: { sample: [], total: 3 },
    },
  })

  state.problemId = "lc-00003"
  state.result["lc-00003"] = {
    verdict: "pass",
    passedCases: 3,
    totalCases: 3,
    durationMs: 12,
    failedCase: null,
    message: "通过：全部 3 个用例通过",
    compilerOutput: "",
  }
  const section = await panel.__test.renderPractice()
  assert.ok(section.textContent.indexOf("通过") !== -1, section.textContent)
  delete state.result["lc-00003"]
})

test("渲染竞态：切走的视图不会被后来的过期请求追加内容", async function () {
  const view = mountContainer("view")
  mountContainer("tabs")

  // 第一次请求 /api/bank 挂住不放，第二次立刻返回
  let pending = null
  let bankCalls = 0
  global.fetch = function (url) {
    if (String(url).indexOf("/api/bank") === 0) {
      bankCalls += 1
      if (bankCalls === 1) {
        return new Promise(function (resolve) {
          pending = function () {
            resolve({ ok: true, status: 200, json: async function () { return { total: 0, items: [] } } })
          }
        })
      }
      return Promise.resolve({
        ok: true,
        status: 200,
        json: async function () {
          return { total: 0, items: [] }
        },
      })
    }
    return Promise.resolve({
      ok: true,
      status: 200,
      json: async function () {
        return { ok: true, node: "v24", learning: { ok: false }, bank: { problemCount: 0 }, attempts: 0 }
      },
    })
  }

  panel.__test.setView("bank")
  const first = panel.__test.render()

  panel.__test.setView("today")
  const second = panel.__test.render()

  await second
  assert.ok(pending, "第一次 /api/bank 请求应该还挂着")
  pending()
  await first

  // 第一次渲染被序号判定为过期，不能把题库面板追加进来
  // （今日页本来就会渲染一个题库面板，所以这里数个数：只能有一个）
  const text = view.textContent
  const bankPanels = text.split("题库（").length - 1
  assert.strictEqual(bankPanels, 1, "过期的题库面板不该被追加：" + text)
  assert.ok(text.indexOf("运行状态") !== -1, "今天的健康面板应该在：" + text)
})

/** 把树里所有文本拼起来（textContent 在 stub 上是累加的，重复无妨，能用来判断在不在） */
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

test("轮次卡片不在题库里时，今日页给「去轮次页勾掉它」而不是「去作答」", async function () {
  stubFetch({
    "/api/next": {
      action: {
        kind: "round",
        problemId: null,
        title: "virtual_env",
        estimatedMinutes: 15,
        reason: "轮次 r32 还剩 11 张卡片没做，先补「virtual_env」这个短板",
        reviewQueueLength: 0,
        roundId: "r32",
        itemId: "virtual_env",
      },
      reviewQueueLength: 0,
    },
    "/api/progress": {
      date: "2026-11-12",
      solved: 0,
      reviewed: 0,
      reviewMinutes: 0,
      quota: { questions: 0, minutes: 0, limitQuestions: 10, limitMinutes: 20, exhausted: false },
    },
    "/api/health": {
      ok: true,
      node: "v24.0.0",
      learning: { ok: true, root: "/tmp/learning", reason: "" },
      bank: { problemCount: 1, fileCount: 1, warnings: [] },
      attempts: 0,
    },
  })

  const today = require("../web/pages/today.js")
  const view = mountContainer("view")
  await today.render(view)

  const text = collectText(view)
  assert.ok(text.indexOf("去轮次页勾掉它") !== -1, text)
  assert.ok(text.indexOf("去作答") === -1, "卡片不在题库里，不该给「去作答」")
  assert.ok(text.indexOf("不在算法题库里") !== -1, text)
})

test("动作里有题目 id 时，今日页给「去作答」", async function () {
  stubFetch({
    "/api/next": {
      action: {
        kind: "round",
        problemId: "lc-00001",
        title: "两数之和",
        estimatedMinutes: 15,
        reason: "轮次 r33 还剩 11 张卡片没做",
        reviewQueueLength: 0,
        roundId: "r33",
        itemId: "two_sum",
      },
      reviewQueueLength: 0,
    },
    "/api/progress": {
      date: "2026-11-12",
      solved: 0,
      reviewed: 0,
      reviewMinutes: 0,
      quota: { questions: 0, minutes: 0, limitQuestions: 10, limitMinutes: 20, exhausted: false },
    },
    "/api/health": {
      ok: true,
      node: "v24.0.0",
      learning: { ok: true, root: "/tmp/learning", reason: "" },
      bank: { problemCount: 1, fileCount: 1, warnings: [] },
      attempts: 0,
    },
  })

  const today = require("../web/pages/today.js")
  const view = mountContainer("view")
  await today.render(view)

  const text = collectText(view)
  assert.ok(text.indexOf("去作答") !== -1, text)
  assert.ok(text.indexOf("去轮次页勾掉它") === -1, text)
})
