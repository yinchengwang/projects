"use strict"
/**
 * fetch-nsis.js —— 拉取 NSIS 编译工具链
 *
 * 打包需要 makensis.exe。NSIS 官方没有 npm 包，但 electron-builder 把
 * 预编译的二进制托管在 GitHub 上，直接下 .7z 就行。
 *
 * 流程：下载 nsis.7z → 用独立版 7zr.exe 解开 → 落到 .toolchain/nsis/
 * 产物不入库（.gitignore 已排除 .toolchain/）。
 *
 * 用法：node apps/web/installer/fetch-nsis.js
 */

const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const HERE = path.dirname(__filename)
const TOOLCHAIN = path.join(HERE, ".toolchain")
const NSIS_DIR = path.join(TOOLCHAIN, "nsis")
const SEVENZIP = path.join(TOOLCHAIN, "7zr.exe")

/** electron-builder 托管的 NSIS 预编译包 */
const NSIS_URL = "https://github.com/electron-userland/electron-builder-binaries/releases/download/nsis-3.0.4.2/nsis-3.0.4.2.7z"
/** 独立版 7z 解压器（只用来解 .7z） */
const SEVENZIP_URL = "https://www.7-zip.org/a/7zr.exe"

/**
 * 下载一个文件。
 * @param {string} url
 * @param {string} target
 */
function download (url, target) {
  console.log("[取工具] 下载 " + url.split("/").pop())
  execFileSync("powershell", [
    "-NoProfile", "-Command",
    "$ProgressPreference='SilentlyContinue';" +
    "Invoke-WebRequest -Uri '" + url + "' -OutFile '" + target + "' -UseBasicParsing -TimeoutSec 300"
  ], { stdio: "inherit", timeout: 330000 })
}

/**
 * 已经装好就直接返回 makensis 路径。
 * @returns {string} makensis.exe 路径
 */
function ensureNsis () {
  const local = path.join(NSIS_DIR, "makensis.exe")
  if (fs.existsSync(local)) return local

  fs.mkdirSync(TOOLCHAIN, { recursive: true })

  if (!fs.existsSync(SEVENZIP)) download(SEVENZIP_URL, SEVENZIP)

  const archive = path.join(os.tmpdir(), "nsis-toolchain.7z")
  download(NSIS_URL, archive)

  fs.rmSync(NSIS_DIR, { recursive: true, force: true })
  execFileSync(SEVENZIP, ["x", archive, "-o" + NSIS_DIR, "-y"], { stdio: "inherit", timeout: 180000 })
  fs.rmSync(archive, { force: true })

  if (!fs.existsSync(local)) throw new Error("解包后没找到 makensis.exe：" + local)
  return local
}

if (require.main === module) {
  const p = ensureNsis()
  console.log("[取工具] 就绪：" + p)
}

module.exports = { ensureNsis: ensureNsis, NSIS_DIR: NSIS_DIR, TOOLCHAIN: TOOLCHAIN }
