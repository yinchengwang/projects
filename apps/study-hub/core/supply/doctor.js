"use strict"
/**
 * doctor.js —— 算法轨批量补题的环境自检（**不联网、不编译、不调用 LLM**）
 *
 * 跑全量之前先过一遍这关：工具链在不在、learning 仓通不通、题库扫得到、
 * 产物目录写得进去、配额设置是什么、密钥配没配（**只报有没有，绝不报内容**）。
 *
 * 每项三态：ok（就绪）/ bad（挡住了）/ warn（能跑但有隐患）。
 * 「密钥没配」是 warn 而不是 bad——离线命令照样能用，只是 generate 跑不了。
 */

const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { spawnSync } = require("node:child_process")

const fill = require("./algorithm-fill.js")
const batch = require("./algorithm-batch.js")

/** 跑一条命令，只取版本行 */
function probe(cmd, args) {
  const res = spawnSync(cmd, args, { encoding: "utf8", windowsHide: true, timeout: 20000 })
  if (res.error || res.status !== 0) {
    return { ok: false, text: String((res.error && res.error.message) || res.stderr || "") }
  }
  const out = String(res.stdout || "").trim()
  return { ok: true, text: out.split("\n")[0] || "" }
}

/** Node 大版本够不够（>= 24） */
function nodeOk() {
  const major = Number(process.versions.node.split(".")[0])
  return { ok: major >= 24, text: "v" + process.versions.node }
}

/**
 * 环境自检。
 * @param {{learningRoot: string|null, unitsDir?: string, localBank?: string|null,
 *          store?: object, env?: object, pythonOverride?: string|null}} params
 * @returns {{ok: boolean, items: Array, notes: string[]}}
 */
