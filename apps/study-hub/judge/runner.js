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
const { validateAsserts, compareByMode } = require("./asserts.js")
const {
  createSandbox,
  runCommand,
  resolveToolchain,
  clampTimeoutMs,
  copyIntoSandbox,
  copyDirIntoSandbox,
  collectArtifacts,
} = require("./sandbox.js")

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
    hint: "代码没能编译成可执行文件（也可能是本机缺少编译器），下面是编译器原文。",
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
 * 卡类判题的脚本超时（毫秒）。脚本要先把 demo 跑一遍，比单跑一个用例慢得多。
 */
const SCRIPT_TIMEOUT_MS = 30000

/**
 * 卡类脚本成功时必须打印的标记（接口契约 §3.5 冻结）。
 * judgeUnit 靠它把「脚本跑完了」和「脚本真的全过了」区分开。
 */
const SCRIPT_PASS_MARK = "全部断言通过"

/**
 * 卡类脚本失败时打印的前缀（接口契约 §3.5 冻结）。
 */
const SCRIPT_FAIL_PREFIX = "断言失败："

/**
 * C++ 编译命令的参数。固定不变（G20）：
 *   -std=c++17  统一 C++ 标准
 *   -O2         开优化
 *   不加 -Wall / -Werror——用户的代码有点小毛病不该直接判死。
 * 导出成常量是为了让测试能直接断言这几个参数不会被顺手改掉。
 */
const CPP_COMPILE_ARGS = ["-std=c++17", "-O2", "main.cpp", "-o", "main"]

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
 * 组装 judgeUnit 的结果。字段与既有 judge() 的结果同构，
 * 额外多出 stdout / stderr（单元页右栏要展示 demo 输出与错误输出）。
 */
function makeUnitResult(verdict, passedCases, totalCases, started, failedCase, detail, stdout, stderr, compilerOutput) {
  const panel = VERDICT_PANEL[verdict]
  return {
    verdict: verdict,
    passedCases: passedCases,
    totalCases: totalCases,
    durationMs: Date.now() - started,
    failedCase: failedCase,
    message: panel.title + "：" + detail,
    compilerOutput: compilerOutput || "",
    stdout: stdout || "",
    stderr: stderr || "",
  }
}

/**
 * 从脚本的 stdout 里挑出「断言失败：…」那几行，拼成一句中文说明。
 * @param {string} stdout
 * @returns {string}
 */
function collectFailLines(stdout) {
  const lines = String(stdout || "").split(/\r?\n/)
  const picked = []
  for (const line of lines) {
    if (line.indexOf(SCRIPT_FAIL_PREFIX) !== -1) picked.push(line.trim())
  }
  return picked.join("；")
}

/**
 * 组装卡类单元的断言集：隐含断言在前，单元自带断言追加在后。
 * 隐含两条是契约 §7.4 冻结的：exit 0 + contains 成功标记。
 * @param {object} unit
 * @returns {Array<object>}
 */
function buildCardAsserts(unit) {
  const own = Array.isArray(unit && unit.asserts) ? unit.asserts : []
  return [
    { type: "exit", expect: 0 },
    { type: "contains", text: SCRIPT_PASS_MARK },
  ].concat(own)
}

/**
 * 判一个卡类单元：跑它自带的断言脚本。
 * @param {object} ctx {p, unit, entry, own, started}
 * @returns {Promise<object>}
 */
