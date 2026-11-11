"use strict"
/**
 * runner.js —— 判题编排器：写文件 → 编译 → 逐用例执行 → 比对 → verdict
 *
 * 判题契约（硬约束，见设计文档 §5.1 与接口契约 G18）：
 *   提交的是「stdin 进、stdout 出」的裸程序，用户自己写 main()。
 *   本文件绝不注入 Solution 包装、绝不自动生成 main。
 *   存量 LeetCode 风格题解是 class Solution { ... }（没有 main），
 *   那种代码提交进来必然编译失败——这是有意的。
 *
 * ⚠ 这不是安全沙箱：用户代码以当前用户权限在系统临时目录里跑（设计文档 §5.4）。
 */

const fs = require("node:fs")
const path = require("node:path")

const { normalizeCases } = require("../core/judge-spec/cases.js")
const { compareOutput } = require("../core/judge-spec/compare.js")
const { createSandbox, runCommand, resolveToolchain } = require("./sandbox.js")

/** 支持的语言 */
const LANGUAGES = ["cpp", "python"]

/** 五类 verdict，顺序即面板展示顺序 */
const VERDICTS = ["pass", "wrong_answer", "timeout", "compile_error", "runtime_error"]

/** 每类 verdict 的中文面板文案 */
const VERDICT_PANEL = {
  pass: {
    title: "通过",
    tone: "ok",
    hint: "全部用例通过，程序行为符合预期。",
  },
  wrong_answer: {
    title: "答案错误",
    tone: "bad",
    hint: "程序编译通过、也正常退出了，但输出与期望不一致。",
  },
  timeout: {
    title: "超时",
    tone: "warn",
    hint: "单个用例的运行时间超过了上限，已强制结束整棵进程树。",
  },
  compile_error: {
    title: "编译失败",
    tone: "bad",
    hint: "代码没能编译成可执行文件，下面是编译器原文。",
  },
  runtime_error: {
    title: "运行时错误",
    tone: "bad",
    hint: "程序运行时报错或以非零状态退出，下面是错误输出。",
  },
}

/** 单用例默认超时（毫秒） */
const DEFAULT_CASE_TIMEOUT_MS = 2000

/** 编译超时（毫秒）：编译比单个用例慢得多，单独给一份预算 */
const COMPILE_TIMEOUT_MS = 10000

/**
 * 取第一行非空文本。
 * @param {string} text
 * @returns {string}
 */
function firstNonEmptyLine(text) {
  const lines = String(text === null || text === undefined ? "" : text).split(/\r?\n/)
  for (const line of lines) {
    const trimmed = line.trim()
    if (trimmed !== "") return trimmed
  }
  return ""
}

/**
 * 从编译器输出里解析第一个错误位置。
 * g++ 的格式是 "main.cpp:4:5: error: 'xxx' was not declared in this scope"。
 * 链接期错误（缺 main）没有行号，这时退化成中文说明。
 * @param {string} output
 * @returns {{line: number|null, column: number|null, text: string}}
 */
