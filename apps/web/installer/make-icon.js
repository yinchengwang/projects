"use strict"
/**
 * make-icon.js —— 生成 Windows 图标 .ico
 *
 * 做法：借 headless Chrome 把 icon.svg 渲染成各尺寸的 PNG（带透明通道），
 * 再按 ICO 格式打包。ICO 目录项 + PNG 负载是 Vista 之后的通行做法，
 * Windows 11 直接认。
 *
 * 用法：node apps/web/installer/make-icon.js
 */

const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const HERE = path.dirname(__filename)
const SVG = path.join(HERE, "icon.svg")
const OUT = path.join(HERE, "icon.ico")

/** 要打进去的尺寸 */
const SIZES = [16, 24, 32, 48, 64, 128, 256]

/** Chrome / Edge 可执行文件候选路径 */
const BROWSERS = [
  "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
  path.join(os.homedir(), "AppData\\Local\\Google\\Chrome\\Application\\chrome.exe"),
  "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
  "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe"
]

/**
 * 找一个浏览器。
 * @returns {string}
 */
function findBrowser () {
  for (const b of BROWSERS) if (fs.existsSync(b)) return b
  throw new Error("找不到 Chrome/Edge")
}

/**
 * 把 SVG 渲染成某个尺寸的 PNG。
 * @param {string} browser
 * @param {number} size
 * @param {string} outFile
 */
function renderPng (browser, size, outFile) {
  const svg = fs.readFileSync(SVG, "utf8")
  const html = "<!doctype html><meta charset=\"utf-8\">" +
    "<style>html,body{margin:0;padding:0;width:" + size + "px;height:" + size + "px;overflow:hidden;background:transparent}" +
    "svg{display:block;width:" + size + "px;height:" + size + "px}</style>" + svg
  const tmpHtml = outFile.replace(/\.png$/, ".html")
  fs.writeFileSync(tmpHtml, html, "utf8")

  execFileSync(browser, [
    "--headless=new",
    "--disable-gpu",
    "--no-sandbox",
    "--hide-scrollbars",
    "--force-device-scale-factor=1",
    "--default-background-color=00000000",
    "--window-size=" + size + "," + size,
    "--virtual-time-budget=4000",
    "--user-data-dir=" + path.join(os.tmpdir(), "icon-profile-" + size),
    "--screenshot=" + outFile,
    "file:///" + tmpHtml.replace(/\\/g, "/")
  ], { stdio: "ignore", timeout: 60000 })

  fs.rmSync(tmpHtml, { force: true })
  if (!fs.existsSync(outFile)) throw new Error("截图没生成：" + outFile)
}

/**
 * 解 PNG → RGBA 像素。
 *
 * 只支持 Chrome 输出的那一种：8 位、彩色类型 6（RGBA）、非隔行。
 * 够用就行，不做通用 PNG 解码器。
 *
 * @param {Buffer} buf
 * @returns {{width:number, height:number, data:Buffer}} data 是 RGBA
 */
function decodePng (buf) {
  const zlib = require("node:zlib")
  const SIG = [137, 80, 78, 71, 13, 10, 26, 10]
  for (let i = 0; i < 8; i += 1) {
    if (buf[i] !== SIG[i]) throw new Error("不是 PNG")
  }

  let pos = 8
  let width = 0
  let height = 0
  let depth = 0
  let colorType = 0
  const idat = []

  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos)
    const type = buf.toString("ascii", pos + 4, pos + 8)
    const data = buf.slice(pos + 8, pos + 8 + len)
    pos += 12 + len

    if (type === "IHDR") {
      width = data.readUInt32BE(0)
      height = data.readUInt32BE(4)
      depth = data[8]
      colorType = data[9]
      if (data[12] !== 0) throw new Error("不支持隔行 PNG")
    } else if (type === "IDAT") {
      idat.push(data)
    } else if (type === "IEND") {
      break
    }
  }

  if (depth !== 8) throw new Error("只支持 8 位深度的 PNG，实际 " + depth)
  const channels = colorType === 6 ? 4 : colorType === 2 ? 3 : 0
  if (!channels) throw new Error("只支持 RGB / RGBA 的 PNG，实际彩色类型 " + colorType)

  const raw = zlib.inflateSync(Buffer.concat(idat))
  const stride = width * channels
  const out = Buffer.alloc(width * height * 4)
  const line = Buffer.alloc(stride)
  const prev = Buffer.alloc(stride)

  let rp = 0
  for (let y = 0; y < height; y += 1) {
    const filter = raw[rp]
    rp += 1
    raw.copy(line, 0, rp, rp + stride)
    rp += stride

    // 反滤波
    for (let x = 0; x < stride; x += 1) {
      const a = x >= channels ? line[x - channels] : 0
      const b = prev[x]
      const c = x >= channels ? prev[x - channels] : 0
      let v = line[x]
      if (filter === 1) v += a
      else if (filter === 2) v += b
      else if (filter === 3) v += Math.floor((a + b) / 2)
      else if (filter === 4) {
        const p = a + b - c
        const pa = Math.abs(p - a)
        const pb = Math.abs(p - b)
        const pc = Math.abs(p - c)
        v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c)
      }
      line[x] = v & 0xff
    }

    for (let x = 0; x < width; x += 1) {
      const s = x * channels
      const d = (y * width + x) * 4
      out[d] = line[s]
      out[d + 1] = line[s + 1]
      out[d + 2] = line[s + 2]
      out[d + 3] = channels === 4 ? line[s + 3] : 255
    }
    line.copy(prev)
  }

  return { width: width, height: height, data: out }
}

