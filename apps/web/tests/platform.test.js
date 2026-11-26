"use strict"
/**
 * platform.test.js —— 平台适配层
 *
 * 验收标准：页面代码把 wx.xxx 换成 platform.xxx 之后，行为不变。
 * 所以这里逐个断言 9 个 API 的签名与返回值形状。
 */

const test = require("node:test")
const assert = require("node:assert")

const dom = require("./dom-stub.js")
dom.install()

/**
 * Node 下没有 history/location，用一个可回退的桩。
 * 必须在 require platform.js **之前**装好 —— platform.js 在模块加载时就会读 global.history。
 */
function installHistoryStub () {
  const stack = []
  let current = "/"
  global.history = {
    pushState: function (state, title, url) { stack.push(current); current = url },
    replaceState: function (state, title, url) { current = url },
    back: function () { const prev = stack.pop(); if (prev !== undefined) current = prev },
    get length () { return stack.length + 1 }
  }
  Object.defineProperty(global, "location", {
    configurable: true,
    get: function () { return { pathname: current, href: "http://127.0.0.1" + current } }
  })
}

installHistoryStub()

const platform = require("../shared/platform.js")

test("showToast：把提示挂到页面上并返回", function () {
  const r = platform.showToast({ title: "已保存", icon: "success" })
  assert.ok(r !== undefined)
})

test("showActionSheet：返回被点选的序号", async function () {
  const p = platform.showActionSheet({ itemList: ["置顶", "删除"] })
  // 桩环境下模拟用户点第二项
  platform.__testClickSheet(1)
  const res = await p
  assert.strictEqual(res.tapIndex, 1)
})

test("showActionSheet：取消返回 -1", async function () {
  const p = platform.showActionSheet({ itemList: ["A", "B"] })
  platform.__testClickSheet(-1)
  const res = await p
  assert.strictEqual(res.tapIndex, -1)
})

test("showModal：确认返回 confirm=true", async function () {
  const p = platform.showModal({ title: "删除笔记？", content: "删除后无法恢复。" })
  platform.__testClickModal(true, "")
  const res = await p
  assert.strictEqual(res.confirm, true)
  assert.strictEqual(res.content, "")
})

test("showModal：取消返回 confirm=false", async function () {
  const p = platform.showModal({ title: "删除笔记？", content: "删除后无法恢复。" })
  platform.__testClickModal(false, "")
  const res = await p
  assert.strictEqual(res.confirm, false)
})

test("showModal：editable 模式带回输入内容", async function () {
  const p = platform.showModal({ title: "记一笔", editable: true })
  platform.__testClickModal(true, "今天把存储层写完了")
  const res = await p
  assert.strictEqual(res.confirm, true)
  assert.strictEqual(res.content, "今天把存储层写完了")
})

test("showModal：editable 模式取消时不带内容", async function () {
  const p = platform.showModal({ title: "记一笔", editable: true })
  platform.__testClickModal(false, "忽略")
  const res = await p
  assert.strictEqual(res.confirm, false)
  assert.strictEqual(res.content, "")
})

test("navigateTo：pushState 到目标路径", function () {
  const before = global.history.length
  platform.navigateTo("/workbench/tech/")
  assert.strictEqual(global.location.pathname, "/workbench/tech/")
  assert.ok(global.history.length >= before)
})

test("navigateBack：回退一步", function () {
  platform.navigateTo("/a/")
  platform.navigateTo("/b/")
  platform.navigateBack()
  assert.strictEqual(global.location.pathname, "/a/")
})

test("switchTab：替换当前路径而不是压栈", function () {
  platform.navigateTo("/a/")
  platform.switchTab("/knowledge/")
  assert.strictEqual(global.location.pathname, "/knowledge/")
})

test("showLoading / hideLoading：成对使用不抛异常", function () {
  platform.showLoading({ title: "保存中…" })
  platform.hideLoading()
})

test("previewImage：不抛异常", function () {
  platform.previewImage({ current: "blob:1", urls: ["blob:1", "blob:2"] })
})

test("stopPullDownRefresh：Web 无此手势，空实现不抛异常", function () {
  platform.stopPullDownRefresh()
})

test("chooseMedia：没有文件选择器时 reject 中文错误", async function () {
  await assert.rejects(
    function () { return platform.chooseMedia({ count: 3 }) },
    function (err) {
      assert.ok(err && err.message.indexOf("文件") >= 0, "错误信息应为中文，实际：" + (err && err.message))
      return true
    }
  )
})
