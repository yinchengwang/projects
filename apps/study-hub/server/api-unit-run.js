"use strict"
/**
 * api-unit-run.js —— P2 的单元运行接口
 *
 * 单独一个文件，**不动 P0 的 server/api-units.js**（那是 P0/P1 的地盘）：
 *   POST /api/units/:id/judge     跑断言 -> verdict
 *   POST /api/units/:id/complete  把单元进度置为 done
 *   POST /api/units/:id/ask       问 LLM（单元上下文）
 *   GET  /api/env                 环境自检（python / g++ / numpy）
 *
 * 判题要拿完整的 Unit（含 entry 与 asserts），所以查的是**内存里的单元索引**
 * （loadUnits 的 byId），不是 units 表——那张表只是摘要（契约 §4.2）。
 */

const { readJsonBody, HttpError } = require("./router.js")
const { judgeUnit } = require("../judge/runner.js")
const { detectCompiler, detectPython, detectNumpy } = require("../judge/sandbox.js")
const PYTHON_SETTING_KEY = "judge.python.cmd"

/** 只支持这两种语言（与 judge/runner.js 的 LANGUAGES 一致） */
const SUPPORTED_LANGUAGES = ["cpp", "python"]

/**
 * 服务层用「带 status 的普通 Error」表达业务错误（429 配额用完 / 503 没装配），
 * 路由层只认 HttpError。这里转一道——不然它们统统会变成 500，
 * 前端就分不清「参数写错了」和「服务端炸了」。
 */
function toHttpError(err) {
  if (err instanceof HttpError) return err
  const status = Number(err && err.status)
  if (status >= 400 && status <= 599) {
    return new HttpError(status, String((err && err.message) || "请求失败"))
  }
  return err
}

/**
 * 从单元索引里按 id 取单元。
 * byId 既可能是普通对象（契约 §3.4：{ [id]: Unit }），也可能是 Map——两种都认。
 * @param {{units?: object}} ctx
 * @param {string} id
 * @returns {object|null}
 */
function pickUnit(ctx, id) {
  const src = ctx.units || {}
  const byId = src.byId || src
  if (!byId) return null
  if (typeof byId.get === "function") return byId.get(id) || null
  if (Object.prototype.hasOwnProperty.call(byId, id)) return byId[id]
  return null
}

/** 响应里的单元摘要 */
function unitBrief(unit) {
  return {
    id: String(unit.id || ""),
    kind: String(unit.kind || ""),
    title: String(unit.title || ""),
    track: String(unit.track || ""),
  }
}

/**
 * 组装 P2 的单元运行路由表。
 * @param {{store: object, units: object, llm?: object, learning?: object, sendJson: Function}} ctx
 * @returns {Array<{method: string, pattern: string, handler: Function}>}
 */
