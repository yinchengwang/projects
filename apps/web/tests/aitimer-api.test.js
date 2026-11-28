"use strict"
/**
 * aitimer-api.test.js —— AI 定时任务产物接口
 *
 * 用合成夹具目录（临时目录）而不是仓库里真实的 ai-timer/：
 * 真实目录是 gitignored 的，新克隆的仓可能没有它，测试不能依赖它存在。
 * 通过 AI_TIMER_ROOT 环境变量把服务端指向夹具。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const http = require("node:http")

const aitimer = require("../server/aitimer.js")

/**
 * 造一个合成的 ai-timer 目录。
 * @returns {string} 夹具根目录绝对路径
 */
function makeFixture () {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "aitimer-fixture-"))

  // AI 简报：两篇日报 + 一篇索引页
  const briefing = path.join(dir, "AI简报")
  fs.mkdirSync(briefing)
  fs.writeFileSync(path.join(briefing, "2026-11-27 数据库与AI简报.md"),
    "---\nstack: db\ndifficulty: easy\nstatus: done\ntags: [briefing, daily]\ncreated: 2026-11-27\n---\n\n# 数据库与 AI 日报 · 2026-11-27\n\n正文甲。\n")
  fs.writeFileSync(path.join(briefing, "2026-11-28 数据库与AI简报.md"),
    "---\nstack: db\ndifficulty: easy\nstatus: done\nlinks:\n  - https://example.com/a\n  - https://example.com/b\nref: []\ncreated: 2026-11-28\nupdated: 2026-11-28\ntags: [briefing, daily, db, ai]\n---\n\n# 数据库与 AI 日报 · 2026-11-28\n\n正文乙。\n")
  fs.writeFileSync(path.join(briefing, "Briefing Index.md"),
    "---\ncreated: 2026-11-28\n---\n\n# Briefing Index\n\n索引页。\n")

  // 代码仓巡检：两篇巡检 + 一篇指令页
  const inspection = path.join(dir, "代码仓巡检")
  fs.mkdirSync(inspection)
  fs.writeFileSync(path.join(inspection, "代码仓巡检-2026-11-27.md"),
    "---\ncreated: 2026-11-27\n---\n\n# 代码仓巡检 · 2026-11-27\n\n巡检甲。\n")
  fs.writeFileSync(path.join(inspection, "代码仓巡检-2026-11-28.md"),
    "---\ncreated: 2026-11-28\n---\n\n# 代码仓巡检 · 2026-11-28\n\n巡检乙。\n")
  fs.writeFileSync(path.join(inspection, "巡检指令.md"),
    "---\ncreated: 2026-11-24\n---\n\n# 巡检指令\n\n指令页。\n")

  // 每日一特性：learn_deep/db 一个系列（README + 两篇），learn_deep/comp 一个系列
  const deep = path.join(dir, "每日一特性", "learn_deep")
  const db = path.join(deep, "db")
  fs.mkdirSync(db, { recursive: true })
  fs.writeFileSync(path.join(db, "README.md"),
    "---\ncreated: 2026-11-28\n---\n\n# 数据库系列\n\n> 主线根基：存储引擎、事务、索引。\n")
  fs.writeFileSync(path.join(db, "pg-hot-update.md"),
    "---\nstack: db\ndifficulty: hard\nstatus: done\ncreated: 2026-11-26\ntags: [theory, postgresql]\n---\n\n# HOT 更新\n\n正文丙。\n")
  fs.writeFileSync(path.join(db, "mysql-undo.md"),
    "---\nstack: db\ndifficulty: hard\ncreated: 2026-11-27\n---\n\n# Undo Purge\n\n正文丁。\n")
  const comp = path.join(deep, "comp")
  fs.mkdirSync(comp)
  fs.writeFileSync(path.join(comp, "README.md"),
    "---\ncreated: 2026-11-28\n---\n\n# 计算机基础系列\n\n> 非科班地基：数据表示、内存模型。\n")

  // 领域索引页
  const index = path.join(dir, "每日一特性", "_index")
  fs.mkdirSync(index, { recursive: true })
  fs.writeFileSync(path.join(index, "db.md"),
    "---\ncreated: 2026-11-28\n---\n\n# 数据库\n\n索引说明。\n")
  fs.writeFileSync(path.join(index, "comp.md"),
    "---\ncreated: 2026-11-28\n---\n\n# 计算机基础\n\n索引说明。\n")

  return dir
}

/**
 * 起一个指向夹具的服务。
 * @param {string} root 夹具根目录
 * @returns {Promise<object>} { app, base }
 */