function parseCompilerError(output) {
  const text = String(output || "")

  // 捕获组：1=文件名 2=行号 3=列号（可选） 4=原因
  const located = /(\S+\.(?:cpp|cc|cxx|c|h|hpp)):(\d+)(?::(\d+))?:\s*(?:fatal\s+)?error:\s*(.*)$/m.exec(text)
  if (located) {
    return {
      line: Number(located[2]),
      column: located[3] === undefined ? null : Number(located[3]),
      text: located[4].trim(),
    }
  }

  // 缺 main：mingw 报 WinMain，Linux 报 main，两种都要认
  if (/undefined reference to [`'"]?(Win)?main[`'"]?/i.test(text)) {
    return {
      line: null,
      column: null,
      text: "没有 main 函数：判题契约要求提交完整的 stdin→stdout 程序，用户自己写 main()",
    }
  }

  const plain = /(?:^|\n)([^\n]*error:[^\n]*)/i.exec(text)
  if (plain) {
    return { line: null, column: null, text: plain[1].trim() }
  }

  const symbol = /undefined reference to [`'"]?([^`'"\s(]+)/.exec(text)
  if (symbol) {
    return { line: null, column: null, text: "undefined reference to " + symbol[1] }
  }

  return { line: null, column: null, text: firstNonEmptyLine(text) }
}

/**
 * 组装判题结果。message 统一以 verdict 的中文标题开头，面板直接用它当标题。
 */
function makeResult(verdict, passedCases, totalCases, started, failedCase, detail, compilerOutput) {
  const panel = VERDICT_PANEL[verdict]
  return {
    verdict: verdict,
    passedCases: passedCases,
    totalCases: totalCases,
    durationMs: Date.now() - started,
    failedCase: failedCase,
    message: panel.title + "：" + detail,
    compilerOutput: compilerOutput || "",
  }
}

/**
 * 找可执行文件。mingw 编译 -o main 产出的是 main.exe。
 * @param {string} dir
 * @returns {string}
 */
function resolveBinary(dir) {
  const names = process.platform === "win32" ? ["main.exe", "main"] : ["main", "main.exe"]
  for (const name of names) {
    const file = path.join(dir, name)
    if (fs.existsSync(file)) return file
  }
  return path.join(dir, names[0])
}

/**
 * 判一道题。
 * @param {{problem: object, code: string, language: string, timeoutMs?: number, toolchain?: object}} params
 *   toolchain：{compiler, python}；缺省就现场探测
 * @returns {Promise<{
 *   verdict: string, passedCases: number, totalCases: number, durationMs: number,
 *   failedCase: {index: number, input: string, expected: string, actual: string}|null,
 *   message: string, compilerOutput: string
 * }>}
 */
async function judge(params) {
  const p = params || {}
  const problem = p.problem || {}
  const code = String(p.code === null || p.code === undefined ? "" : p.code)
  const language = String(p.language || "cpp").toLowerCase()
  const caseTimeoutMs = Number(p.timeoutMs) > 0 ? Number(p.timeoutMs) : DEFAULT_CASE_TIMEOUT_MS

  const cases = normalizeCases(problem.cases)
  const started = Date.now()

  // 语言不认识，直接报回去，不 spawn 任何东西
  if (LANGUAGES.indexOf(language) === -1) {
    return makeResult(
      "compile_error", 0, cases.length, started, null,
      "不支持的语言：" + language + "（只支持 cpp 与 python）", ""
    )
  }

  if (cases.length === 0) {
    // verdict 只有五类（G27），没有用例不属于任何一类，落到 wrong_answer 并说清原因。
    // 正常路径不会走到这里：server/api.js 的 POST /api/attempt 会先挡一道并返回 400。
    return makeResult(
      "wrong_answer", 0, 0, started, null,
      "题目没有测试用例，无法判定是否通过；请先在题库里补齐用例", ""
    )
  }

  const toolchain = p.toolchain || resolveToolchain({})
  const sandbox = createSandbox()

  try {
    let command = ""
    let args = []

    if (language === "cpp") {
      const compiler = toolchain.compiler
      if (!compiler) {
        return makeResult(
          "compile_error", 0, cases.length, started, null,
          "本机没有找到 g++，无法编译 C++ 代码", ""
        )
      }
      fs.writeFileSync(path.join(sandbox.dir, "main.cpp"), code, "utf8")
      const compiled = await runCommand(
        compiler.cmd,
        ["-std=c++17", "-O2", "main.cpp", "-o", "main"],
        { cwd: sandbox.dir, timeoutMs: COMPILE_TIMEOUT_MS }
      )
      if (compiled.timedOut) {
        return makeResult(
          "compile_error", 0, cases.length, started, null,
          "编译超时（超过 " + COMPILE_TIMEOUT_MS + "ms）", compiled.stderr
        )
      }
      if (compiled.code !== 0) {
        const detail = parseCompilerError(compiled.stderr)
        const where = detail.line === null ? "" : "第 " + detail.line + " 行："
        return makeResult(
          "compile_error", 0, cases.length, started, null,
          where + detail.text, compiled.stderr
        )
      }
      command = resolveBinary(sandbox.dir)
      args = []
    } else {
      const python = toolchain.python
      if (!python) {
        return makeResult(
          "compile_error", 0, cases.length, started, null,
          "本机没有找到可用的 Python 解释器（试过 python3 / python / py -3）", ""
        )
      }
      fs.writeFileSync(path.join(sandbox.dir, "main.py"), code, "utf8")
      command = python.cmd
      args = python.args.concat(["main.py"])
    }

    const pythonEnv = Object.assign({}, process.env, { PYTHONIOENCODING: "utf-8" })
    let passed = 0

    for (const item of cases) {
      const run = await runCommand(command, args, {
        cwd: sandbox.dir,
        input: item.input === "" ? "" : item.input + "\n",
        timeoutMs: caseTimeoutMs,
        env: language === "python" ? pythonEnv : process.env,
      })

      if (run.timedOut) {
        return makeResult(
          "timeout", passed, cases.length, started, null,
          "第 " + item.index + " 个用例超时：运行超过 " + caseTimeoutMs + "ms 后被强制结束",
          run.stderr
        )
      }

      if (run.code !== 0) {
        const how = run.code === null && run.signal
          ? "被信号 " + run.signal + " 终止"
          : "退出码 " + run.code
        return makeResult(
          "runtime_error", passed, cases.length, started, null,
          "第 " + item.index + " 个用例运行失败（" + how + "）：" + firstNonEmptyLine(run.stderr),
          run.stderr
        )
      }

      const cmp = compareOutput(run.stdout, item.expect, item.tol)
      if (!cmp.equal) {
        return makeResult(
          "wrong_answer", passed, cases.length, started,
          { index: item.index, input: item.input, expected: item.expect, actual: run.stdout },
          "第 " + item.index + " 个用例不通过：" + cmp.reason,
          run.stderr
        )
      }

      passed += 1
    }

    return makeResult(
      "pass", passed, cases.length, started, null,
      "全部 " + cases.length + " 个用例通过", ""
    )
  } finally {
    // G25：临时目录与编译产物判完即删
    sandbox.cleanup()
  }
}

module.exports = {
  LANGUAGES: LANGUAGES,
  VERDICTS: VERDICTS,
  VERDICT_PANEL: VERDICT_PANEL,
  DEFAULT_CASE_TIMEOUT_MS: DEFAULT_CASE_TIMEOUT_MS,
  COMPILE_TIMEOUT_MS: COMPILE_TIMEOUT_MS,
  judge: judge,
  parseCompilerError: parseCompilerError,
  firstNonEmptyLine: firstNonEmptyLine,
  resolveBinary: resolveBinary,
}
