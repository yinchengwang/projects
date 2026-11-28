"use strict"
/**
 * generate.js —— 生成整个 app 前端的可点击 HTML 预览
 *
 * 做法：
 * 1. 起一个真实的服务进程（apps/web/server/main.js），这样预览的是真数据；
 * 2. 用 headless Chrome 逐页截图（--screenshot），页面里的 JS 会真跑一遍；
 * 3. 生成 index.html —— 按模块分组的缩略图画廊，点缩略图打开大图。
 *
 * detail 类页面（笔记/片段/题库/足迹详情）需要真实 id。
 * seed 数据的 id 是随机生成的（newId()），没法预先猜，
 * 所以做法是：先用一个「探针页」把 seed 数据写进 localStorage，
 * 再把 localStorage 内容导出，据此拼出各详情页的真实 id。
 *
 * 用法：
 *   node apps/web/preview/generate.js
 *
 * 产物：apps/web/preview/out/index.html + apps/web/preview/out/shots/*.png
 */

const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const http = require("node:http")
const { spawn, execFileSync } = require("node:child_process")

const REPO = path.resolve(__dirname, "..", "..", "..")
const OUT = path.join(__dirname, "out")
const SHOTS = path.join(OUT, "shots")

/** Chrome / Edge 可执行文件候选路径 */
const CHROME_CANDIDATES = [
  "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
  path.join(os.homedir(), "AppData\\Local\\Google\\Chrome\\Application\\chrome.exe"),
  "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
  "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe"
]

/** 视口尺寸（宽 × 高） */
const VIEWPORT = { width: 1280, height: 900 }

/** 预览用的 Chrome 用户目录（复用，保证 localStorage 在各页之间共享） */
const PROFILE = path.join(os.tmpdir(), "apps-web-preview-profile")

/**
 * 找到浏览器可执行文件。
 * @returns {string}
 */
function findBrowser () {
  for (const c of CHROME_CANDIDATES) {
    if (fs.existsSync(c)) return c
  }
  throw new Error("找不到 Chrome/Edge 可执行文件")
}

/**
 * 取某个页面的 DOM（headless Chrome --dump-dom）。
 * @param {string} browser
 * @param {string} url
 * @returns {string} HTML
 */
function dumpDom (browser, url) {
  return execFileSync(browser, [
    "--headless=new",
    "--disable-gpu",
    "--no-sandbox",
    "--virtual-time-budget=9000",
    "--user-data-dir=" + PROFILE,
    "--dump-dom",
    url
  ], { encoding: "utf8", timeout: 60000, stdio: ["ignore", "pipe", "ignore"] })
}

/**
 * 截一张图。
 * @param {string} browser
 * @param {string} url
 * @param {string} file
 */
function capture (browser, url, file) {
  execFileSync(browser, [
    "--headless=new",
    "--disable-gpu",
    "--no-sandbox",
    "--hide-scrollbars",
    "--force-device-scale-factor=1",
    "--window-size=" + VIEWPORT.width + "," + VIEWPORT.height,
    "--virtual-time-budget=9000",
    "--user-data-dir=" + PROFILE,
    "--screenshot=" + file,
    url
  ], { stdio: "ignore", timeout: 60000 })
}

/**
 * 等端口可连。
 * @param {number} port
 * @param {number} timeoutMs
 * @returns {Promise<void>}
 */
function waitForPort (port, timeoutMs) {
  const deadline = Date.now() + (timeoutMs || 20000)
  return new Promise(function (resolve, reject) {
    const attempt = function () {
      const req = http.request(
        { host: "127.0.0.1", port: port, path: "/api/health", method: "GET", timeout: 1000 },
        function (res) { res.resume(); resolve() }
      )
      req.on("error", function () {
        if (Date.now() > deadline) reject(new Error("服务启动超时"))
        else setTimeout(attempt, 250)
      })
      req.on("timeout", function () { req.destroy() })
      req.end()
    }
    attempt()
  })
}

