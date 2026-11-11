"use strict"
/**
 * bank-files.js —— 题库目录里已有题目的索引（两个导入器共用）
 *
 * 为什么必须按 id 找、不能只按「自己算出来的那条路径」找：
 * 同一道题在两路来源里算出来的轨道可能不一样。
 * 例：lc-00070（爬楼梯）在目录来源里属于 dp-basic → advanced，
 *     在题解来源里没有专题信息 → 未分类 → fundamentals。
 * 只查自己那条路径会漏判冲突，结果同一个 id 写出两份文件，
 * provider 列表里就重了，GET /api/problem/:id 也会指向不确定的那一份。
 * 契约 §6 的 G19 写的是「同 id 已存在则跳过」，所以按 id 找。
 */

const fs = require("node:fs")
const path = require("node:path")

/**
 * 扫一遍题库目录，返回已有题目的 id → 文件绝对路径。
 * 题库布局是 bank/<track>/<id>.md，这里递归扫，认不准的目录也一并看掉。
 * @param {string} bankDir learning/bank 目录（可以不存在）
 * @returns {Object<string,string>} id → 文件绝对路径
 */
function indexExistingProblems(bankDir) {
  const out = Object.create(null)
  function walk(dir) {
    let entries
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true })
    } catch (e) {
      return
    }
    entries.forEach(function (entry) {
      const abs = path.join(dir, entry.name)
      if (entry.isDirectory()) {
        walk(abs)
        return
      }
      if (!/\.md$/i.test(entry.name)) return
      const id = entry.name.slice(0, -3)
      if (!Object.prototype.hasOwnProperty.call(out, id)) out[id] = abs
    })
  }
  walk(String(bankDir || ""))
  return out
}

module.exports = {
  indexExistingProblems: indexExistingProblems,
}