async function judgeCardScript(ctx) {
  const p = ctx.p
  const entry = ctx.entry
  const started = ctx.started
  const asserts = buildCardAsserts(ctx.unit)
  const scriptRel = String(entry.script || "")
  const learningRoot = p.learningRoot ? String(p.learningRoot) : ""

  if (!scriptRel) {
    return makeUnitResult("compile_error", 0, asserts.length, started, null,
      "这张卡没有配断言脚本，判不了", "", "", "")
  }
  if (!learningRoot) {
    return makeUnitResult("compile_error", 0, asserts.length, started, null,
      "还没连上 learning 仓，卡片判不了", "", "", "")
  }

  const scriptAbs = path.resolve(learningRoot, scriptRel)
  if (!fs.existsSync(scriptAbs)) {
    // 契约 §12：脚本缺失是 compile_error（附中文说明），不是 500。
    // totalCases 用 asserts.length（**不是 0**）：这条路径上断言集是**已知的**（隐含两条 + 单元自带），
    // 只是还没跑。返回 0 会让 UI 显示「0/0」，与同函数里其它 compile_error 分支
    // （「没配脚本」「没连 learning 仓」都给 asserts.length）不一致。
    return makeUnitResult("compile_error", 0, asserts.length, started, null,
      "断言脚本不存在：" + scriptRel, "", "", "")
  }

  const toolchain = p.toolchain || resolveToolchain({ pythonOverride: p.pythonOverride || null })
  const python = toolchain.python
  if (!python) {
    return makeUnitResult("compile_error", 0, asserts.length, started, null,
      "本机没有找到可用的 Python 解释器（试过 python3 / python / py -3）", "", "", "")
  }

  const sandbox = p.sandbox || createSandbox()
  const owned = !p.sandbox
  const timeoutMs = p.timeoutMs ? clampTimeoutMs(p.timeoutMs) : SCRIPT_TIMEOUT_MS

  try {
    // demo 目录整个拷进沙箱：demo 会往自己目录里写 .db，
    // 直接在 learning 仓里跑会污染工作树（G18）。
    let demoAbs = ""
    const demoRel = String(entry.demo || "")
    if (demoRel) {
      const demoFile = path.resolve(learningRoot, demoRel)
      const copiedDir = copyDirIntoSandbox(sandbox, path.dirname(demoFile), "demo")
      demoAbs = path.join(copiedDir, path.basename(demoFile))
    }
    const scriptTarget = copyIntoSandbox(sandbox, scriptAbs, path.basename(scriptRel))

    const env = Object.assign({}, process.env, {
      // Windows 上默认 GBK，脚本里的中文会直接抛 UnicodeEncodeError（G21）
      PYTHONIOENCODING: "utf-8",
      STUDY_HUB_DEMO: demoAbs,
    })

    const run = await runCommand(python.cmd, python.args.concat([scriptTarget]), {
      cwd: sandbox.dir,
      timeoutMs: timeoutMs,
      env: env,
    })

    const stdout = String(run.stdout || "")
    const stderr = String(run.stderr || "")
    const artifacts = collectArtifacts(sandbox, asserts)

    // 契约 §7.4 的 verdict 映射顺序：命中即止
    if (run.timedOut) {
      return makeUnitResult("timeout", 0, asserts.length, started, null,
        "断言脚本超时（超过 " + timeoutMs + "ms）", stdout, stderr, "")
    }
    if (/SyntaxError|IndentationError/.test(stderr)) {
      return makeUnitResult("compile_error", 0, asserts.length, started, null,
        "断言脚本有语法错误：" + firstNonEmptyLine(stderr), stdout, stderr, stderr)
    }
    if (stderr.indexOf("ModuleNotFoundError") !== -1 && /numpy/.test(stderr)) {
      return makeUnitResult("runtime_error", 0, asserts.length, started, null,
        "环境缺失：本机没有装 numpy，这张卡暂时跑不了（装上 numpy 即可运行）", stdout, stderr, stderr)
    }
    if (stderr.indexOf("Traceback (most recent call last)") !== -1) {
      return makeUnitResult("runtime_error", 0, asserts.length, started, null,
        "断言脚本或 demo 崩了：" + firstNonEmptyLine(stderr), stdout, stderr, stderr)
    }
    if (run.code === 1) {
      const detail = collectFailLines(stdout)
      return makeUnitResult("wrong_answer", 0, asserts.length, started,
        { index: 0, type: "exit", reason: "断言脚本没有通过" },
        detail || "断言脚本没有通过（退出码 1）", stdout, stderr, "")
    }
    if (run.code !== 0) {
      return makeUnitResult("runtime_error", 0, asserts.length, started, null,
        "断言脚本异常退出（退出码 " + run.code + "）", stdout, stderr, stderr)
    }

    const res = validateAsserts(asserts, {
      stdout: stdout,
      stderr: stderr,
      exitCode: run.code,
      timedOut: false,
      artifacts: artifacts,
    })
    if (res.failures.length > 0) {
      return makeUnitResult("wrong_answer", res.passed, res.total, started,
        res.failures[0], res.failures[0].reason, stdout, stderr, "")
    }
    return makeUnitResult("pass", res.passed, res.total, started, null,
      "全部 " + res.total + " 条断言通过", stdout, stderr, "")
  } finally {
    if (owned) sandbox.cleanup()
  }
}