function buildUnitRunRoutes(ctx) {
  const routes = []
  const store = ctx.store
  const llm = ctx.llm
  const learningRoot = ctx.learning && ctx.learning.root ? ctx.learning.root : null

  /**
   * 当前该用哪个 Python 解释器。
   *
   * 每次请求都重读 settings：设置页（或上一轮探测）改完就立即生效，不用重启服务。
   * 注入的 ctx.pythonOverride 优先（测试与显式指定用）。
   * @returns {string|null}
   */
  function pythonOverrideNow() {
    if (ctx.pythonOverride !== undefined && ctx.pythonOverride !== null) return ctx.pythonOverride
    if (store && typeof store.getSetting === "function") {
      const raw = store.getSetting(PYTHON_SETTING_KEY)
      if (raw !== undefined && raw !== null && String(raw).trim() !== "") return String(raw).trim()
    }
    return ctx.envPython || null
  }

  function notFound(res, id) {
    ctx.sendJson(res, 404, { error: "单元不存在", id: String(id) })
  }

  // 判题：一条路径，problem 走用户代码、card 走单元自带断言脚本
  routes.push({
    method: "POST",
    pattern: "/api/units/:id/judge",
    handler: async function (req, res, params) {
      const unit = pickUnit(ctx, params.id)
      if (!unit) {
        notFound(res, params.id)
        return
      }
      const body = await readJsonBody(req)
      const kind = String(unit.kind || "")
      let code = ""
      let language = ""

      // 卡类不收代码：它的入口是单元自带的断言脚本
      if (kind === "problem") {
        code = body.code === null || body.code === undefined ? "" : String(body.code)
        if (code.trim() === "") {
          ctx.sendJson(res, 400, { error: "代码不能为空" })
          return
        }
        const entry = unit.entry || {}
        language = String(body.language || entry.language || "cpp").toLowerCase()
        if (SUPPORTED_LANGUAGES.indexOf(language) === -1) {
          ctx.sendJson(res, 400, {
            error: "不支持的语言：" + language,
            supported: SUPPORTED_LANGUAGES.slice(),
          })
          return
        }
      }

      const result = await judgeUnit({
        unit: unit,
        code: code,
        language: language,
        learningRoot: learningRoot,
        pythonOverride: pythonOverrideNow(),
      })

      ctx.sendJson(res, 200, {
        verdict: result.verdict,
        passed: result.passedCases,
        total: result.totalCases,
        durationMs: result.durationMs,
        message: result.message,
        failed: result.failedCase || null,
        stdout: result.stdout || "",
        stderr: result.stderr || "",
        unit: unitBrief(unit),
      })
    },
  })

  // 完成：写 unit_progress（复习卡与掌握度是 P1/P3 的事，这里不碰）
  routes.push({
    method: "POST",
    pattern: "/api/units/:id/complete",
    handler: async function (req, res, params) {
      const unit = pickUnit(ctx, params.id)
      if (!unit) {
        notFound(res, params.id)
        return
      }
      await readJsonBody(req)
      if (!store || typeof store.upsertUnitProgress !== "function") {
        ctx.sendJson(res, 503, { error: "进度存储还没装配" })
        return
      }
      const lastAt = new Date().toISOString()
      store.upsertUnitProgress(unit.id, "done", lastAt)
      ctx.sendJson(res, 200, { unitId: unit.id, progress: "done", lastAt: lastAt })
    },
  })

  // 问 LLM：单元上下文（题面或讲解正文）喂给 explain
  routes.push({
    method: "POST",
    pattern: "/api/units/:id/ask",
    handler: async function (req, res, params) {
      const unit = pickUnit(ctx, params.id)
      if (!unit) {
        notFound(res, params.id)
        return
      }
      const body = await readJsonBody(req)
      if (!llm || typeof llm.explain !== "function") {
        // LLM 没装配不是错误：单元页照常能读能判，只是问不了（G10）
        ctx.sendJson(res, 200, {
          text: "LLM 没装配，问不了。可以先自己读讲解、跑一遍断言。",
          model: "null",
        })
        return
      }
      try {
        const result = await llm.explain({
          problem: {
            id: unit.id,
            title: unit.title || "",
            difficulty: unit.difficulty || "medium",
            topics: Array.isArray(unit.topics) ? unit.topics : [],
            statement: unit.statement || "",
          },
          level: String(body.level || "思路"),
          sessionId: body.sessionId,
        })
        ctx.sendJson(res, 200, { text: result.text, model: result.model })
      } catch (err) {
        throw toHttpError(err)
      }
    },
  })

  // 环境自检：只读，不写任何文件
  routes.push({
    method: "GET",
    pattern: "/api/env",
    handler: async function (req, res) {
      const override = pythonOverrideNow()
      const python = detectPython(override)
      const compiler = detectCompiler()
      const numpy = detectNumpy(override)
      const notes = []
      if (!python) {
        notes.push("本机没有找到 Python 解释器，数据库轨与 AI 轨的 demo 跑不了")
      }
      if (!compiler) {
        notes.push("本机没有找到 g++，算法轨的 C++ 代码判不了")
      }
      if (!numpy.available) {
        notes.push("本机没有装 numpy，13 张 AI 卡与 clustering 卡暂时跑不了（装上 numpy 即可运行）")
      }
      if (notes.length === 0) notes.push("运行环境正常")
      ctx.sendJson(res, 200, {
        python: { available: !!python, version: python ? python.version : "" },
        compiler: { available: !!compiler, version: compiler ? compiler.version : "" },
        numpy: { available: !!numpy.available, version: numpy.version || "" },
        notes: notes,
      })
    },
  })

  return routes
}

module.exports = {
  buildUnitRunRoutes: buildUnitRunRoutes,
  SUPPORTED_LANGUAGES: SUPPORTED_LANGUAGES,
}
