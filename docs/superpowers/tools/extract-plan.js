"use strict"
/**
 * extract-plan.js —— 从计划文档里提取「新建 `路径`：」后的代码块，落到真实文件
 *
 * 用法：
 *   node docs/superpowers/tools/extract-plan.js <计划文件> [--dry]
 *
 * 规则：
 *   - 匹配形如「新建 `apps/web/xxx.js`：」的行
 *   - 其后第一个开围栏到配对闭围栏之间即文件内容
 *   - 已存在的文件跳过（不覆盖人工写过的）
 *   - 围栏用定长反引号配对，支持 ``` 与 ```` 两种
 */

const fs = require("node:fs")
const path = require("node:path")

const REPO = path.resolve(__dirname, "..", "..", "..")
const MARK = "新建 `"
const RE_TARGET = /^\u65b0\u5efa `(apps\/web\/[^`]+)`/

/**
 * 从一份计划里抽取全部待建文件。
 * @param {string} planPath 相对仓库根的路径
 * @returns {Array<{rel: string, content: string, line: number}>}
 */
function extract (planPath) {
  const abs = path.join(REPO, planPath)
  const text = fs.readFileSync(abs, "utf8")
  const lines = text.split(/\r?\n/)
  const out = []

  for (let i = 0; i < lines.length; i += 1) {
    const m = RE_TARGET.exec(lines[i])
    if (!m) continue
    const rel = m[1]

    // 往下找开围栏；若先遇到下一个「新建 `」，说明这段没代码块
    let open = -1
    let fence = ""
    for (let j = i + 1; j < lines.length; j += 1) {
      const fm = /^(`{3,})/.exec(lines[j])
      if (fm) { open = j; fence = fm[1]; break }
      if (lines[j].startsWith(MARK)) break
    }
    if (open < 0) continue

    let close = -1
    for (let j = open + 1; j < lines.length; j += 1) {
      if (lines[j].trim() === fence) { close = j; break }
    }
    if (close < 0) continue

    out.push({
      rel: rel,
      content: lines.slice(open + 1, close).join("\n") + "\n",
      line: open + 2
    })
    i = close
  }
  return out
}

function main () {
  const args = process.argv.slice(2)
  const dry = args.includes("--dry")
  const plans = args.filter(function (a) { return !a.startsWith("--") })
  if (plans.length === 0) {
    console.error("用法：node docs/superpowers/tools/extract-plan.js <计划文件...> [--dry]")
    process.exit(1)
  }

  let written = 0
  let skipped = 0
  const all = []

  plans.forEach(function (plan) {
    const items = extract(plan)
    console.log("\n▶ " + plan + "（" + items.length + " 个文件）")
    items.forEach(function (it) {
      const abs = path.join(REPO, it.rel)
      all.push(it)
      if (fs.existsSync(abs)) {
        console.log("  - 已存在: " + it.rel)
        skipped += 1
        return
      }
      if (!dry) {
        fs.mkdirSync(path.dirname(abs), { recursive: true })
        fs.writeFileSync(abs, it.content, "utf8")
      }
      console.log("  + " + it.rel + "  (" + it.content.split("\n").length + " 行)")
      written += 1
    })
  })

  console.log("\n" + "=".repeat(56))
  console.log("写入 " + written + " / 跳过 " + skipped + " / 总计 " + all.length)
  if (dry) console.log("（dry run，未落盘）")
}

main()
