"use strict"
/**
 * paths.js —— learning 仓根路径解析
 *
 * 不硬编码 learning 仓位置。只认环境变量 ALGO_PLAN_LEARNING_ROOT，
 * 取不到就降级为「纯本地模式」：只有 SQLite，不与 learning 仓同步。
 */

const fs = require("node:fs")
const path = require("node:path")

/**
 * 解析 learning 仓根目录。
 * @param {object} env 进程环境变量对象
 * @param {string} [override] 显式指定路径（优先于环境变量，测试用）
 * @returns {{root: string|null, ok: boolean, reason: string}}
 */
function resolveLearningRoot(env, override) {
  const raw =
    override !== undefined && override !== null ? override : (env || process.env).ALGO_PLAN_LEARNING_ROOT

  if (raw === undefined || raw === null || String(raw).trim() === "") {
    return {
      root: null,
      ok: false,
      reason: "未设置 ALGO_PLAN_LEARNING_ROOT，进入纯本地模式（不与 learning 仓同步）",
    }
  }

  const abs = path.resolve(String(raw).trim())
  if (!fs.existsSync(abs)) {
    return { root: null, ok: false, reason: "ALGO_PLAN_LEARNING_ROOT 指向的目录不存在：" + abs }
  }
  if (!fs.statSync(abs).isDirectory()) {
    return { root: null, ok: false, reason: "ALGO_PLAN_LEARNING_ROOT 不是目录：" + abs }
  }
  return { root: abs, ok: true, reason: "" }
}

/**
 * 题库目录：learning 仓下的 bank/。
 * @param {string|null} learningRoot
 * @returns {string|null}
 */
function bankDir(learningRoot) {
  if (!learningRoot) return null
  return path.join(learningRoot, "bank")
}

module.exports = { resolveLearningRoot: resolveLearningRoot, bankDir: bankDir }