/**
 * 从 seed 源码里取出示例数据的真实 id。
 *
 * 做法：在 Node 里直接 require 应用的 seed 模块，跑一遍它的 seed 函数，
 * 这样拿到的就是浏览器里会写进 localStorage 的同一批 id（id.genId() 是随机，
 * 但只要在同一个 Chrome profile 里重新生成也拿不到 —— 所以反过来做：
 * 让浏览器先种好，再把 localStorage 导出来）。
 *
 * 实际采用的办法：跑一个探针页，用 Chrome 的 --dump-dom 把 localStorage
 * 序列化出来放进 DOM，再从 DOM 里解析。
 *
 * @param {string} browser
 * @param {string} base 服务地址
 * @returns {object} { techNotes, snippets, interview, trip }
 */
function discoverIds (browser, base) {
  const probe = path.join(OUT, "probe.html")
  fs.writeFileSync(probe, [
    "<!doctype html><html><head><meta charset=\"utf-8\"></head><body>",
    "<pre id=\"out\">pending</pre>",
    "<script>",
    // 借一个真实页面把 seed 数据种进 localStorage：workbench 的列表页会读 store，
    // 读不到就落 seed。直接调 seed 模块更直接 —— 这里用同步 XHR 拿脚本再执行。
    "var keys = ['tech_notes','snippets','interview','todos','reading','fitness','mood','schedule','projects','resources','insights'];",
    "function collect() {",
    "  var out = {};",
    "  keys.forEach(function (k) {",
    "    var raw = null;",
    "    try { raw = localStorage.getItem('db:' + k); } catch (e) {}",
    "    if (!raw) return;",
    "    try { var arr = JSON.parse(raw); if (arr && arr.length) out[k] = arr.map(function (x) { return x.id; }); } catch (e) {}",
    "  });",
    "  document.getElementById('out').textContent = JSON.stringify(out);",
    "}",
    "collect();",
    "window.addEventListener('load', function () { setTimeout(collect, 1200); });",
    "setTimeout(collect, 800);",
    "</script></body></html>"
  ].join("\n"), "utf8")

  // 探针页要同源才能读 localStorage —— 放到服务根下
  const served = path.join(REPO, "apps", "web", "web", "__preview_probe.html")
  fs.writeFileSync(served, fs.readFileSync(probe, "utf8"), "utf8")
  try {
    const dom = dumpDom(browser, base + "/__preview_probe.html")
    const m = /<pre id="out">([\s\S]*?)<\/pre>/.exec(dom)
    if (!m) return {}
    const text = m[1].replace(/&quot;/g, '"').replace(/&amp;/g, "&").replace(/&lt;/g, "<").replace(/&gt;/g, ">")
    return JSON.parse(text || "{}")
  } finally {
    fs.rmSync(served, { force: true })
    fs.rmSync(probe, { force: true })
  }
}

/**
 * 全部页面清单。
 *
 * note 字段：页面上已知的既有问题（不是本次预览引入的），
 * 会在预览页上标出来，避免看图的人误以为是预览工具的问题。
 *
 * @param {object} ids 探针拿到的真实 id
 * @returns {Array}
 */
