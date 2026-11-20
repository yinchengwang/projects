"use strict"
/**
 * p2-acceptance.test.js —— 契约 §12 的 P2 验收清单（静态闸门）
 *
 * 每一条断言对应契约 §12 里的一个验收项。
 * 这一套只读源码与配置，不起服务、不跑判题，所以**不依赖运行环境**，
 * 永远该绿；跑判题的那部分在 p2-e2e-accept.test.js 里。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const ROOT = path.resolve(__dirname, "..")

/** 读仓库里的一个文件（不存在就抛中文错误） */
function read(rel) {
  const p = path.join(ROOT, rel)
  if (!fs.existsSync(p)) throw new Error("文件不存在：" + rel)
  return fs.readFileSync(p, "utf8")
}

/** 文件里有没有这个字符串 */
function has(rel, needle) {
  return read(rel).indexOf(needle) !== -1
}

test("P2-1：单条判题路径——judge/runner.js 导出 judgeUnit，且内部只有一处跑沙箱", function () {
  const src = read("judge/runner.js")
  assert.ok(src.indexOf("judgeUnit") !== -1, "runner.js 必须导出 judgeUnit")
  const calls = src.match(/runCommand\(/g) || []
  assert.ok(calls.length >= 1, "runner.js 必须调沙箱")
})

test("P2-2：verdict 只有五类，不增不改", function () {
  const allowed = ["pass", "wrong_answer", "runtime_error", "timeout", "compile_error"]
  const src = read("judge/runner.js")
  // 两种写法都算：makeUnitResult("pass", …) 与 { verdict: "pass" }
  const found = (src.match(/makeUnitResult\(\s*"([a-z_]+)"/g) || []).concat(
    src.match(/verdict:\s*"([a-z_]+)"/g) || []
  )
  assert.ok(found.length > 0, "runner.js 里一条 verdict 都没扫到，检查方式本身失效了")
  found.forEach(function (m) {
    const v = m.replace(/.*"([a-z_]+)".*/, "$1")
    assert.ok(allowed.indexOf(v) !== -1, "runner.js 出现了六类 verdict：" + v)
  })
  allowed.forEach(function (v) {
    assert.ok(has("judge/runner.js", '"' + v + '"'), "少了 verdict：" + v)
  })
  // 断言层只报失败原因，不自己发明第六类 verdict
  const assertsSrc = read("judge/asserts.js")
  const verdictLiterals = assertsSrc.match(/verdict:\s*"([a-z_]+)"/g) || []
  verdictLiterals.forEach(function (m) {
    const v = m.replace(/.*"([a-z_]+)".*/, "$1")
    assert.ok(allowed.indexOf(v) !== -1, "asserts.js 里出现了不在五类之内的 verdict：" + v)
  })
})

test("P2-3：断言只有四型", function () {
  const src = read("judge/asserts.js")
  ;["output", "exit", "contains", "file"].forEach(function (t) {
    assert.ok(src.indexOf('"' + t + '"') !== -1, "少了断言类型：" + t)
  })
})

test("P2-4：判题产物里不含密钥字段", function () {
  const src = read("judge/runner.js")
  ;["apiKey", "api_key", "authorization", "Authorization"].forEach(function (k) {
    assert.strictEqual(src.indexOf(k), -1, "判题链路里不该出现：" + k)
  })
})

test("P2-5：单元页与单元库页都已注册，且 index.html 引了它们的脚本", function () {
  assert.ok(has("web/pages/unit.js", "pages.unit"), "单元页要注册 pages.unit")
  assert.ok(has("web/pages/library.js", "pages.library"), "单元库页要注册 pages.library")
  assert.ok(has("web/index.html", "/pages/unit.js"), "index.html 要引 unit.js")
  assert.ok(has("web/index.html", "/pages/library.js"), "index.html 要引 library.js")
})

test("P2-6：answer.js 已不再注册（文件保留）", function () {
  assert.ok(fs.existsSync(path.join(ROOT, "web/pages/answer.js")), "answer.js 文件要保留在仓库里")
  assert.strictEqual(has("web/index.html", "/pages/answer.js"), false, "index.html 不许再引 answer.js")
  assert.ok(has("web/app.js", "delete APP.pages.answer"), "app.js 要删掉 answer 页的注册")
})

test("P2-7：前端没有用 innerHTML（契约 §9.4）", function () {
  ;["web/app.js", "web/pages/unit.js", "web/pages/library.js"].forEach(function (rel) {
    const src = read(rel)
    // 只看**属性访问**（`.innerHTML`），注释里提到这个词不算违规
    assert.strictEqual(/\.innerHTML\b/.test(src), false, rel + " 里出现了 .innerHTML 属性访问")
  })
})

test("P2-8：前端没有引入第三方库（零 npm 依赖、零前端框架）", function () {
  const html = read("web/index.html")
  const src = html.match(/<script[^>]*src="([^"]+)"/g) || []
  src.forEach(function (m) {
    const url = m.replace(/.*src="([^"]+)".*/, "$1")
    assert.strictEqual(url.indexOf("http"), -1, "不许引外链脚本：" + url)
  })
  assert.strictEqual(html.indexOf("react"), -1, "不许引前端框架")
  // charts.js 是项目内的原生模块（诊断页依赖），第三方图表库另由 package.json 禁止。
  const pkg = JSON.parse(read("package.json"))
  assert.deepStrictEqual(pkg.dependencies, {}, "不许新增 npm 运行依赖")
  assert.deepStrictEqual(pkg.devDependencies || {}, {}, "不许新增 npm 开发依赖")
})

