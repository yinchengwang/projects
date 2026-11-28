"use strict"
/**
 * design-system.test.js —— 设计系统的可测约束
 *
 * 「好看」本身测不了，但好看背后的那些硬指标能测：
 * 对比度够不够、边框是不是真的画得出来、键盘能不能用。
 * 这些一旦回退，下面这些用例就会红。
 *
 * 覆盖范围：web/style.css（共用层）+ 各模块 style.css
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const WEB = path.resolve(__dirname, "..", "web")
const STYLE = path.join(WEB, "style.css")

/** 所有 css 文件（共用层 + 模块层） */
function allCssFiles () {
  const out = [STYLE]
  fs.readdirSync(WEB).forEach(function (dir) {
    const p = path.join(WEB, dir)
    if (!fs.statSync(p).isDirectory()) return
    const f = path.join(p, "style.css")
    if (fs.existsSync(f)) out.push(f)
  })
  return out
}

/** 去掉注释，避免文档里的示例被当成真规则 */
function stripComments (css) {
  return css.replace(/\/\*[\s\S]*?\*\//g, "")
}

/**
 * 取某个选择器块里的变量表。
 * @param {string} css
 * @param {string} sel
 * @returns {object}
 */
function tokens (css, sel) {
  const re = new RegExp(sel.replace(/[:.]/g, "\\$&") + "\\s*\\{([^}]*)\\}", "m")
  const m = re.exec(css)
  const out = {}
  if (!m) return out
  const vr = /--([a-z-]+)\s*:\s*([^;]+);/g
  let v
  while ((v = vr.exec(m[1])) !== null) out[v[1]] = v[2].trim()
  return out
}

/** #RRGGBB → [r,g,b] */
function hex (s) {
  if (typeof s !== "string") return null
  const m = /^#?([0-9a-f]{6})$/i.exec(s.trim())
  if (!m) return null
  const n = parseInt(m[1], 16)
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255]
}

/** WCAG 相对亮度 */
function luminance (rgb) {
  const c = rgb.map(function (v) {
    const s = v / 255
    return s <= 0.03928 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4)
  })
  return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]
}

/** WCAG 对比度 */
function contrast (a, b) {
  const la = luminance(a)
  const lb = luminance(b)
  const hi = Math.max(la, lb)
  const lo = Math.min(la, lb)
  return (hi + 0.05) / (lo + 0.05)
}

const css = stripComments(fs.readFileSync(STYLE, "utf8"))
const rootVars = tokens(css, ":root")
const light = tokens(css, ".theme-light")
const dark = tokens(css, ".theme-dark")

/* ---------------------------------------------------------- 对比度 */

const TEXT_VARS = ["text-primary", "text-secondary", "text-muted"]
const BG_VARS = ["bg-primary", "bg-surface", "bg-subtle"]

;["light", "dark"].forEach(function (mode) {
  const t = mode === "light" ? light : dark
  TEXT_VARS.forEach(function (fgName) {
    BG_VARS.forEach(function (bgName) {
      test(mode + " 主题：" + fgName + " 对 " + bgName + " 过 AA（4.5:1）", function () {
        const fg = hex(t[fgName])
        const bg = hex(t[bgName])
        assert.ok(fg, fgName + " 不是合法颜色：" + t[fgName])
        assert.ok(bg, bgName + " 不是合法颜色：" + t[bgName])
        const r = contrast(fg, bg)
        assert.ok(r >= 4.5,
          fgName + "(" + t[fgName] + ") 对 " + bgName + "(" + t[bgName] + ") 只有 " +
          r.toFixed(2) + ":1，达不到 AA 的 4.5:1")
      })
    })
  })
})

test("品牌色对浅底过 AA（按钮上的白字靠它）", function () {
  const brand = hex(rootVars.brand)
  assert.ok(brand, "--brand 不是合法颜色：" + rootVars.brand)
  ;["bg-primary", "bg-surface"].forEach(function (bgName) {
    const r = contrast(brand, hex(light[bgName]))
    assert.ok(r >= 4.5,
      "--brand(" + rootVars.brand + ") 对 " + bgName + " 只有 " + r.toFixed(2) + ":1")
  })
})

