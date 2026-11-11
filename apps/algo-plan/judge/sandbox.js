"use strict"
/**
 * sandbox.js —— 判题沙箱：临时目录、工具链探测、带超时的子进程执行
 *
 * ⚠ 这不是安全沙箱。用户代码以当前用户权限在系统临时目录里跑。
 *   它防的是：跑飞、死循环、爆内存、编译出一堆垃圾。
 *   它不防：恶意代码（删文件、联网、挖矿）。真隔离要上 Docker 或受限账号
 *   ——那是另一个项目，不塞进这里（设计文档 §5.4）。
 */

const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { spawn, spawnSync } = require("node:child_process")

/** 单用例默认超时（毫秒） */
const DEFAULT_TIMEOUT_MS = 2000

/** 探测工具链时的超时（毫秒） */
const DETECT_TIMEOUT_MS = 5000

/**
 * 单次运行的输出上限（字节）。超过就截断。
 * 不设上限的话，一个 while(true) print 的程序能把 stdout 撑到几百 MB，
 * Buffer.concat().toString() 会撞上 V8 的字符串长度上限抛 ERR_STRING_TOO_LONG，
 * 而那行代码在 close 事件回调里 —— 抛出去就是 uncaughtException，整个服务进程直接死。
 */
const MAX_OUTPUT_BYTES = 2 * 1024 * 1024

/**
 * 杀掉进程树之后仍不退出时的宽限期（毫秒）。
 * 超时了还不收摊，就是进程树杀不动（taskkill 失败、进程卡在不可中断 IO 等），
 * 这时必须强行按超时判定收结果，否则这个请求永远挂着，临时目录也永远删不掉。
 */
const KILL_GRACE_MS = 2000

/**
 * Python 解释器的探测顺序。
 * 本机 python3 是 Microsoft Store 的空占位符（无输出、退出码 49），
 * 真身是 python（3.11.15）与 py -3（3.12.3），所以必须依次试（G17）。
 */
const PYTHON_CANDIDATES = [
  { cmd: "python3", args: [] },
  { cmd: "python", args: [] },
  { cmd: "py", args: ["-3"] },
]

/** 探测结果缓存，避免每次判题都 spawn 一遍探测 */
const detectionCache = { compiler: undefined, python: undefined }

/** 清空探测缓存（测试用） */
function resetDetectionCache() {
  detectionCache.compiler = undefined
  detectionCache.python = undefined
}

/**
 * 探测 C/C++ 编译器。
 * 同步实现（spawnSync）：接口契约把 detectCompiler/detectPython 定成非 Promise，
 * 探测本身也只是跑一次 --version，同步更省事。
 * @returns {{cmd: string, version: string}|null}
 */
function detectCompiler() {
  if (detectionCache.compiler !== undefined) return detectionCache.compiler
  const run = spawnSync("g++", ["--version"], {
    timeout: DETECT_TIMEOUT_MS,
    encoding: "utf8",
    windowsHide: true,
  })
  const stdout = String((run && run.stdout) || "")
  if (!run || run.error || run.status !== 0 || stdout.trim() === "") {
    detectionCache.compiler = null
    return null
  }
  detectionCache.compiler = {
    cmd: "g++",
    version: stdout.trim().split("\n")[0].trim(),
  }
  return detectionCache.compiler
}

/**
 * 把 "py -3" 这种用户输入拆成 {cmd, args}。
 * @param {string} text
 * @returns {{cmd: string, args: string[]}}
 */
function parseCandidate(text) {
  const parts = String(text === null || text === undefined ? "" : text)
    .trim()
    .split(/\s+/)
    .filter(function (s) {
      return s !== ""
    })
  return { cmd: parts[0] || "python", args: parts.slice(1) }
}

/**
 * 探测 Python 解释器。
 * 同步实现（spawnSync）：接口契约把 detectPython 定成非 Promise。
 * @param {string|null} [override] 用户在设置里指定的解释器，形如 "python" 或 "py -3"
 * @returns {{cmd: string, args: string[], version: string}|null}
 */