function startServer (root) {
  const previous = process.env.AI_TIMER_ROOT
  process.env.AI_TIMER_ROOT = root
  const { createApp } = require("../server/app.js")
  const app = createApp()
  return app.listen(0).then(function (port) {
    return {
      app: app,
      base: "http://127.0.0.1:" + port,
      restore: function () {
        if (previous === undefined) delete process.env.AI_TIMER_ROOT
        else process.env.AI_TIMER_ROOT = previous
      }
    }
  })
}

/** 请求一次并返回 {status, body} */
function request (base, urlPath) {
  return new Promise(function (resolve, reject) {
    const parsed = new URL(urlPath, base)
    const req = http.request(
      { host: parsed.hostname, port: parsed.port, path: parsed.pathname + parsed.search, method: "GET" },
      function (res) {
        let body = ""
        res.on("data", function (c) { body += c })
        res.on("end", function () { resolve({ status: res.statusCode, body: body }) })
      }
    )
    req.on("error", reject)
    req.end()
  })
}

test("frontmatter：能拆出元信息与正文", function () {
  const text = "---\nstack: db\ndifficulty: easy\ncreated: 2026-11-28\n---\n\n# 标题\n\n正文。\n"
  const parsed = aitimer.parseFrontmatter(text)
  assert.strictEqual(parsed.meta.stack, "db")
  assert.strictEqual(parsed.meta.difficulty, "easy")
  assert.strictEqual(parsed.meta.created, "2026-11-28")
  assert.ok(parsed.body.indexOf("# 标题") >= 0, "正文应保留标题")
  assert.ok(parsed.body.indexOf("stack:") < 0, "正文不应含 frontmatter")
})

test("frontmatter：links 兼容内联数组与缩进列表", function () {
  const inline = aitimer.parseFrontmatter("---\ntags: [a, b, c]\n---\n正文")
  assert.deepStrictEqual(inline.meta.tags, ["a", "b", "c"])

  const block = aitimer.parseFrontmatter("---\nlinks:\n  - https://a.dev\n  - https://b.dev\n---\n正文")
  assert.deepStrictEqual(block.meta.links, ["https://a.dev", "https://b.dev"])

  const empty = aitimer.parseFrontmatter("---\nref: []\n---\n正文")
  assert.deepStrictEqual(empty.meta.ref, [])
})

test("frontmatter：没有 frontmatter 时正文原样返回", function () {
  const parsed = aitimer.parseFrontmatter("# 直接就是正文\n")
  assert.deepStrictEqual(parsed.meta, {})
  assert.strictEqual(parsed.body, "# 直接就是正文\n")
})

test("frontmatter：只保留实际出现的字段", function () {
  const parsed = aitimer.parseFrontmatter("---\nstack: db\n---\n正文")
  assert.deepStrictEqual(Object.keys(parsed.meta), ["stack"])
})

test("extractTitle：取正文第一个一级标题", function () {
  assert.strictEqual(aitimer.extractTitle("# 数据库日报\n\n正文"), "数据库日报")
  assert.strictEqual(aitimer.extractTitle("没有标题"), "")
})

test("buildIndex：扫描出三个集合", function () {
  const root = makeFixture()
  const index = aitimer.buildIndex(root)
  assert.strictEqual(index.exists, true)
  assert.strictEqual(index.collections.length, 3)
  assert.deepStrictEqual(index.collections.map(function (c) { return c.id }), ["briefing", "inspection", "learn"])
})

test("buildIndex：目录不存在时 exists 为 false 且不抛异常", function () {
  const index = aitimer.buildIndex(path.join(os.tmpdir(), "aitimer-does-not-exist-xyz"))
  assert.strictEqual(index.exists, false)
  assert.deepStrictEqual(index.collections, [])
})

test("buildIndex：简报按日期倒序，索引页沉底", function () {
  const index = aitimer.buildIndex(makeFixture())
  const briefing = index.collections.find(function (c) { return c.id === "briefing" })
  assert.strictEqual(briefing.docs.length, 3)
  assert.strictEqual(briefing.docs[0].date, "2026-11-28")
  assert.strictEqual(briefing.docs[1].date, "2026-11-27")
  assert.strictEqual(briefing.docs[2].isIndex, true, "Briefing Index 应排在最后")
})

test("buildIndex：巡检报告的日期从文件名里取", function () {
  const index = aitimer.buildIndex(makeFixture())
  const inspection = index.collections.find(function (c) { return c.id === "inspection" })
  assert.strictEqual(inspection.docs.length, 3)
  assert.strictEqual(inspection.docs[0].date, "2026-11-28")
  assert.strictEqual(inspection.docs[0].id, "inspection/2026-11-28")
})