/**
 * RGBA 像素 → ICO 里的 DIB 条目。
 *
 * NSIS 的 Icon / UninstallIcon 命令只认 BMP 格式的 ICO 条目，
 * PNG 压缩的那种它读不了 —— 所以这里转成 32 位 DIB（BITMAPINFOHEADER）。
 *
 * @param {{width:number, height:number, data:Buffer}} img RGBA
 * @returns {Buffer}
 */
function rgbaToDib (img) {
  const w = img.width
  const h = img.height
  const header = Buffer.alloc(40)
  header.writeUInt32LE(40, 0)          // biSize
  header.writeInt32LE(w, 4)            // biWidth
  header.writeInt32LE(h * 2, 8)        // biHeight（XOR 图 + AND 掩码）
  header.writeUInt16LE(1, 12)          // biPlanes
  header.writeUInt16LE(32, 14)         // biBitCount
  header.writeUInt32LE(0, 16)          // biCompression = BI_RGB

  // 像素：自下而上，每像素 BGRA
  const xor = Buffer.alloc(w * h * 4)
  for (let y = 0; y < h; y += 1) {
    const srcRow = (h - 1 - y) * w * 4
    const dstRow = y * w * 4
    for (let x = 0; x < w; x += 1) {
      const s = srcRow + x * 4
      const d = dstRow + x * 4
      xor[d] = img.data[s + 2]         // B
      xor[d + 1] = img.data[s + 1]       // G
      xor[d + 2] = img.data[s]           // R
      xor[d + 3] = img.data[s + 3]       // A
    }
  }

  // AND 掩码：1bpp，每行按 4 字节对齐。alpha 通道已经带了透明度，掩码全 0
  const maskStride = Math.ceil(w / 32) * 4
  const mask = Buffer.alloc(maskStride * h)

  const size = 40 + xor.length + mask.length
  header.writeUInt32LE(size, 20)       // biSizeImage

  return Buffer.concat([header, xor, mask])
}

/**
 * 把若干 PNG 打包成 ICO（条目是 BMP/DIB 格式，NSIS 认）。
 * @param {Array<{size:number, png:Buffer}>} images
 * @returns {Buffer}
 */
function packIco (images) {
  const decoded = images.map(function (img) {
    return { size: img.size, data: rgbaToDib(decodePng(img.png)) }
  })

  const count = decoded.length
  const header = Buffer.alloc(6)
  header.writeUInt16LE(0, 0)   // reserved
  header.writeUInt16LE(1, 2)   // type = icon
  header.writeUInt16LE(count, 4)

  const entries = Buffer.alloc(16 * count)
  let offset = 6 + 16 * count
  decoded.forEach(function (img, i) {
    const e = 16 * i
    entries.writeUInt8(img.size >= 256 ? 0 : img.size, e + 0)   // width（256 记作 0）
    entries.writeUInt8(img.size >= 256 ? 0 : img.size, e + 1)   // height
    entries.writeUInt8(0, e + 2)        // 调色板数
    entries.writeUInt8(0, e + 3)        // reserved
    entries.writeUInt16LE(1, e + 4)     // color planes
    entries.writeUInt16LE(32, e + 6)    // bits per pixel
    entries.writeUInt32LE(img.data.length, e + 8)
    entries.writeUInt32LE(offset, e + 12)
    offset += img.data.length
  })

  return Buffer.concat([header, entries].concat(decoded.map(function (i) { return i.data })))
}

function main () {
  const browser = findBrowser()
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "icon-build-"))
  const images = []
  SIZES.forEach(function (size) {
    const png = path.join(tmp, size + ".png")
    renderPng(browser, size, png)
    images.push({ size: size, png: fs.readFileSync(png) })
    console.log("  " + size + "px ✓")
  })
  fs.writeFileSync(OUT, packIco(images))
  fs.rmSync(tmp, { recursive: true, force: true })
  console.log("图标已生成：" + OUT + "（" + (fs.statSync(OUT).size / 1024).toFixed(1) + " KB）")
}

if (require.main === module) main()

module.exports = { packIco: packIco, SIZES: SIZES, OUT: OUT }