function detectPython(override) {
  // 没指定解释器时，缓存里已经探到就直接用，不重复 spawn
  if (!override && detectionCache.python) return detectionCache.python

  // 指定了解释器时，若缓存里探到的正是它，也直接复用——
  // 否则每次判题都会重新 spawn 一遍 python --version，白白多几百毫秒。
  if (override && detectionCache.python) {
    const cached = detectionCache.python
    const cachedText = cached.cmd + (cached.args.length ? " " + cached.args.join(" ") : "")
    if (cachedText === String(override).trim()) return cached
  }

  const candidates = override
    ? [parseCandidate(override)].concat(PYTHON_CANDIDATES)
    : PYTHON_CANDIDATES

  for (const candidate of candidates) {
    const run = spawnSync(candidate.cmd, candidate.args.concat(["--version"]), {
      timeout: DETECT_TIMEOUT_MS,
      encoding: "utf8",
      windowsHide: true,
    })
    const stdout = String((run && run.stdout) || "")
    // 判据是「退出码 0 且有输出」——Store 占位符 python3 退出码是 49 且无输出，会被正确跳过
    if (run && !run.error && run.status === 0 && stdout.trim() !== "") {
      const found = {
        cmd: candidate.cmd,
        args: candidate.args.slice(),
        version: stdout.trim().split("\n")[0].trim(),
      }
      detectionCache.python = found
      return found
    }
  }

  detectionCache.python = null
  return null
}

/**
 * 组装判题要用的工具链。
 * @param {{pythonOverride?: string|null}} [options]
 * @returns {{compiler: object|null, python: object|null}}
 */
function resolveToolchain(options) {
  const opts = options || {}
  return {
    compiler: detectCompiler(),
    python: detectPython(opts.pythonOverride || null),
  }
}

/**
 * 建一个临时目录。判完必须调 cleanup()（G25）。
 * @returns {{dir: string, cleanup: () => void}}
 */
function createSandbox() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "algo-plan-judge-"))
  let cleaned = false
  return {
    dir: dir,
    cleanup: function () {
      if (cleaned) return
      cleaned = true
      try {
        fs.rmSync(dir, { recursive: true, force: true })
      } catch (e) {
        // 临时目录删不掉不算失败：判题结果已经拿到手了
      }
    },
  }
}

/**
 * 杀整棵进程树。
 * Windows：taskkill /T /F；POSIX：杀整个进程组（spawn 时开了 detached）。
 * taskkill 失败时退回 child.kill()（只能杀直接子进程，聊胜于无）。
 * @param {import("node:child_process").ChildProcess} child
 * @returns {boolean} 是否看起来杀掉了
 */
function killTree(child) {
  if (!child || !child.pid) return false
  const pid = child.pid
  if (process.platform === "win32") {
    try {
      const res = spawnSync("taskkill", ["/PID", String(pid), "/T", "/F"], {
        stdio: "ignore",
        windowsHide: true,
      })
      if (res && res.status === 0) return true
    } catch (e) {
      // 进程可能已经自己退了，杀不掉就算了
    }
    try {
      return typeof child.kill === "function" ? child.kill("SIGKILL") : false
    } catch (e) {
      return false
    }
  }
  let killed = false
  try {
    process.kill(-pid, "SIGKILL")
    killed = true
  } catch (e) {}
  try {
    process.kill(pid, "SIGKILL")
    killed = true
  } catch (e) {}
  return killed
}

/**
 * 跑一个子进程，带超时。超时后杀整棵进程树并把 timedOut 置 true。
 * @param {string} cmd
 * @param {string[]} args
 * @param {{cwd?: string, input?: string, timeoutMs?: number, env?: object}} [options]
 * @returns {Promise<{code: number|null, stdout: string, stderr: string, durationMs: number, timedOut: boolean, signal: string|null, truncated: boolean}>}
 */
