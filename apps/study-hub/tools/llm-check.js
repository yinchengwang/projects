"use strict"
/**
 * llm-check.js —— LLM 配置离线自检（**不发任何网络请求，不改任何文件**）
 *
 * 用法：
 *   node tools/llm-check.js
 *
 * 打印三块：
 *   1. 当前 provider、模型名、地址、密钥来源（**只报变量名，绝不报密钥本身**）；
 *   2. 成本护栏的会话上限与每日上限；
 *   3. 五个提示词的纯函数自检：同样输入两次构造必须得到同样的指纹。
 *
 * 退出码：0 表示自检通过；1 表示提示词不是纯函数（构造结果不稳定）。
 * 「LLM 未启用」**不算失败**——那是合法的关闭状态，app 照常可用。
 */

const crypto = require("node:crypto")
const path = require("node:path")

const { loadDotEnv } = require("../server/main.js")
const { createLlmClient, keySourceOf } = require("../core/llm/client.js")
const { buildPrompt, KINDS } = require("../core/llm/prompts.js")
const { limits } = require("../core/llm/guard.js")

/** 只读的假 store：只为拿到配额上限，不写任何东西 */
function fakeStore() {
  return {
    getSetting: function (key, fallback) {
      return fallback === undefined ? null : fallback
    },
    setSetting: function () {},
  }
}

/** 提示词的指纹：同样的输入必须得到同样的指纹 */
function digest(prompt) {
  return crypto
    .createHash("sha1")
    .update(String(prompt.system) + "\n" + String(prompt.user))
    .digest("hex")
    .slice(0, 12)
}

/**
 * 跑一遍自检并打印报告。
 * @returns {boolean} 提示词自检通过返回 true
 */
function check() {
  loadDotEnv(path.join(__dirname, "..", ".env"))
  const client = createLlmClient({ env: process.env })
  const quota = limits(fakeStore())

  console.log("==== LLM 配置自检 ====")
  console.log("provider ：" + client.name)
  console.log("模型     ：" + client.model)
  console.log("地址     ：" + (client.baseUrl || "（未启用）"))
  console.log("密钥来源 ：" + keySourceOf(client.name, process.env))
  if (client.name === "null") {
    console.log("状态     ：未启用 —— " + client.reason)
    console.log("           app 的其余功能（排程、判题、复习、进度）照常可用。")
  }
  console.log("会话上限 ：" + quota.sessionLimit + " 次")
  console.log("每日上限 ：" + quota.dailyLimit + " 次")

  console.log("")
  console.log("==== 提示词自检（纯函数：同样输入两次构造必须一致） ====")
  const context = {
    title: "两数之和",
    difficulty: "easy",
    topics: ["哈希表"],
    statement: "给定数组与目标值，找出和为目标值的两个下标。",
    level: "思路",
    idea: "用哈希表存见过的数",
    track: "interview",
    cases: [{ input: "[2,7], 9", expect: "[0,1]" }],
    solution: "int main(){ return 0; }",
    solutionLang: "cpp",
  }
  let ok = true
  KINDS.forEach(function (kind) {
    const first = buildPrompt(kind, context)
    const second = buildPrompt(kind, context)
    const same = digest(first) === digest(second)
    if (!same) ok = false
    console.log(
      (same ? "[通过] " : "[失败] ")
      + kind + "：system " + String(first.system.length).padStart(4) + " 字"
      + " / user " + String(first.user.length).padStart(4) + " 字"
      + " / 指纹 " + digest(first)
    )
  })
  console.log("")
  console.log(ok ? "提示词自检通过。" : "提示词自检失败：同样的输入构造出了不同的提示词。")
  return ok
}

if (require.main === module) {
  process.exit(check() ? 0 : 1)
}

module.exports = { check: check }