function pageList (ids) {
  const w = "workbench"
  const k = "knowledge"
  const noteId = (ids.tech_notes && ids.tech_notes[0]) || ""
  const snipId = (ids.snippets && ids.snippets[0]) || ""
  const qId = (ids.interview && ids.interview[0]) || ""

  // 三个 detail 页的 render 是从「健身」页复制来的，读的是永远为空的
  // state.items，且空态文案写着「暂无健身」。这是 2026-11 的 web 迁移
  // （commit 7e4cd4239）带进来的既有问题，不在本次预览的修改范围内。
  const DETAIL_BUG = "该页 render 沿用了「健身」页的实现（读空的 state.items、空态文案写错），属既有缺陷"

  return [
    { module: "外壳", title: "模块入口首页", path: "/" },

    { module: "程序员工作台", title: "今日面板", path: "/" + w + "/pages/dashboard/index" },
    { module: "程序员工作台", title: "个人档案", path: "/" + w + "/pages/profile/index" },
    { module: "程序员工作台", title: "技术工作台", path: "/" + w + "/pages/tech/index" },
    { module: "程序员工作台", title: "技术笔记 · 列表", path: "/" + w + "/pages/tech/notes/index" },
    { module: "程序员工作台", title: "技术笔记 · 详情", path: "/" + w + "/pages/tech/notes/detail", query: "id=" + noteId, note: DETAIL_BUG },
    { module: "程序员工作台", title: "代码片段 · 列表", path: "/" + w + "/pages/tech/snippets/index" },
    { module: "程序员工作台", title: "代码片段 · 详情", path: "/" + w + "/pages/tech/snippets/detail", query: "id=" + snipId, note: DETAIL_BUG },
    { module: "程序员工作台", title: "面试题库 · 列表", path: "/" + w + "/pages/tech/interview/index" },
    { module: "程序员工作台", title: "面试题库 · 详情", path: "/" + w + "/pages/tech/interview/detail", query: "id=" + qId, note: DETAIL_BUG },
    { module: "程序员工作台", title: "资源库", path: "/" + w + "/pages/tech/resources/index" },
    { module: "程序员工作台", title: "生活总览", path: "/" + w + "/pages/life/index" },
    { module: "程序员工作台", title: "读书清单", path: "/" + w + "/pages/life/reading/index" },
    { module: "程序员工作台", title: "健身打卡", path: "/" + w + "/pages/life/fitness/index" },
    { module: "程序员工作台", title: "心情记录", path: "/" + w + "/pages/life/mood/index" },
    { module: "程序员工作台", title: "日程安排", path: "/" + w + "/pages/life/schedule/index" },
    { module: "程序员工作台", title: "项目集", path: "/" + w + "/pages/projects/index" },
    { module: "程序员工作台", title: "数据洞察", path: "/" + w + "/pages/insights/index" },
    { module: "程序员工作台", title: "离线工具箱", path: "/" + w + "/pages/tools/index" },

    { module: "知识中心", title: "学习仪表盘", path: "/" + k + "/pages/dashboard/index" },
    { module: "知识中心", title: "学习路径", path: "/" + k + "/pages/learn/index" },
    { module: "知识中心", title: "题库练习", path: "/" + k + "/pages/learn/quiz/index" },
    { module: "知识中心", title: "间隔复习", path: "/" + k + "/pages/learn/review/index" },
    { module: "知识中心", title: "图解系列", path: "/" + k + "/pages/learn/deep/index" },
    { module: "知识中心", title: "五年计划路径", path: "/" + k + "/pages/learn/path/index" },
    { module: "知识中心", title: "面试总览", path: "/" + k + "/pages/interview/index" },
    { module: "知识中心", title: "面试追踪", path: "/" + k + "/pages/interview/tracker/index" },
    { module: "知识中心", title: "面试题库", path: "/" + k + "/pages/interview/bank/index" },
    { module: "知识中心", title: "模拟面试", path: "/" + k + "/pages/interview/mock/index" },
    { module: "知识中心", title: "知识图谱", path: "/" + k + "/pages/knowledge/index" },
    { module: "知识中心", title: "图谱可视化", path: "/" + k + "/pages/knowledge/graph/index" },
    { module: "知识中心", title: "论文精读", path: "/" + k + "/pages/knowledge/excerpt/index" },
    { module: "知识中心", title: "技术雷达", path: "/" + k + "/pages/knowledge/radar/index" },
    { module: "知识中心", title: "个人档案", path: "/" + k + "/pages/profile/index" },
    { module: "知识中心", title: "五年计划", path: "/" + k + "/pages/plan/five-year/index" },
    { module: "知识中心", title: "能力路线图", path: "/" + k + "/pages/plan/roadmap/index" },

    { module: "AI 简报", title: "产物概览", path: "/aitimer/pages/index" },
    { module: "AI 简报", title: "领域系列 · 数据库", path: "/aitimer/pages/series", query: "id=db" },
    { module: "AI 简报", title: "文档阅读 · 每日简报", path: "/aitimer/pages/doc", query: "id=briefing/2026-11-28" },

    { module: "足迹", title: "全部记录", path: "/trip/pages/index" },
    { module: "足迹", title: "新增记录", path: "/trip/pages/new-trip" },
    { module: "足迹", title: "记录详情", path: "/trip/pages/detail", query: "id=trip-1", note: "足迹数据由用户自己创建，没有示例数据，故详情页显示「记录不存在」" },

    { module: "游戏", title: "游戏大厅", path: "/games/pages/index" },
    { module: "游戏", title: "贪吃蛇", path: "/games/pages/snake" },
    { module: "游戏", title: "2048", path: "/games/pages/game2048" },
    { module: "游戏", title: "数独", path: "/games/pages/sudoku" },
    { module: "游戏", title: "消消乐", path: "/games/pages/match3" }
  ]
}