/**
 * 判一个题目类单元：跑用户提交的代码。
 * @param {object} ctx {p, unit, entry, own, started}
 * @returns {Promise<object>}
 */
async function judgeUserCode(ctx) {
  const p = ctx.p
  const entry = ctx.entry
  const started = ctx.started
  const own = ctx.own
  const code = String(p.code === null || p.code === undefined ? "" : p.code)
  const language = String(p.language || entry.language || "cpp").toLowerCase()

  if (LANGUAGES.indexOf(language) === -1) {
    return makeUnitResult("compile_error", 0, own.length, started, null,
      "不支持的语言：" + language + "（只支持 cpp 与 python）", "", "", "")
  }
  if (own.length === 0) {
    return makeUnitResult("wrong_answer", 0, 0, started, null,
      "这个单元没有测试用例，无法判定是否通过；请先在题库里补齐用例", "", "", "")
  }

  const toolchain = p.toolchain || resolveToolchain({ pythonOverride: p.pythonOverride || null })
  const sandbox = p.sandbox || createSandbox()
  const owned = !p.sandbox
  const caseTimeoutMs = p.timeoutMs ? clampTimeoutMs(p.timeoutMs) : DEFAULT_CASE_TIMEOUT_MS

  try {
    let command = ""
    let args = []

    if (language === "cpp") {
      const compiler = toolchain.compiler
      if (!compiler) {
        return makeUnitResult("compile_error", 0, own.length, started, null,
          "本机没有找到 g++，无法编译 C++ 代码", "", "", "")
      }
      fs.writeFileSync(path.join(sandbox.dir, "main.cpp"), code, "utf8")
      const compiled = await runCommand(compiler.cmd, CPP_COMPILE_ARGS, {
        cwd: sandbox.dir,
        timeoutMs: COMPILE_TIMEOUT_MS,
      })
      const compilerOut = [compiled.stderr, compiled.stdout].filter(Boolean).join("\n")
      if (compiled.timedOut) {
        return makeUnitResult("compile_error", 0, own.length, started, null,
          "编译超时（超过 " + COMPILE_TIMEOUT_MS + "ms）", "", "", compilerOut)
      }
      if (compiled.code !== 0) {
        const detail = parseCompilerError(compilerOut)
        const where = detail.line === null ? "" : "第 " + detail.line + " 行："
        return makeUnitResult("compile_error", 0, own.length, started, null,
          where + detail.text, "", "", compilerOut)
      }
      command = resolveBinary(sandbox.dir)
      args = []
    } else {
      const python = toolchain.python
      if (!python) {
        return makeUnitResult("compile_error", 0, own.length, started, null,
          "本机没有找到可用的 Python 解释器（试过 python3 / python / py -3）", "", "", "")
      }
      fs.writeFileSync(path.join(sandbox.dir, "main.py"), code, "utf8")
      command = python.cmd
      args = python.args.concat(["main.py"])
    }

    const childEnv =
      language === "python"
        ? Object.assign({}, process.env, { PYTHONIOENCODING: "utf-8" })
        : process.env

    const outputAsserts = own.filter(function (a) { return a && a.type === "output" })
    const otherAsserts = own.filter(function (a) { return a && a.type !== "output" })

    let passed = 0
    let lastRun = null

    // 逐条 output 断言各跑一次进程；第一条不匹配就停（与既有 judge() 一致）
    for (let i = 0; i < outputAsserts.length; i += 1) {
      const a = outputAsserts[i]
      const run = await runCommand(command, args, {
        cwd: sandbox.dir,
        input: a.input === undefined || a.input === null || a.input === "" ? "" : String(a.input) + "\n",
        timeoutMs: caseTimeoutMs,
        env: childEnv,
      })
      lastRun = run
      if (run.timedOut) {
        return makeUnitResult("timeout", passed, own.length, started, null,
          "第 " + (i + 1) + " 个用例超时：运行超过 " + caseTimeoutMs + "ms 后被强制结束",
          "", run.stderr, run.stderr)
      }
      if (run.code !== 0) {
        let how
        if (run.code === null && run.signal) how = "被信号 " + run.signal + " 终止"
        else if (run.code === null) how = "进程启动失败"
        else how = "退出码 " + run.code
        return makeUnitResult("runtime_error", passed, own.length, started, null,
          "第 " + (i + 1) + " 个用例运行失败（" + how + "）：" + firstNonEmptyLine(run.stderr),
          "", run.stderr, run.stderr)
      }
      const cmp = compareByMode(run.stdout, a.expect, a.cmp || "exact")
      if (!cmp.equal) {
        const cutNote = run.truncated ? "（程序输出过长，已截断后比对）" : ""
        return makeUnitResult("wrong_answer", passed, own.length, started,
          { index: own.indexOf(a), type: "output", reason: cmp.reason },
          "第 " + (i + 1) + " 个用例不通过：" + cmp.reason + cutNote,
          run.stdout, run.stderr, run.stderr)
      }
      passed += 1
    }

    // exit / contains / file 断言对最后一次运行的产物判定
    if (otherAsserts.length > 0) {
      if (!lastRun) {
        lastRun = await runCommand(command, args, {
          cwd: sandbox.dir,
          timeoutMs: caseTimeoutMs,
          env: childEnv,
        })
      }
      if (lastRun.timedOut) {
        return makeUnitResult("timeout", passed, own.length, started, null,
          "运行超时：超过 " + caseTimeoutMs + "ms 后被强制结束", "", lastRun.stderr, lastRun.stderr)
      }
      const artifacts = collectArtifacts(sandbox, own)
      const res = validateAsserts(otherAsserts, {
        stdout: lastRun.stdout,
        stderr: lastRun.stderr,
        exitCode: lastRun.code,
        timedOut: false,
        artifacts: artifacts,
      })
      passed += res.passed
      if (res.failures.length > 0) {
        const first = res.failures[0]
        return makeUnitResult("wrong_answer", passed, own.length, started,
          { index: own.indexOf(otherAsserts[first.index]), type: first.type, reason: first.reason },
          first.reason, lastRun.stdout, lastRun.stderr, lastRun.stderr)
      }
    }

    return makeUnitResult("pass", passed, own.length, started, null,
      "全部 " + own.length + " 条断言通过", "", "", "")
  } finally {
    if (owned) sandbox.cleanup()
  }
}

