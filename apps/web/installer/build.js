"use strict"
/**
 * build.js —— 生成 Windows 安装包
 *
 * 流程：
 *   1. 摆好 staging 目录（node.exe + 应用文件 + ai-timer 数据 + 图标/文案）
 *   2. 调 makensis 编译 installer.nsi
 *   3. 产物落到 build/apps-web/
 *
 * 用法：
 *   node apps/web/installer/build.js
 *   node apps/web/installer/build.js --skip-ai-timer   不带 AI 简报数据
 *
 * 产物不入库（build/ 已在 .gitignore 里）。
 */

const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const HERE = path.dirname(__filename)
const REPO = path.resolve(HERE, "..", "..", "..")
const WEB = path.join(REPO, "apps", "web")
const OUT_DIR = path.join(REPO, "build", "apps-web")
const STAGE = path.join(OUT_DIR, "staging")

const { ensureNsis } = require("./fetch-nsis.js")

const APP_VERSION = "1.0.0"
const APP_NAME = "工作台"

/**
 * 复制一个文件或目录。
 * @param {string} from
 * @param {string} to
 */
function copy (from, to) {
  fs.cpSync(from, to, { recursive: true, force: true })
}

/**
 * 把应用文件摆进 staging。
 */
function stageApp () {
  fs.rmSync(STAGE, { recursive: true, force: true })
  fs.mkdirSync(STAGE, { recursive: true })

  // 应用本体
  const appDir = path.join(STAGE, "app")
  fs.mkdirSync(appDir, { recursive: true })
  ;["server", "shared", "web"].forEach(function (name) {
    copy(path.join(WEB, name), path.join(appDir, name))
  })

  // 启动器与辅助文件
  copy(path.join(HERE, "launch.js"), path.join(STAGE, "launch.js"))
  copy(path.join(HERE, "start.vbs"), path.join(STAGE, "start.vbs"))
  copy(path.join(HERE, "stop.vbs"), path.join(STAGE, "stop.vbs"))
  copy(path.join(HERE, "icon.ico"), path.join(STAGE, "icon.ico"))
  copy(path.join(HERE, "license.txt"), path.join(STAGE, "license.txt"))
  copy(path.join(HERE, "README.txt"), path.join(STAGE, "README.txt"))

  // Node 运行时 —— 装完不依赖系统里有没有装 Node
  const nodeExe = process.execPath
  fs.copyFileSync(nodeExe, path.join(STAGE, "node.exe"))

  console.log("[摆文件] 应用本体 OK")
}

/**
 * 摆 ai-timer 数据。
 * @returns {boolean} 有没有摆上
 */
function stageAiTimer () {
  const src = path.join(REPO, "ai-timer")
  if (!fs.existsSync(src)) {
    console.log("[摆文件] ai-timer/ 不存在，跳过")
    return false
  }
  const dest = path.join(STAGE, "ai-timer")
  fs.mkdirSync(dest, { recursive: true })
  copy(src, dest)
  const n = countFiles(dest)
  console.log("[摆文件] ai-timer/ OK（" + n + " 个文件）")
  return true
}

/**
 * 数目录里的文件数。
 * @param {string} dir
 * @returns {number}
 */
function countFiles (dir) {
  let n = 0
  fs.readdirSync(dir, { withFileTypes: true }).forEach(function (e) {
    if (e.isDirectory()) n += countFiles(path.join(dir, e.name))
    else n += 1
  })
  return n
}

/**
 * 把 UTF-8 文本转成 UTF-16LE（带 BOM）。
 *
 * NSIS 的脚本与 LicenseData / MUI_FINISHPAGE_SHOWREADME 读的文本都按
 * Windows 的 ANSI/UTF-16 处理 —— 中文必须用 UTF-16LE 才不会乱码。
 * 仓库里的源文件保持 UTF-8，编译前才转。
 *
 * @param {string} utf8
 * @returns {Buffer}
 */
function toUtf16le (utf8) {
  return Buffer.concat([
    Buffer.from([0xff, 0xfe]),
    Buffer.from(utf8, "utf16le")
  ])
}

/**
 * 编译安装包。
 * @param {boolean} withAiTimer
 * @returns {string} 产物路径
 */
function compile (withAiTimer) {
  const makensis = ensureNsis()
  const tmpOut = path.join(OUT_DIR, "setup-raw.exe")
  fs.mkdirSync(OUT_DIR, { recursive: true })

  // 脚本与两份文本转成 UTF-16LE
  const nsiUtf16 = path.join(OUT_DIR, "installer-utf16.nsi")
  fs.writeFileSync(nsiUtf16, toUtf16le(fs.readFileSync(path.join(HERE, "installer.nsi"), "utf8")))
  ;["license.txt", "README.txt"].forEach(function (name) {
    const p = path.join(STAGE, name)
    fs.writeFileSync(p, toUtf16le(fs.readFileSync(p, "utf8")))
  })

  const args = [
    "/DSTAGE=" + STAGE,
    "/DOUTFILE=" + tmpOut,
    "/DAPP_VERSION=" + APP_VERSION,
    nsiUtf16
  ]

  console.log("[编译] " + path.basename(makensis))
  execFileSync(makensis, args, { stdio: "inherit", cwd: HERE, timeout: 600000 })

  fs.rmSync(nsiUtf16, { force: true })

  const finalPath = path.join(OUT_DIR, APP_NAME + "-" + APP_VERSION + "-安装包.exe")
  fs.copyFileSync(tmpOut, finalPath)
  fs.rmSync(tmpOut, { force: true })

  if (!withAiTimer) {
    // 没带 ai-timer 的包，元数据里说明一下，免得装完以为坏了
    console.log("[编译] 注意：本次没包含 AI 简报数据")
  }
  return finalPath
}

function main () {
  const withAiTimer = process.argv.indexOf("--skip-ai-timer") < 0

  stageApp()
  if (withAiTimer) stageAiTimer()

  const out = compile(withAiTimer)
  const size = (fs.statSync(out).size / 1024 / 1024).toFixed(1)
  console.log("\n[完成] " + path.relative(REPO, out) + "（" + size + " MB）")
  console.log("[完成] 双击它就能安装")
}

if (require.main === module) main()

module.exports = { stageApp: stageApp, stageAiTimer: stageAiTimer, compile: compile, STAGE: STAGE, OUT_DIR: OUT_DIR }
