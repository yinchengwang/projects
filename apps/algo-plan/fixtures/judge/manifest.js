"use strict"
/**
 * manifest.js —— 判题 fixture 的路径表
 *
 * fixture 分两类：
 *   problems/ —— 题面 + 用例 + 参考解法（题库 markdown 格式，可直接导入 learning/bank）
 *   code/     —— 「用户的提交物」：正例与三类负例（编译错误 / 超时 / 运行时异常）
 */

const path = require("node:path")

const ROOT = __dirname

module.exports = {
  ROOT: ROOT,
  PROBLEMS: {
    cppSum: path.join(ROOT, "problems", "interview", "self-00010.md"),
    pythonReverse: path.join(ROOT, "problems", "fundamentals", "self-00011.md"),
  },
  CODE: {
    cppOk: path.join(ROOT, "code", "cpp-ok.cpp"),
    cppWrongAnswer: path.join(ROOT, "code", "cpp-wrong-answer.cpp"),
    cppCompileError: path.join(ROOT, "code", "cpp-compile-error.cpp"),
    cppTimeout: path.join(ROOT, "code", "cpp-timeout.cpp"),
    cppRuntimeError: path.join(ROOT, "code", "cpp-runtime-error.cpp"),
    pythonOk: path.join(ROOT, "code", "python-ok.py"),
    pythonWrongAnswer: path.join(ROOT, "code", "python-wrong-answer.py"),
    pythonRuntimeError: path.join(ROOT, "code", "python-runtime-error.py"),
  },
}