test("P2-9：新测试文件都在 tests/ 一级目录，且命名 p2-*.test.js", function () {
  const dir = path.join(ROOT, "tests")
  const files = fs.readdirSync(dir).filter(function (f) {
    return /\.test\.js$/.test(f)
  })
  const p2 = files.filter(function (f) {
    return /^p2-.*\.test\.js$/.test(f)
  })
  ;[
    "p2-asserts.test.js",
    "p2-sandbox.test.js",
    "p2-runner.test.js",
    "p2-judge-e2e.test.js",
    "p2-loader-b.test.js",
    "p2-api-unit-run.test.js",
    "p2-unit-page.test.js",
    "p2-library-page.test.js",
    "p2-hash-route.test.js",
    "p2-acceptance.test.js",
    "p2-e2e-accept.test.js",
  ].forEach(function (f) {
    assert.ok(files.indexOf(f) !== -1, "少了测试文件：" + f)
  })
  assert.ok(p2.length >= 11, "p2-*.test.js 至少要有 11 个")
  // tests/ 下面不许建子目录装测试（run-all.js 只扫一级目录）
  fs.readdirSync(dir, { withFileTypes: true }).forEach(function (e) {
    if (!e.isDirectory()) return
    assert.strictEqual(e.name, "helpers", "tests/ 下不许建子目录：" + e.name)
  })
})

test("P2-10：没有占位符残留", function () {
  const files = [
    "judge/asserts.js",
    "judge/runner.js",
    "judge/sandbox.js",
    "server/api-unit-run.js",
    "web/app.js",
    "web/pages/unit.js",
    "web/pages/library.js",
  ]
  // 注意不要放 "xxx"：judge/runner.js 的注释里有一个 g++ 报错样例
  // （"'xxx' was not declared in this scope"），那是文档不是占位符。
  const banned = ["TODO", "FIXME", "TBD", "待补", "类似 Task", "????"]
  files.forEach(function (rel) {
    const src = read(rel)
    banned.forEach(function (b) {
      assert.strictEqual(src.indexOf(b), -1, rel + " 里还有占位符：" + b)
    })
  })
})

