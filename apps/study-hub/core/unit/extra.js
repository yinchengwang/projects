"use strict"
/**
 * extra.js —— unit.extra：附加元数据的形状与归一
 *
 * 为什么要有这个嵌套袋：契约 §1.2 的 17 个 Unit 字段是**冻结**的
 * （三个 loader 共用，createUnit 校验，多处测试钉住形状），加顶层字段要动
 * createUnit、字段表与所有断言 Unit 形状的测试。
 *
 * 契约 §3.3.1 已给过先例：算法轨的 negative: / source_id: / url: 「都不进 Unit」，
 * 由消费方自己再读一遍 frontmatter。这里换个更省的做法——loader C 读一次 frontmatter
 * 就把 4 个字段装进 unit.extra，下游不必重读文件。
 *
 * 四个键**恒存在**（loader A/B 是全缺省），形状不随来源漂移。
 */

/** extra 的四个键，顺序固定 */
const EXTRA_KEYS = ["runs_local", "needs", "fallback", "timeout_s"]

/** runs_local 的取值域：yes = 本机真跑得起来；partial = 只能玩具化；no = 跑不了 */
const RUNS_LOCAL_VALUES = ["yes", "partial", "no"]

/** needs 的取值域：缺什么 */
const NEEDS_VALUES = ["gpu-count", "vram", "nvcc", "fp8-train", "ram"]

/** 断言脚本默认预算（秒）——与 judge/runner.js 的 SCRIPT_TIMEOUT_MS 对齐 */
const DEFAULT_TIMEOUT_S = 30

/** 断言脚本预算上限（秒）——与 judge/sandbox.js 的 MAX_SCRIPT_TIMEOUT_MS 对齐 */
const MAX_TIMEOUT_S = 600

/**
 * 把一组原始值归一成 extra。
 * 非法值一律落缺省，**不抛错**：一个单元文件的脏字段不该让整个索引加载失败。
 * @param {object} input
 * @returns {{runs_local: string, needs: string[], fallback: string, timeout_s: number}}
 */
function makeExtra(input) {
  const src = input && typeof input === "object" ? input : {}

  // runs_local：非法值落 "no"。宁可把能跑的误标成跑不了（用户会自己发现），
  // 也不能把跑不了的误标成能跑——后者会让人卡在一个永远跑不通的实验上。
  const rawRuns = String(src.runs_local === undefined || src.runs_local === null ? "" : src.runs_local).trim()
  const runs_local = RUNS_LOCAL_VALUES.indexOf(rawRuns) !== -1 ? rawRuns : "no"

  const rawNeeds = Array.isArray(src.needs) ? src.needs : []
  const needs = []
  rawNeeds.forEach(function (item) {
    const text = String(item === undefined || item === null ? "" : item).trim()
    if (text === "") return
    if (NEEDS_VALUES.indexOf(text) === -1) return
    if (needs.indexOf(text) !== -1) return
    needs.push(text)
  })

  const fallback = String(src.fallback === undefined || src.fallback === null ? "" : src.fallback).trim()

  const rawTimeout = Number(src.timeout_s)
  let timeout_s = DEFAULT_TIMEOUT_S
  if (Number.isFinite(rawTimeout) && rawTimeout > 0) {
    timeout_s = Math.min(Math.floor(rawTimeout), MAX_TIMEOUT_S)
  }

  return {
    runs_local: runs_local,
    needs: needs,
    fallback: fallback,
    timeout_s: timeout_s,
  }
}

/**
 * 取一个单元的 extra；没有就造一份全缺省的。
 * @param {object} unit
 * @returns {{runs_local: string, needs: string[], fallback: string, timeout_s: number}}
 */
function normalizeExtra(unit) {
  const src = unit && typeof unit === "object" ? unit : {}
  return makeExtra(src.extra)
}

module.exports = {
  EXTRA_KEYS: EXTRA_KEYS,
  RUNS_LOCAL_VALUES: RUNS_LOCAL_VALUES,
  NEEDS_VALUES: NEEDS_VALUES,
  DEFAULT_TIMEOUT_S: DEFAULT_TIMEOUT_S,
  MAX_TIMEOUT_S: MAX_TIMEOUT_S,
  makeExtra: makeExtra,
  normalizeExtra: normalizeExtra,
}