function runDoctor(params) {
  const p = params || {}
  const env = p.env || process.env
  const items = []
  const notes = []

  function item(name, status, detail) {
    items.push({ name: name, status: status, detail: detail })
  }

  // —— 1. Node ——
  const node = nodeOk()
  item("Node.js", node.ok ? "ok" : "bad", node.text + (node.ok ? "（≥ 24）" : "（需要 ≥ 24）"))

  // —— 2. g++ ——
  const gpp = probe("g++", ["--version"])
  item(
    "g++ 编译器",
    gpp.ok ? "ok" : "bad",
    gpp.ok ? gpp.text : "找不到 g++，算法轨的真实编译校验跑不了"
  )

  // —— 3. git ——
  const git = probe("git", ["--version"])
  item("git", git.ok ? "ok" : "bad", git.ok ? git.text : "找不到 git，查脏与提交都做不了")

  // —— 4. learning 仓 ——
  const root = String(p.learningRoot || "")
  if (root === "") {
    item("learning 仓", "bad", "没解析到 learning 仓根目录，写回不可用")
  } else if (!fs.existsSync(root)) {
    item("learning 仓", "bad", "目录不存在：" + root)
  } else {
    const isRepo = fs.existsSync(path.join(root, ".git"))
    item("learning 仓", isRepo ? "ok" : "warn", root + (isRepo ? "" : "（不是 git 仓，查脏会失败）"))
  }

  // —— 5. 题库扫得到 ——
  if (root !== "" && fs.existsSync(root)) {
    const scan = fill.scanBank({ learningRoot: root, localBank: p.localBank || null })
    const stats = scan.stats
    item(
      "题库扫描",
      stats.total > 0 ? "ok" : "bad",
      "共 " + stats.total + " 题（A " + stats.byShape.A + " / B " + stats.byShape.B +
        " / C " + stats.byShape.C + " / ready " + stats.byShape.ready + "）"
    )
    if (scan.warnings.length > 0) notes.push("题库扫描有 " + scan.warnings.length + " 条提醒")
  } else {
    item("题库扫描", "bad", "learning 仓不可用，扫不了题库")
  }

  // —— 6. 产物目录可写 ——
  const unitsDir = String(p.unitsDir || (root ? path.join(root, "units", "algorithm") : ""))
  // learning 仓本身不可用时**不要**去 mkdir：那会在不存在的路径上硬造目录，
  // 把「目录不存在」这个事实变成「目录存在」，下一次自检就会误报成能用。
  if (unitsDir === "" || root === "" || !fs.existsSync(root)) {
    item("产物目录", "bad", unitsDir === "" ? "不知道往哪写（learning 仓不可用）" : "learning 仓不可用，写不进去")
  } else {
    try {
      fs.mkdirSync(unitsDir, { recursive: true })
      const probeFile = path.join(unitsDir, ".doctor-write-probe")
      fs.writeFileSync(probeFile, "probe", "utf8")
      fs.rmSync(probeFile, { force: true })
      item("产物目录", "ok", unitsDir + "（可写）")
    } catch (err) {
      item("产物目录", "bad", unitsDir + " 写不进去：" + String((err && err.message) || err))
    }
  }

  // —— 7. 配额设置 ——
  const quot = batch.readLimits ? batch.readLimits(p.store) : null
  if (quot) {
    item(
      "批处理配额",
      "ok",
      "单批 " + quot.sessionLimit + " 次 / 当日 " + quot.dailyLimit +
        " 次 / 间隔 " + quot.rateMs + " 毫秒 / 最多 " + (1 + quot.maxRetries) + " 次尝试"
    )
  } else {
    item("批处理配额", "warn", "没拿到 limits（不影响文件层面的命令）")
  }

  // —— 8. 密钥（只报有没有）——
  const hasArk = Boolean(String(env.ARK_API_KEY || "").trim())
  const hasAnthropic = Boolean(String(env.ANTHROPIC_API_KEY || "").trim())
  const hasDotEnv = Boolean(p.hasDotEnv)
  if (hasArk || hasAnthropic) {
    item("LLM 密钥", "ok", (hasArk ? "ARK_API_KEY 有" : "ANTHROPIC_API_KEY 有") + "（内容不显示）")
  } else {
    item(
      "LLM 密钥",
      "warn",
      "没配 ARK_API_KEY / ANTHROPIC_API_KEY" +
        (hasDotEnv ? "（.env 在，但里面没有可用的密钥）" : "（也没有 .env）") +
        "，generate 跑不了，其余命令照常可用"
    )
  }

  // —— 9. 磁盘余量（粗查）——
  try {
    const stat = fs.statfsSync ? fs.statfsSync(unitsDir || os.tmpdir()) : null
    if (stat) {
      const freeGb = (Number(stat.bfree) * Number(stat.bsize)) / (1024 * 1024 * 1024)
      item("磁盘余量", freeGb > 0.5 ? "ok" : "warn", freeGb.toFixed(1) + " GB 可用")
    }
  } catch (err) {
    // statfs 在某些平台不可用，跳过即可，不当作问题
  }

  const bad = items.filter(function (it) { return it.status === "bad" }).length
  return { ok: bad === 0, items: items, notes: notes }
}

/**
 * 自检结果 → 中文报告。
 * @param {object} result
 * @returns {string}
 */
function renderDoctor(result) {
  const r = result || {}
  const items = Array.isArray(r.items) ? r.items : []
  const lines = []
  lines.push("算法轨环境自检")
  items.forEach(function (it) {
    const mark = it.status === "ok" ? "✅" : it.status === "bad" ? "❌" : "…"
    lines.push("  " + mark + " " + it.name + "：" + it.detail)
  })
  const bad = items.filter(function (it) { return it.status === "bad" }).length
  const warn = items.filter(function (it) { return it.status === "warn" }).length
  lines.push("")
  if (bad === 0) {
    lines.push(
      "环境就绪" + (warn ? "（" + warn + " 项提醒见上）" : "") + "：可以先跑 dry-run，再跑 generate。"
    )
  } else {
    lines.push("有 " + bad + " 项挡住了，先解决再跑 generate。")
  }
  return lines.join("\n")
}

module.exports = {
  runDoctor: runDoctor,
  renderDoctor: renderDoctor,
}