test("buildIndex：learn 集合的每个领域是一个系列", function () {
  const index = aitimer.buildIndex(makeFixture())
  const learn = index.collections.find(function (c) { return c.id === "learn" })
  assert.strictEqual(learn.series.length, 2)

  const db = learn.series.find(function (s) { return s.id === "db" })
  assert.strictEqual(db.name, "数据库", "系列名应取自 _index/<领域>.md 的标题")
  assert.ok(db.desc.indexOf("存储引擎") >= 0, "系列简介应取自 README 的引用块")
  assert.strictEqual(db.docs.length, 3, "db 系列含 README 与两篇文章")
  assert.strictEqual(db.docs[0].isIndex, true, "README 应排第一")
  assert.ok(db.docs.some(function (d) { return d.id === "learn/db/pg-hot-update" }))
})

test("buildIndex：文档 id 全局唯一", function () {
  const index = aitimer.buildIndex(makeFixture())
  const ids = []
  index.collections.forEach(function (c) {
    ;(c.docs || []).forEach(function (d) {
      if (!d.path) return
      ids.push(d.id)
    })
  })
  assert.strictEqual(new Set(ids).size, ids.length, "文档 id 不应重复")
})

test("接口：索引返回三个集合", async function (t) {
  const server = await startServer(makeFixture())
  t.after(function () { server.restore(); return server.app.close() })

  const r = await request(server.base, "/api/aitimer/index")
  assert.strictEqual(r.status, 200)
  const body = JSON.parse(r.body)
  assert.strictEqual(body.ok, true)
  assert.strictEqual(body.exists, true)
  assert.strictEqual(body.collections.length, 3)
})

test("接口：读文档返回正文与元信息", async function (t) {
  const server = await startServer(makeFixture())
  t.after(function () { server.restore(); return server.app.close() })

  const r = await request(server.base, "/api/aitimer/doc?id=briefing/2026-11-28")
  assert.strictEqual(r.status, 200)
  const body = JSON.parse(r.body)
  assert.strictEqual(body.ok, true)
  assert.strictEqual(body.doc.title, "数据库与 AI 日报 · 2026-11-28")
  assert.ok(body.doc.body.indexOf("正文乙") >= 0)
  assert.ok(body.doc.body.indexOf("stack:") < 0, "正文不应含 frontmatter")
  assert.deepStrictEqual(body.doc.meta.links, ["https://example.com/a", "https://example.com/b"])
})

test("接口：读 learn 系列里的文章", async function (t) {
  const server = await startServer(makeFixture())
  t.after(function () { server.restore(); return server.app.close() })

  const r = await request(server.base, "/api/aitimer/doc?id=learn/db/pg-hot-update")
  assert.strictEqual(r.status, 200)
  const body = JSON.parse(r.body)
  assert.strictEqual(body.doc.title, "HOT 更新")
  assert.strictEqual(body.doc.meta.difficulty, "hard")
})

test("接口：读原文接口返回含 frontmatter 的全文", async function (t) {
  const server = await startServer(makeFixture())
  t.after(function () { server.restore(); return server.app.close() })

  const r = await request(server.base, "/api/aitimer/raw?id=briefing/2026-11-27")
  assert.strictEqual(r.status, 200)
  const body = JSON.parse(r.body)
  assert.ok(body.raw.indexOf("stack: db") >= 0, "原文应含 frontmatter")
})

test("接口：文档 id 无效时返回 404", async function (t) {
  const server = await startServer(makeFixture())
  t.after(function () { server.restore(); return server.app.close() })

  const r = await request(server.base, "/api/aitimer/doc?id=bogus/nope")
  assert.strictEqual(r.status, 404)
  assert.ok(JSON.parse(r.body).error.indexOf("未找到") >= 0)
})

test("接口：缺 id 参数时返回 400", async function (t) {
  const server = await startServer(makeFixture())
  t.after(function () { server.restore(); return server.app.close() })

  const r = await request(server.base, "/api/aitimer/doc")
  assert.strictEqual(r.status, 400)
})

test("接口：ai-timer 目录不存在时索引返回 200 且 exists 为 false", async function (t) {
  const server = await startServer(path.join(os.tmpdir(), "aitimer-missing-xyz"))
  t.after(function () { server.restore(); return server.app.close() })

  const r = await request(server.base, "/api/aitimer/index")
  assert.strictEqual(r.status, 200, "目录不存在也不能 500")
  const body = JSON.parse(r.body)
  assert.strictEqual(body.ok, true)
  assert.strictEqual(body.exists, false)
  assert.deepStrictEqual(body.collections, [])
})

test("接口：模块页面壳可访问", async function (t) {
  const server = await startServer(makeFixture())
  t.after(function () { server.restore(); return server.app.close() })

  const r = await request(server.base, "/aitimer/pages/index")
  assert.strictEqual(r.status, 200)
  assert.ok(r.body.indexOf("app-main") >= 0)
})
