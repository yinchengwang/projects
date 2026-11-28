"use strict"
/**
 * sync.js —— 把页面清单同步成前端可直接 <script> 引入的静态文件
 *
 * 为什么要同步成静态文件：
 * 预览页可能被 file:// 直接打开（双击打开），那时 fetch 跨域会被浏览器拦掉
 * （Origin 是 null）。而 <script src="..."> 在 file:// 下是允许的。
 *
 * 所以清单以静态文件的形式随应用一起托管：
 *   源   —— server/preview-manifest.js（唯一数据源）
 *   产物 —— web/preview-manifest.js（提交入库，给预览页用）
 *
 * 同步后跑一遍测试，确认两者一致（tests/preview-manifest.test.js）。
 *
 * 用法：node apps/web/preview/sync.js
 */

const fs = require("node:fs")
const path = require("node:path")

const REPO = path.resolve(__dirname, "..", "..", "..")
const SOURCE = path.join(REPO, "apps", "web", "server", "preview-manifest.js")
const TARGET = path.join(REPO, "apps", "web", "web", "preview-manifest.js")

const { buildManifest } = require(SOURCE)

/**
 * 生成静态文件内容。
 * @returns {string}
 */
function render () {
  const json = JSON.stringify(buildManifest(), null, 2)
  return [
    '"use strict"',
    '/**',
    ' * preview-manifest.js —— 页面清单的静态副本',
    ' *',
    ' * 由 apps/web/preview/sync.js 从 server/preview-manifest.js 生成，**不要手改**。',
    ' * 手改会在下次同步时被覆盖 —— 改源文件，然后跑 node apps/web/preview/sync.js。',
    ' *',
    ' * 做成静态文件而不是接口，是为了让预览页在 file:// 下也能用：',
    ' * 那时 fetch 会被 CORS 拦掉，而 <script src> 不会。',
    ' */',
    '',
    'var manifest = ' + json,
    '',
    'if (typeof window !== "undefined") window.__PREVIEW_MANIFEST__ = manifest',
    'if (typeof module === "object" && module.exports) module.exports = manifest',
    ''
  ].join("\n")
}

function main () {
  const content = render()
  fs.writeFileSync(TARGET, content, "utf8")
  const count = buildManifest().reduce(function (n, m) { return n + m.pages.length }, 0)
  console.log("[同步] 已生成 " + path.relative(REPO, TARGET) + "（" + count + " 个页面）")
}

if (require.main === module) main()

module.exports = { render: render, TARGET: TARGET, SOURCE: SOURCE }