function runCommand(cmd, args, options) {
  const opts = options || {}
  const timeoutMs = Number(opts.timeoutMs) > 0 ? Number(opts.timeoutMs) : DEFAULT_TIMEOUT_MS
  const started = Date.now()

  return new Promise(function (resolve) {
    let child
    try {
      child = spawn(cmd, args || [], {
        cwd: opts.cwd,
        // POSIX 上开新进程组，超时时能整组杀；Windows 上不能开（会弹新控制台）
        detached: process.platform !== "win32",
        stdio: ["pipe", "pipe", "pipe"],
        windowsHide: true,
        env: opts.env || process.env,
      })
    } catch (e) {
      resolve({
        code: null,
        stdout: "",
        stderr: String((e && e.message) || e),
        durationMs: Date.now() - started,
        timedOut: false,
        signal: null,
        truncated: false,
      })
      return
    }

    const outChunks = []
    const errChunks = []
    let outBytes = 0
    let errBytes = 0
    let truncated = false
    let timedOut = false
    let settled = false
    let timer = null
    let graceTimer = null

    /**
     * 把一段输出塞进缓冲区，超出上限就截断。
     * 注意：超限时**继续把流读干净**（只是不存），否则子进程写 stdout 写不动会卡死。
     * @param {Buffer} chunk
     * @param {Buffer[]} chunks
     * @param {number} used 已占用的字节数
     * @returns {number} 新的占用字节数
     */
    function push(chunk, chunks, used) {
      if (used >= MAX_OUTPUT_BYTES) {
        truncated = true
        return used
      }
      const room = MAX_OUTPUT_BYTES - used
      if (chunk.length > room) {
        chunks.push(chunk.subarray(0, room))
        truncated = true
        return MAX_OUTPUT_BYTES
      }
      chunks.push(chunk)
      return used + chunk.length
    }

    function finish(code, signal, spawnError) {
      if (settled) return
      settled = true
      if (timer) clearTimeout(timer)
      if (graceTimer) clearTimeout(graceTimer)
      // 拼字符串这一步必须包 try/catch：万一还有别的路径把输出撑爆，
      // 也不能让异常从事件回调里逃出去变成 uncaughtException（那会整个服务进程死掉）。
      let stdout = ""
      let stderr = ""
      try {
        stdout = Buffer.concat(outChunks).toString("utf8")
      } catch (e) {
        stdout = ""
        truncated = true
      }
      try {
        stderr = spawnError ? String(spawnError) : Buffer.concat(errChunks).toString("utf8")
      } catch (e) {
        stderr = ""
        truncated = true
      }
      resolve({
        code: code === undefined ? null : code,
        stdout: stdout,
        stderr: stderr,
        durationMs: Date.now() - started,
        timedOut: timedOut,
        signal: signal === undefined ? null : signal,
        truncated: truncated,
      })
    }

    timer = setTimeout(function () {
      timedOut = true
      killTree(child)
      // 杀完还活着就等一会儿；宽限期到了强行按超时收摊，
      // 否则这个请求会永远挂着，临时目录也永远删不掉。
      graceTimer = setTimeout(function () {
        // 管道还开着的话，父进程的 event loop 会被它吊住（野进程不退出就一直在等），
        // 把流拆掉，别让判题之外的资源跟着泄漏。
        try {
          if (child.stdout) child.stdout.destroy()
        } catch (e) {}
        try {
          if (child.stderr) child.stderr.destroy()
        } catch (e) {}
        finish(
          child.exitCode === undefined ? null : child.exitCode,
          child.signalCode === undefined ? null : child.signalCode,
          "进程树在超时后未能结束，已强制按超时判定"
        )
      }, KILL_GRACE_MS)
    }, timeoutMs)

    if (child.stdout) {
      child.stdout.on("data", function (chunk) {
        outBytes = push(chunk, outChunks, outBytes)
      })
    }
    if (child.stderr) {
      child.stderr.on("data", function (chunk) {
        errBytes = push(chunk, errChunks, errBytes)
      })
    }
    child.on("error", function (err) {
      finish(null, null, (err && err.message) || String(err))
    })
    child.on("close", function (code, signal) {
      finish(code, signal, null)
    })

    if (child.stdin) {
      // 子进程提前退出时写 stdin 会 EPIPE，这里把错误吞掉
      child.stdin.on("error", function () {})
      if (opts.input !== undefined && opts.input !== null) {
        child.stdin.write(String(opts.input))
      }
      child.stdin.end()
    }
  })
}

module.exports = {
  DEFAULT_TIMEOUT_MS: DEFAULT_TIMEOUT_MS,
  DETECT_TIMEOUT_MS: DETECT_TIMEOUT_MS,
  MAX_OUTPUT_BYTES: MAX_OUTPUT_BYTES,
  KILL_GRACE_MS: KILL_GRACE_MS,
  PYTHON_CANDIDATES: PYTHON_CANDIDATES,
  detectCompiler: detectCompiler,
  detectPython: detectPython,
  parseCandidate: parseCandidate,
  resolveToolchain: resolveToolchain,
  createSandbox: createSandbox,
  killTree: killTree,
  runCommand: runCommand,
  resetDetectionCache: resetDetectionCache,
}
