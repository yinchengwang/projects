"use strict"
/**
 * supply-contract.test.js —— 跨计划契约回归闸门（**不联网、不写文件**）
 *
 * P5 依赖接口契约里的若干条款。这些条款一旦被别人改掉，P5 的代码会**安静地**跑偏：
 * 比如 loader C 不再读 `## 题面`，单元的 statement 就空了，但没有任何一行代码会报错。
 * 所以这里把 P5 需要的每一条钉成断言——契约里必须**逐字出现**某句话。
 *
 * 契约文件的位置：从本文件向上最多找 6 层，每层找
 *   docs/superpowers/plans/2026-11-12-study-hub-接口契约.md
 * 也可以用环境变量 STUDY_HUB_CONTRACT_PATH 直接指定（CI / 别的机器上用）。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")

/** 契约文件名（冻结） */
const CONTRACT_NAME = "2026-11-12-study-hub-接口契约.md"

/** 向上最多找几层 */
const MAX_LEVELS = 6

/**
 * 找契约文件。
 * @returns {string} 找不到返回空串
 */
function findContractFile() {
  const fromEnv = process.env.STUDY_HUB_CONTRACT_PATH
  if (fromEnv && fs.existsSync(fromEnv)) return fromEnv
  let dir = __dirname
  for (let level = 0; level < MAX_LEVELS; level++) {
    const candidate = path.join(dir, "docs", "superpowers", "plans", CONTRACT_NAME)
    if (fs.existsSync(candidate)) return candidate
    const parent = path.dirname(dir)
    if (parent === dir) break
    dir = parent
  }
  return ""
}

const CONTRACT_FILE = findContractFile()
const CONTRACT = CONTRACT_FILE ? fs.readFileSync(CONTRACT_FILE, "utf8") : ""

test("找得到接口契约文件", function () {
  assert.ok(CONTRACT_FILE, "找不到接口契约：" + CONTRACT_NAME + "（可用环境变量 STUDY_HUB_CONTRACT_PATH 指定）")
  assert.ok(CONTRACT.length > 2000, "接口契约内容异常短，是不是找错文件了")
})

test("§2 负例不是第五种断言", function () {
  assert.ok(CONTRACT.includes("负例不是第五种断言"), "契约 §2 必须写明「负例不是第五种断言」")
  assert.ok(CONTRACT.includes("Assert"), "契约 §2 必须保留 Assert 模型")
})

test("§3.3 loader C 的算法轨分支：小节名与负例命名约定", function () {
  assert.ok(CONTRACT.includes("## 题面"), "契约必须写明 loader C 读 `## 题面` 小节")
  assert.ok(CONTRACT.includes("## 测试用例"), "契约必须写明 loader C 读 `## 测试用例` 小节")
  assert.ok(CONTRACT.includes("## 参考解法"), "契约必须写明 loader C 读 `## 参考解法` 小节")
  assert.ok(CONTRACT.includes("<stem>.negative.cpp"), "契约必须写明负例文件的命名约定")
})

test("§5 P5 的 settings 键写死", function () {
  const keys = [
    "supply.batch.daily_limit",
    "supply.batch.session_limit",
    "supply.batch.max_retries",
    "supply.batch.rate_ms",
    "supply.batch.calls.daily.",
    "supply.batch.calls.session.",
    "supply.batch.item.algo.",
    "supply.batch.run.",
  ]
  for (const key of keys) {
    assert.ok(CONTRACT.includes(key), "契约 §5 必须写明 settings 键：" + key)
  }
})

test("§10 算法轨 CLI 选项写死", function () {
  const flags = ["--batch", "--resume", "--retry-rejected", "--dry-run", "--limit", "--session"]
  for (const flag of flags) {
    assert.ok(CONTRACT.includes(flag), "契约 §10 必须写明算法轨选项：" + flag)
  }
})

test("§10 算法轨校验判定规则写死", function () {
  assert.ok(CONTRACT.includes("负例必须被拒"), "契约 §10 必须写明「负例必须被拒」")
  assert.ok(CONTRACT.includes("负例竟然全部通过"), "契约 §10 必须写明「负例全过 ⇒ rejected」的判据")
  assert.ok(CONTRACT.includes("负例编译失败"), "契约 §10 必须写明「负例编译失败 ⇒ rejected」的判据")
})

test("§10 负例被拒的 verdict 白名单写死", function () {
  for (const verdict of ["wrong_answer", "runtime_error", "timeout"]) {
    assert.ok(CONTRACT.includes(verdict), "契约必须写明负例被拒时的 verdict：" + verdict)
  }
})

test("§11 P5 拥有自己的 supply 模块与测试文件", function () {
  const names = [
    "core/supply/algorithm-fill.js",
    "core/supply/algorithm-verify.js",
    "core/supply/algorithm-batch.js",
    "core/supply/algorithm-cli.js",
    "tests/supply-algorithm-fill.test.js",
    "tests/supply-algorithm-verify.test.js",
    "tests/supply-algorithm-batch.test.js",
    "tests/supply-algorithm-e2e.test.js",
  ]
  for (const name of names) {
    assert.ok(CONTRACT.includes(name), "契约 §11 必须列出 P5 的文件：" + name)
  }
})

test("§11 存量题库对 P5 是「不动」", function () {
  const lines = CONTRACT.split(/\r?\n/)
  const row = lines.find(function (line) {
    return line.trim().indexOf("| `learning/bank/**`") === 0
  })
  assert.ok(row, "契约 §11 必须有 `learning/bank/**` 那一行")
  const cells = row.split("|").map(function (cell) { return cell.trim() })
  // 列序：cells[1]=文件，cells[2]=P0 …… cells[7]=P5
  assert.equal(cells[7], "**不动**", "契约 §11 里 learning/bank/** 对 P5 必须是「不动」")
})

test("G20 verdict 五类没被扩", function () {
  for (const verdict of ["pass", "wrong_answer", "timeout", "compile_error", "runtime_error"]) {
    assert.ok(CONTRACT.includes(verdict), "契约必须保留 verdict：" + verdict)
  }
})