/**
 * 生成预览页 HTML。
 * @param {Array} items 已截图的条目
 * @returns {string} HTML
 */
function buildHtml (items) {
  const modules = []
  const byModule = {}
  items.forEach(function (it) {
    if (!byModule[it.module]) {
      byModule[it.module] = []
      modules.push(it.module)
    }
    byModule[it.module].push(it)
  })

  const sections = modules.map(function (name, mi) {
    const cards = byModule[name].map(function (it) {
      const badge = it.note
        ? '        <span class="flag" title="' + it.note.replace(/"/g, "&quot;") + '">⚠ 已知问题</span>\n'
        : ""
      return [
        '      <a class="card" href="shots/' + it.file + '" target="_blank" title="新标签页打开大图">',
        '        <img src="shots/' + it.file + '" alt="' + it.title + '" loading="lazy">',
        '        <div class="meta"><span class="t">' + it.title + '</span>',
        '        <span class="p">' + it.path + (it.query ? "?" + it.query : "") + '</span>',
        badge,
        '        </div>',
        '      </a>'
      ].filter(Boolean).join("\n")
    }).join("\n")

    return [
      '  <section id="m' + mi + '">',
      '    <h2>' + name + ' <span class="cnt">' + byModule[name].length + ' 页</span></h2>',
      '    <div class="grid">',
      cards,
      '    </div>',
      '  </section>'
    ].join("\n")
  }).join("\n")

  const nav = modules.map(function (name, mi) {
    return '<a href="#m' + mi + '">' + name + '</a>'
  }).join("")

  return [
    '<!doctype html>',
    '<html lang="zh-CN"><head>',
    '<meta charset="utf-8">',
    '<meta name="viewport" content="width=device-width, initial-scale=1">',
    '<title>合并应用 · 前端页面预览</title>',
    '<style>',
    ':root{--bg:#0f1115;--panel:#171a21;--line:#262b36;--fg:#e6e9ef;--dim:#9aa4b2;--acc:#4f8cff}',
    '*{box-sizing:border-box}',
    'body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.6 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif}',
    'header{position:sticky;top:0;z-index:10;background:rgba(15,17,21,.92);backdrop-filter:blur(8px);border-bottom:1px solid var(--line);padding:14px 22px}',
    'h1{margin:0 0 10px;font-size:18px}',
    '.nav{display:flex;flex-wrap:wrap;gap:7px}',
    '.nav a{color:var(--dim);text-decoration:none;border:1px solid var(--line);border-radius:999px;padding:3px 11px;font-size:12px}',
    '.nav a:hover{color:var(--fg);border-color:var(--acc)}',
    'main{padding:22px;max-width:1500px;margin:0 auto}',
    'h2{font-size:15px;margin:26px 0 12px;padding-bottom:7px;border-bottom:1px solid var(--line)}',
    'h2 .cnt{color:var(--dim);font-weight:400;font-size:12px;margin-left:7px}',
    '.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(310px,1fr));gap:14px}',
    '.card{display:block;background:var(--panel);border:1px solid var(--line);border-radius:10px;overflow:hidden;text-decoration:none;color:inherit;transition:.14s}',
    '.card:hover{border-color:var(--acc);transform:translateY(-2px)}',
    '.card img{width:100%;display:block;background:#fff;border-bottom:1px solid var(--line)}',
    '.meta{padding:9px 12px;display:flex;flex-direction:column;gap:2px}',
    '.meta .t{font-size:13px;font-weight:600}',
    '.meta .p{font-size:11px;color:var(--dim);font-family:ui-monospace,Menlo,Consolas,monospace;word-break:break-all}',
    '.flag{display:inline-block;margin-top:5px;font-size:10px;color:#ffb454;background:rgba(255,180,84,.12);border:1px solid rgba(255,180,84,.3);border-radius:4px;padding:1px 6px}',
    'footer{padding:26px 22px;color:var(--dim);font-size:12px;text-align:center;border-top:1px solid var(--line);margin-top:26px}',
    '</style></head><body>',
    '<header>',
    '  <h1>合并应用 · 前端页面预览 <span style="color:var(--dim);font-weight:400;font-size:13px">共 ' + items.length + ' 页</span></h1>',
    '  <div class="nav">' + nav + '</div>',
    '</header>',
    '<main>',
    sections,
    '</main>',
    '<footer>由 apps/web/preview/generate.js 用 headless Chrome 实拍生成 · 数据来自本机真实 ai-timer/ 与内置种子数据</footer>',
    '</body></html>'
  ].join("\n")
}

async function main () {
  const browser = findBrowser()
  fs.rmSync(OUT, { recursive: true, force: true })
  fs.mkdirSync(SHOTS, { recursive: true })
  // 清掉旧 profile —— localStorage 要重新种，id 也随之重新生成
  fs.rmSync(PROFILE, { recursive: true, force: true })
  fs.mkdirSync(PROFILE, { recursive: true })

  const port = 5199
  console.log("[预览] 启动服务，端口 " + port)
  const server = spawn(process.execPath, [path.join(REPO, "apps", "web", "server", "main.js")], {
    cwd: REPO,
    env: Object.assign({}, process.env, { PORT: String(port) }),
    stdio: "ignore"
  })

  const items = []
  try {
    await waitForPort(port, 20000)
    const base = "http://127.0.0.1:" + port

    // 先逛一遍列表页 —— 它们会把 seed 数据写进 localStorage
    console.log("[预览] 预热 seed 数据")
    dumpDom(browser, base + "/workbench/pages/tech/notes/index")
    dumpDom(browser, base + "/workbench/pages/tech/snippets/index")
    dumpDom(browser, base + "/workbench/pages/tech/interview/index")
    dumpDom(browser, base + "/trip/pages/index")

    const ids = discoverIds(browser, base)
    console.log("[预览] 探到的 id：" + JSON.stringify(ids).slice(0, 300))

    const list = pageList(ids)
    console.log("[预览] 开始截图，共 " + list.length + " 页")
    let i = 0
    for (const p of list) {
      i += 1
      const slug = String(i).padStart(2, "0") + "-" +
        p.module.replace(/[\\/:*?"<>|]/g, "") + "-" +
        p.title.replace(/[\\/:*?"<>|]/g, "")
      const file = slug + ".png"
      const url = base + p.path + (p.query ? "?" + p.query : "")
      try {
        capture(browser, url, path.join(SHOTS, file))
        items.push({
          module: p.module,
          title: p.title,
          path: p.path,
          query: p.query || "",
          note: p.note || "",
          file: file
        })
        console.log("  [" + i + "/" + list.length + "] " + p.module + " / " + p.title)
      } catch (e) {
        console.error("  [" + i + "/" + list.length + "] 失败 " + p.title + "：" + e.message)
      }
    }
  } finally {
    server.kill()
  }

  fs.writeFileSync(path.join(OUT, "index.html"), buildHtml(items), "utf8")
  console.log("\n[预览] 完成：" + items.length + " 张截图")
  console.log("[预览] 打开：" + path.join(OUT, "index.html"))
}

main().catch(function (err) {
  console.error("[预览] 失败：", err)
  process.exit(1)
})
