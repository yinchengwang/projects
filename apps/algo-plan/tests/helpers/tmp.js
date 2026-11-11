"use strict"
/**
 * tmp.js —— 测试用的临时目录
 *
 * 测试会在系统临时目录里造一堆工作目录，跑完必须删掉，不能让它们一直堆在磁盘上。
 * 这里统一登记、进程退出时统一清一遍——node:test 每个套件是独立进程，
 * 所以各清各的，互不干扰。
 */

const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

/** 本进程造出来的临时目录 */
const created = []

process.on("exit", function () {
  created.forEach(function (dir) {
    try {
      fs.rmSync(dir, { recursive: true, force: true })
    } catch (e) {
      // 删不掉就算了（比如文件还被占用），别让清理失败把测试结果带崩
    }
  })
})

/**
 * 造一个临时目录，进程退出时自动删掉。
 * @param {string} prefix 目录名前缀，形如 "algo-plan-db-"
 * @returns {string} 目录的绝对路径
 */
function tmpDir(prefix) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), prefix))
  created.push(dir)
  return dir
}

module.exports = { tmpDir: tmpDir }
