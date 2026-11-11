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