test("暗色主题的品牌色用亮档，不跟浅色主题共用", function () {
  assert.notStrictEqual(dark.brand, rootVars.brand,
    "暗色底上要用亮一档的品牌色，否则深底深字")
  const r = contrast(hex(dark.brand), hex(dark["bg-surface"]))
  assert.ok(r >= 4.5, "暗色品牌色对卡面只有 " + r.toFixed(2) + ":1")
})

/* ---------------------------------------------------------- 边框与可见性 */

test("所有 css 里不再有 0.5px 边框", function () {
  const bad = []
  allCssFiles().forEach(function (f) {
    const body = stripComments(fs.readFileSync(f, "utf8"))
    if (/0\.5px/.test(body)) bad.push(path.relative(WEB, f))
  })
  assert.deepStrictEqual(bad, [],
    "0.5px 在 2x 屏上只有半个物理像素，等于没画边框：" + bad.join(", "))
})

test("键盘焦点可见（全局有 :focus-visible）", function () {
  assert.ok(/:focus-visible/.test(css), "缺少 :focus-visible 规则，键盘用户看不到焦点在哪")
})

test("页面底色与卡面亮度要拉得开，卡片才立得起来", function () {
  const bg = hex(light["bg-primary"])
  const surface = hex(light["bg-surface"])
  assert.notStrictEqual(bg.join(","), surface.join(","), "页面底与卡面不能是同一个颜色")
  const r = contrast(bg, surface)
  assert.ok(r >= 1.08,
    "页面底与卡面对比只有 " + r.toFixed(3) + ":1，卡片会糊在背景里")
})

/* ---------------------------------------------------------- 字号 */

test("正文基准字号不小于 15px", function () {
  const m = /body\s*\{[^}]*font-size:\s*(\d+)px/.exec(css)
  assert.ok(m, "body 上没有 font-size")
  assert.ok(Number(m[1]) >= 15,
    "正文 " + m[1] + "px 偏小 —— 中文长时间阅读 15px 才是舒适下限")
})

test("没有小于 11px 的字号（11px 在高分屏上已经糊了）", function () {
  const bad = []
  allCssFiles().forEach(function (f) {
    const body = stripComments(fs.readFileSync(f, "utf8"))
    const re = /font-size:\s*(\d+(?:\.\d+)?)px/g
    let m
    while ((m = re.exec(body)) !== null) {
      if (Number(m[1]) < 11) bad.push(path.relative(WEB, f) + " → " + m[1] + "px")
    }
  })
  assert.deepStrictEqual(bad, [], "字号过小：" + bad.join(", "))
})

/* ---------------------------------------------------------- 构件齐全 */

test("共享构件都有对应的样式（ui.js 发出来的类不能没接住）", function () {
  const needed = [
    "panel", "hero", "block", "stat-grid", "stat", "pill", "kv", "empty",
    "progress", "page-head", "page-title", "page-sub", "eyebrow",
    "module-grid", "module-card", "app-shell", "app-nav", "nav-item"
  ]
  const missing = needed.filter(function (cls) {
    return !new RegExp("\\." + cls + "(?![a-z-])").test(css)
  })
  assert.deepStrictEqual(missing, [], "共用层里缺样式：" + missing.join(", "))
})

test("statGrid 的 tone 有样式接住（之前是死类）", function () {
  const tones = ["accent", "ok", "warn", "bad"]
  const missing = tones.filter(function (t) {
    return !new RegExp("\\.stat\\.tone-" + t + "\\b").test(css)
  })
  assert.deepStrictEqual(missing, [],
    "ui.js 的 statGrid 会发 tone-accent/ok/warn/bad，没有样式的等于没这个状态：" +
    missing.join(", "))
})

test("可点的东西有交互反馈（悬停有过渡）", function () {
  const transitions = (css.match(/transition/g) || []).length
  assert.ok(transitions >= 5,
    "共用层只有 " + transitions + " 处 transition，界面会显得很死板")
  assert.ok(/button\s*\{[^}]*transition/.test(css.replace(/\n/g, " ")),
    "button 上没有过渡")
})
