"use strict"
/**
 * assert-scripts.test.js —— 首批 4 张数据库卡的断言脚本
 *
 * 契约 §3.5：脚本落在 learning/units/database/，全部断言通过 → exit 0。
 * 本测试同时做**变异测试**：把 demo 的结论改掉，断言脚本必须失败——
 * 一个「怎么都通过」的断言脚本等于没有断言。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")
const { spawnSync } = require("node:child_process")

const ROOT = path.resolve(__dirname, "..")

/** P0 首批覆盖的 4 张卡，以及各自用来做变异测试的「结论串」。 */
const CARDS = [
  { stem: "mvcc", mutation: "所有 MVCC 演示完成。" },
  { stem: "transaction", mutation: "所有事务演示完成。" },
  { stem: "index_design", mutation: "所有索引演示完成。" },
  { stem: "sql_fundamentals", mutation: "所有演示完成。" },
]

/**
 * 定位 learning 仓。
 * 优先用环境变量；没有就用「apps/study-hub 上三级」的相对位置（开发机上的默认布局）。
 * 两处都找不到就跳过——但**必须给出理由**，不许静默跳过。
 * @returns {string|null}
 */
function resolveLearningRoot() {
  const fromEnv = process.env.STUDY_HUB_LEARNING_ROOT
  if (fromEnv && fs.existsSync(fromEnv)) return path.resolve(fromEnv)
  const fallback = path.resolve(ROOT, "..", "..", "..", "learning")
  if (fs.existsSync(fallback)) return fallback
  return null
}

/**
 * 找一个能用的 Python 命令。
 * @returns {string|null}
 */
function resolvePython() {
  const candidates = ["python3", "python"]
  for (const cmd of candidates) {
    const probe = spawnSync(cmd, ["--version"], { encoding: "utf8" })
    if (!probe.error && probe.status === 0) return cmd
  }
  return null
}

const LEARNING_ROOT = resolveLearningRoot()
const PYTHON = resolvePython()

/** 造一个沙箱：卡目录的 main.py + 对应的断言脚本。 */
function makeSandbox(stem) {
  const dir = fs.mkdtempSync(path.join(require("node:os").tmpdir(), "study-hub-assert-test-"))
  const demoSrc = path.join(LEARNING_ROOT, "scaffold", "grokking", stem, "main.py")
  const scriptSrc = path.join(LEARNING_ROOT, "units", "database", stem + ".assert.py")
  fs.copyFileSync(demoSrc, path.join(dir, "main.py"))
  fs.copyFileSync(scriptSrc, path.join(dir, stem + ".assert.py"))
  return dir
}

/**
 * 把子进程输出解成文本：utf-8 → gbk → latin-1 依次试。
 *
 * 为什么不能写死 utf-8：断言脚本的 print() 走**本机 locale 编码**
 * （Windows 中文环境是 GBK，Linux 是 UTF-8）。这里抓字节再按同一把梯子解，
 * 与 learning/units/database/*.assert.py 里的 decode() 保持一致。
 * @param {Buffer|string|null} raw
 * @returns {string}
 */
function decode(raw) {
  if (!raw) return ""
  const buf = Buffer.isBuffer(raw) ? raw : Buffer.from(String(raw), "utf8")
  for (const enc of ["utf-8", "gbk"]) {
    try {
      return new TextDecoder(enc, { fatal: true }).decode(buf)
    } catch (e) {
      continue
    }
  }
  return buf.toString("latin1")
}

/** 跑沙箱里的断言脚本。 */
function runAssert(dir, stem) {
  return spawnSync(PYTHON, [path.join(dir, stem + ".assert.py")], {
    cwd: dir,
    encoding: "buffer",
    timeout: 240000,
  })
}

test("learning 仓可定位，否则明确跳过", function (t) {
  if (!LEARNING_ROOT) {
    t.skip("找不到 learning 仓：设 STUDY_HUB_LEARNING_ROOT 或在仓库默认布局下运行")
    return
  }
  assert.ok(fs.existsSync(LEARNING_ROOT))
})

test("python 可用，否则明确跳过", function (t) {
  if (!PYTHON) {
    t.skip("找不到 python3 / python，无法实跑断言脚本")
    return
  }
  assert.ok(PYTHON)
})