/**
 * 判一个学习单元。kind: problem 判用户提交的代码，kind: card 判单元自带的断言脚本，
 * 两者走同一套「跑 → 收集 → 逐条校验 → verdict」的核心。
 * @param {{unit: object, code?: string, language?: string, timeoutMs?: number,
 *          toolchain?: object, sandbox?: object, learningRoot?: string|null,
 *          pythonOverride?: string|null}} params
 * @returns {Promise<object>}
 */
async function judgeUnit(params) {
  const p = params || {}
  const unit = p.unit || {}
  const entry = unit.entry || {}
  const kind = String(unit.kind || "")
  const graded = String(entry.graded || (kind === "card" ? "script" : "user"))
  const own = Array.isArray(unit.asserts) ? unit.asserts : []
  const started = Date.now()

  if (graded === "script") {
    return judgeCardScript({ p: p, unit: unit, entry: entry, own: own, started: started })
  }
  return judgeUserCode({ p: p, unit: unit, entry: entry, own: own, started: started })
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

  const toolchain = p.toolchain || resolveToolchain({ pythonOverride: p.pythonOverride || null })
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
        CPP_COMPILE_ARGS,
        { cwd: sandbox.dir, timeoutMs: COMPILE_TIMEOUT_MS }
      )
      // 编译器把诊断信息写到 stderr，但也有工具会写 stdout（警告、插件输出），
      // 两边都留着，只看 stderr 会让面板空掉。
      const compilerOut = [compiled.stderr, compiled.stdout].filter(Boolean).join("\n")
      if (compiled.timedOut) {
        return makeResult(
          "compile_error", 0, cases.length, started, null,
          "编译超时（超过 " + COMPILE_TIMEOUT_MS + "ms）", compilerOut
        )
      }
      if (compiled.code !== 0) {
        const detail = parseCompilerError(compilerOut)
        const where = detail.line === null ? "" : "第 " + detail.line + " 行："
        return makeResult(
          "compile_error", 0, cases.length, started, null,
          where + detail.text, compilerOut
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

    // Python 的标准输出编码必须钉死成 utf-8：Windows 上默认是 GBK，
    // 题目里的中文用例会直接抛 UnicodeEncodeError（G21）。
    const childEnv =
      language === "python"
        ? Object.assign({}, process.env, { PYTHONIOENCODING: "utf-8" })
        : process.env
    let passed = 0

    for (const item of cases) {
      const run = await runCommand(command, args, {
        cwd: sandbox.dir,
        input: item.input === "" ? "" : item.input + "\n",
        timeoutMs: caseTimeoutMs,
        env: childEnv,
      })

      if (run.timedOut) {
        return makeResult(
          "timeout", passed, cases.length, started, null,
          "第 " + item.index + " 个用例超时：运行超过 " + caseTimeoutMs + "ms 后被强制结束",
          run.stderr
        )
      }

      if (run.code !== 0) {
        // code 为 null 且没有 signal，说明进程根本没起来（spawn 失败），不是「退出码 null」
        let how
        if (run.code === null && run.signal) how = "被信号 " + run.signal + " 终止"
        else if (run.code === null) how = "进程启动失败"
        else how = "退出码 " + run.code
        return makeResult(
          "runtime_error", passed, cases.length, started, null,
          "第 " + item.index + " 个用例运行失败（" + how + "）：" + firstNonEmptyLine(run.stderr),
          run.stderr
        )
      }

      const cmp = compareOutput(run.stdout, item.expect, item.tol)
      if (!cmp.equal) {
        // 输出被截断过就说明实际输出是「超长且不匹配」，
        // 得说清楚，否则用户会对着一个被砍掉一半的输出找原因。
        const cutNote = run.truncated ? "（程序输出过长，已截断后比对）" : ""
        return makeResult(
          "wrong_answer", passed, cases.length, started,
          { index: item.index, input: item.input, expected: item.expect, actual: run.stdout },
          "第 " + item.index + " 个用例不通过：" + cmp.reason + cutNote,
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
  SCRIPT_TIMEOUT_MS: SCRIPT_TIMEOUT_MS,
  SCRIPT_PASS_MARK: SCRIPT_PASS_MARK,
  SCRIPT_FAIL_PREFIX: SCRIPT_FAIL_PREFIX,
  CPP_COMPILE_ARGS: CPP_COMPILE_ARGS,
  judge: judge,
  judgeUnit: judgeUnit,
  parseCompilerError: parseCompilerError,
  firstNonEmptyLine: firstNonEmptyLine,
  resolveBinary: resolveBinary,
}
