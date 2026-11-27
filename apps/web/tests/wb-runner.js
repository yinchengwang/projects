"use strict"
/**
 * wb-runner.js —— 迁入测试的最小运行器
 *
 * 迁入的用例原本是同步的；平台 API 改 Promise 后部分流程（对话框 / 动作菜单 /
 * 捕获面板）需等待微任务完成。这里用一条串行队列把同步与异步用例统一起来，
 * 保证「每个用例前重置存储」的顺序语义不变，同时让断言在真实结果之后执行。
 */

/**
 * 创建运行器。
 * @returns {{test: Function, report: Function}}
 */
function createRunner () {
  const failures = []
  let pass = 0
  let fail = 0
  let queue = Promise.resolve()

  /**
   * 登记一个用例。
   * @param {string} name 用例名
   * @param {Function} fn 用例体，可返回 Promise
   * @param {Function} [before] 每个用例执行前的重置钩子
   */
  function test (name, fn, before) {
    queue = queue.then(function () {
      try {
        if (before) before()
      } catch (e) {
        fail++
        failures.push({ name: name, error: e.message })
        console.log("  \x1b[31mFAIL\x1b[0m " + name + " -> " + e.message)
        return null
      }
      let result
      try {
        result = fn()
      } catch (e) {
        fail++
        failures.push({ name: name, error: e.message })
        console.log("  \x1b[31mFAIL\x1b[0m " + name + " -> " + e.message)
        return null
      }
      if (result && typeof result.then === "function") {
        return result.then(function () {
          pass++
          console.log("  \x1b[32mPASS\x1b[0m " + name)
        }, function (e) {
          fail++
          failures.push({ name: name, error: e.message })
          console.log("  \x1b[31mFAIL\x1b[0m " + name + " -> " + e.message)
        })
      }
      pass++
      console.log("  \x1b[32mPASS\x1b[0m " + name)
      return null
    })
    return queue
  }

  /**
   * 输出结果并按失败数决定退出码。
   * @returns {Promise<void>}
   */
  function report () {
    return queue.then(function () {
      console.log("\n" + "=".repeat(52))
      console.log("通过 " + pass + " 项，失败 " + fail + " 项")
      if (fail > 0) {
        console.log("\n失败明细：")
        failures.forEach(function (f) { console.log("  - " + f.name + ": " + f.error) })
        process.exitCode = 1
        return
      }
      console.log("全部通过")
    })
  }

  return { test: test, report: report }
}

module.exports = { createRunner: createRunner }