CARDS.forEach(function (card) {
  test(card.stem + " 的断言脚本存在且 exit 0", function (t) {
    if (!LEARNING_ROOT || !PYTHON) {
      t.skip("缺少 learning 仓或 python，跳过")
      return
    }
    const script = path.join(LEARNING_ROOT, "units", "database", card.stem + ".assert.py")
    assert.ok(fs.existsSync(script), "断言脚本不存在：" + script)

    const dir = makeSandbox(card.stem)
    try {
      const res = runAssert(dir, card.stem)
      assert.equal(res.status, 0, "断言脚本失败：" + decode(res.stdout) + decode(res.stderr))
      assert.ok(
        decode(res.stdout).indexOf("全部断言通过") !== -1,
        "没有打印通过信息：" + decode(res.stdout)
      )
    } finally {
      fs.rmSync(dir, { recursive: true, force: true })
    }
  })

  test(card.stem + " 的断言脚本回显 demo 输出（契约 §3.5 的两行定界）", function (t) {
    if (!LEARNING_ROOT || !PYTHON) {
      t.skip("缺少 learning 仓或 python，跳过")
      return
    }
    const dir = makeSandbox(card.stem)
    try {
      const res = runAssert(dir, card.stem)
      const out = decode(res.stdout)
      assert.equal(res.status, 0, "断言脚本失败：" + out + decode(res.stderr))

      // 单元页右栏靠这两行定界把 demo 输出切出来显示——没有它就只剩断言结论
      const START = "─── demo 输出开始 ───"
      const END = "─── demo 输出结束 ───"
      const i = out.indexOf(START)
      const j = out.indexOf(END)
      assert.ok(i !== -1 && j !== -1 && j > i, "缺少 demo 回显定界：\n" + out)

      // 定界之间必须真有 demo 的内容（不能是空的，否则回显等于没做）
      const body = out.slice(i + START.length, j).trim()
      assert.ok(body.length > 50, "回显区内容过短（" + body.length + " 字符），demo 输出没被回显")

      // 回显的是 demo 自己的输出：它里面的第一个 CHECKS 结论串必须出现在回显区
      assert.ok(
        body.indexOf("[[OK]]") !== -1 || body.indexOf("═══") !== -1,
        "回显区里看不到 demo 的输出痕迹：\n" + body.slice(0, 200)
      )
    } finally {
      fs.rmSync(dir, { recursive: true, force: true })
    }
  })

  test(card.stem + " 的断言脚本能抓住被改坏的 demo（变异测试）", function (t) {
    if (!LEARNING_ROOT || !PYTHON) {
      t.skip("缺少 learning 仓或 python，跳过")
      return
    }
    const dir = makeSandbox(card.stem)
    try {
      // 把 demo 的结论改掉：断言脚本必须因此失败
      const demoPath = path.join(dir, "main.py")
      const src = fs.readFileSync(demoPath, "utf8")
      assert.ok(src.indexOf(card.mutation) !== -1, "demo 里找不到要改坏的结论：" + card.mutation)
      fs.writeFileSync(demoPath, src.split(card.mutation).join("演示结束。"), "utf8")

      const res = runAssert(dir, card.stem)
      assert.notEqual(res.status, 0, "demo 已被改坏，断言脚本却还是通过了")
      assert.ok(
        decode(res.stdout).indexOf("断言失败") !== -1,
        "断言失败时应当打印中文原因：" + decode(res.stdout)
      )
    } finally {
      fs.rmSync(dir, { recursive: true, force: true })
    }
  })
})

test("跑完之后 learning 仓里没有多出 .db 文件", function (t) {
  if (!LEARNING_ROOT) {
    t.skip("找不到 learning 仓，跳过")
    return
  }
  const found = []
  const walk = function (dir, depth) {
    if (depth > 4) return
    let names = []
    try {
      names = fs.readdirSync(dir)
    } catch (e) {
      return
    }
    names.forEach(function (name) {
      const abs = path.join(dir, name)
      let stat = null
      try {
        stat = fs.statSync(abs)
      } catch (e) {
        return
      }
      if (stat.isDirectory()) {
        walk(abs, depth + 1)
      } else if (/\.db($|-)/.test(name)) {
        found.push(abs)
      }
    })
  }
  walk(path.join(LEARNING_ROOT, "scaffold", "grokking"), 0)
  assert.deepEqual(found, [], "learning 仓里出现了 .db 残留：" + found.join(", "))
})