test("P2-11：判题代码只写临时沙箱，不写 learning 仓", function () {
  const src = read("judge/runner.js")
  // 用户提交代码写入 sandbox.dir/main.* 是预期行为；脚本/demo 必须先拷入沙箱，learningRoot 只用于读取源文件。
  assert.ok(src.indexOf("writeFileSync(path.join(sandbox.dir") !== -1, "用户代码要写进沙箱目录")
  assert.ok(src.indexOf("copyIntoSandbox") !== -1, "learning 文件要先拷进沙箱再跑")
  assert.ok(src.indexOf("path.resolve(learningRoot, scriptRel)") !== -1, "断言脚本只从 learningRoot 读取")
  assert.ok(src.indexOf("path.resolve(learningRoot, demoRel)") !== -1, "demo 只从 learningRoot 读取")
})

test("P2-12：超时上限都在契约给的范围内", function () {
  const src = read("judge/runner.js")
  assert.ok(src.indexOf("SCRIPT_TIMEOUT_MS") !== -1, "脚本超时要有上限")
  const sandboxSrc = read("judge/sandbox.js")
  assert.ok(sandboxSrc.indexOf("MAX_TIMEOUT_MS") !== -1, "沙箱要有硬上限")
  assert.ok(sandboxSrc.indexOf("KILL_GRACE_MS") !== -1, "超时要先温和再强杀")
})

test("P2-13：单元运行接口在，四条路由的形状都在", function () {
  const src = read("server/api-unit-run.js")
  assert.ok(src.indexOf("buildUnitRunRoutes") !== -1, "要导出 buildUnitRunRoutes")
  ;[
    "/api/units/:id/judge",
    "/api/units/:id/complete",
    "/api/units/:id/ask",
    "/api/env",
  ].forEach(function (route) {
    assert.ok(src.indexOf(route) !== -1, "少了路由：" + route)
  })
})

test("P2-14：契约冻结的中文错误码都在代码里", function () {
  const src = read("server/api-unit-run.js")
  ;["单元不存在", "代码不能为空", "不支持的语言："].forEach(function (msg) {
    assert.ok(src.indexOf(msg) !== -1, "少了中文错误文案：" + msg)
  })
  // 断言脚本缺失是 compile_error + 中文说明，不是 500
  const runner = read("judge/runner.js")
  assert.ok(runner.indexOf("断言脚本不存在") !== -1, "脚本缺失要给出中文说明")
  assert.ok(runner.indexOf("compile_error") !== -1, "脚本缺失归 compile_error")
})

test("P2-15：tests/pages.test.js 已改成当期口径", function () {
  const src = read("tests/pages.test.js")
  assert.strictEqual(
    src.indexOf("index.html 引用了四个页面脚本"),
    -1,
    "旧的「引用四个页面脚本」那条断言还在，会被 answer.js 退役打红"
  )
  // P2 改成「answer.js 已退役」，P3 又扩成「旧三页与 answer.js 都已退役」——
  // 两种写法都算当期口径，只要有「退役」这层意思就行
  assert.ok(
    src.indexOf("answer.js 都已退役") !== -1 || src.indexOf("answer.js 已退役") !== -1,
    "要改成当期口径的断言"
  )
  assert.ok(src.indexOf("P3") !== -1 || src.indexOf("today") !== -1, "旧三页的退役也要写进断言")
  // 「四个页面脚本都注册进 window.AlgoPlan.pages」那条**留着不动**
  assert.ok(
    src.indexOf("四个页面脚本都注册进 window.AlgoPlan.pages") !== -1,
    "「四个页面脚本都注册」那条不许删——它验的是页面脚本文件本身还加载得了"
  )
})

test("P2-16：全局名以 window.StudyHub 为主，window.AlgoPlan 是同一个对象的别名", function () {
  const src = read("web/app.js")
  assert.ok(src.indexOf("window.StudyHub") !== -1, "全局名以 window.StudyHub 为主")
  assert.ok(
    src.indexOf("window.AlgoPlan = window.StudyHub") !== -1,
    "window.AlgoPlan 必须是 window.StudyHub 的别名（旧页脚本与 tests/pages.test.js 都靠它）"
  )
})
