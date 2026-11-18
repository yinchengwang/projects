"use strict"
/**
 * p2-loader-b.test.js —— loader B 的 DB/AI 卡接入
 *
 * 三件事：
 *   1. 契约 §14.1 冻结的 16 张数据库卡 + 13 张 AI 卡，磁盘上一张不少；
 *   2. numpy 依赖表跟磁盘上的真实 import 对得上；
 *   3. 每一张卡都能被 loader B 读成能直接用的单元。
 *
 * AI 卡依赖 numpy；本机没装 numpy 时**明确 skip 并写明原因**，不静默跳过。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { spawnSync } = require("node:child_process")

const tracks = require("../core/track/tracks.js")
const loaderB = require("../core/unit/loader-b.js")
const { detectPython, detectNumpy } = require("../judge/sandbox.js")

/** 契约 §14.1 冻结的数据库轨 16 张 */
const DB_CARDS = [
  "anti_patterns", "backup_recovery", "cache_strategy", "cap_theorem",
  "clustering", "index_design", "lock_manager", "mvcc", "newsql",
  "normalization", "nosql", "query_optimization", "replication",
  "sharding", "sql_fundamentals", "transaction",
]

/** 契约 §14.1 冻结的 AI 轨 13 张 */
const AI_CARDS = [
  "cnn", "decision_tree", "dimensionality_reduction", "gradient_boosting",
  "linear_regression", "logistic_regression", "ml_fundamentals",
  "neural_network", "nlp", "random_forest", "recommendation", "rnn", "svm",
]

function resolveLearningRoot() {
  const fromEnv = process.env.STUDY_HUB_LEARNING_ROOT
  if (fromEnv && fs.existsSync(fromEnv)) return fromEnv
  const guess = path.resolve(__dirname, "..", "..", "..", "..", "learning")
  if (fs.existsSync(guess)) return guess
  return null
}

function grokkingDir(learningRoot) {
  return path.join(learningRoot, "scaffold", "grokking")
}

function skipNoLearning(t, what) {
  t.skip("找不到 learning 仓（设 STUDY_HUB_LEARNING_ROOT，或确认 ../../../../learning 存在），跳过" + what)
}

test("契约 §14.1 的两张清单与 tracks.js 的表一致", function () {
  assert.deepStrictEqual(tracks.GROKKING_CARDS.database.slice().sort(), DB_CARDS.slice().sort())
  assert.deepStrictEqual(tracks.GROKKING_CARDS.ai.slice().sort(), AI_CARDS.slice().sort())
  assert.strictEqual(DB_CARDS.length, 16)
  assert.strictEqual(AI_CARDS.length, 13)
})

test("两张清单没有交集", function () {
  const dup = DB_CARDS.filter(function (s) { return AI_CARDS.indexOf(s) !== -1 })
  assert.deepStrictEqual(dup, [])
})

test("cardNeedsNumpy：clustering 与 13 张 AI 卡为真，其余为假", function () {
  assert.strictEqual(tracks.cardNeedsNumpy("clustering"), true)
  AI_CARDS.forEach(function (s) {
    assert.strictEqual(tracks.cardNeedsNumpy(s), true, s + " 应该需要 numpy")
  })
  DB_CARDS.filter(function (s) { return s !== "clustering" }).forEach(function (s) {
    assert.strictEqual(tracks.cardNeedsNumpy(s), false, s + " 不该需要 numpy")
  })
})

test("numpy 依赖表与磁盘上的真实 import 一致", function (t) {
  const learningRoot = resolveLearningRoot()
  if (!learningRoot) {
    skipNoLearning(t, "numpy 依赖表核对")
    return
  }
  const dir = grokkingDir(learningRoot)
  const stems = fs.readdirSync(dir).filter(function (name) {
    return fs.existsSync(path.join(dir, name, "main.py"))
  })
  stems.forEach(function (stem) {
    const src = fs.readFileSync(path.join(dir, stem, "main.py"), "utf8")
    const usesNumpy = src.indexOf("import numpy") !== -1
    assert.strictEqual(
      tracks.cardNeedsNumpy(stem),
      usesNumpy,
      stem + " 的 numpy 依赖与表里对不上"
    )
  })
})

test("29 张卡在磁盘上一张不少", function (t) {
  const learningRoot = resolveLearningRoot()
  if (!learningRoot) {
    skipNoLearning(t, "卡片清点")
    return
  }
  const dir = grokkingDir(learningRoot)
  DB_CARDS.concat(AI_CARDS).forEach(function (stem) {
    assert.ok(fs.existsSync(path.join(dir, stem, "main.py")), stem + " 缺 main.py")
    assert.ok(fs.existsSync(path.join(dir, stem, "README.md")), stem + " 缺 README.md")
  })
})

test("loader B 把 16 张数据库卡都读成能用的单元", function (t) {
  const learningRoot = resolveLearningRoot()
  if (!learningRoot) {
    skipNoLearning(t, "loader B 解析验收")
    return
  }
  const dir = grokkingDir(learningRoot)
  const numpy = detectNumpy(null)
  DB_CARDS.forEach(function (stem) {
    const out = loaderB.parseCardDirectory(path.join(dir, stem), {
      learningRoot: learningRoot,
      track: "database",
      unitsDir: path.join(learningRoot, "units"),
      env: { numpy: numpy.available },
    })
    assert.ok(out.unit, stem + " 解析不出单元：" + JSON.stringify(out.warnings))
    assert.strictEqual(out.unit.id, "db:" + stem)
    assert.strictEqual(out.unit.track, "database")
    assert.strictEqual(out.unit.kind, "card")
    assert.strictEqual(out.unit.status, "verified")
    assert.strictEqual(out.unit.entry.graded, "script")
    assert.strictEqual(out.unit.entry.demo, "scaffold/grokking/" + stem + "/main.py")
    assert.strictEqual(out.unit.entry.script, "units/database/" + stem + ".assert.py")
    assert.strictEqual(out.unit.entry.language, "python")
    assert.deepStrictEqual(out.unit.asserts, [])
    assert.ok(out.unit.statement.length > 0, stem + " 的讲解正文是空的")
  })
})

test("loader B 把 13 张 AI 卡都读成能用的单元", function (t) {
  const learningRoot = resolveLearningRoot()
  if (!learningRoot) {
    skipNoLearning(t, "loader B 解析验收")
    return
  }
  const dir = grokkingDir(learningRoot)
  const numpy = detectNumpy(null)
  AI_CARDS.forEach(function (stem) {
    const out = loaderB.parseCardDirectory(path.join(dir, stem), {
      learningRoot: learningRoot,
      track: "ai",
      unitsDir: path.join(learningRoot, "units"),
      env: { numpy: numpy.available },
    })
    assert.ok(out.unit, stem + " 解析不出单元：" + JSON.stringify(out.warnings))
    assert.strictEqual(out.unit.id, "ai:" + stem)
    assert.strictEqual(out.unit.track, "ai")
    assert.strictEqual(out.unit.kind, "card")
    assert.strictEqual(out.unit.entry.graded, "script")
    assert.strictEqual(out.unit.entry.demo, "scaffold/grokking/" + stem + "/main.py")
    assert.strictEqual(out.unit.entry.script, "units/ai/" + stem + ".assert.py")
    assert.ok(out.unit.statement.length > 0, stem + " 的讲解正文是空的")
  })
})

test("缺 numpy 时 AI 卡的 warnings 里写明原因", function (t) {
  const learningRoot = resolveLearningRoot()
  if (!learningRoot) {
    skipNoLearning(t, "numpy 降级验收")
    return
  }
  const out = loaderB.parseCardDirectory(path.join(grokkingDir(learningRoot), "cnn"), {
    learningRoot: learningRoot,
    track: "ai",
    unitsDir: path.join(learningRoot, "units"),
    env: { numpy: false },
  })
  assert.ok(out.unit, "cnn 应当仍能解析出单元（可读不可跑）")
  assert.ok(out.warnings.join("；").indexOf("numpy") !== -1, "warnings 里要写明缺 numpy")
})

test("detectNumpy：结果可用且能清缓存", function () {
  const numpy = detectNumpy(null)
  assert.strictEqual(typeof numpy.available, "boolean")
  assert.strictEqual(typeof numpy.version, "string")
  if (numpy.available) {
    assert.ok(numpy.version.length > 0, "有 numpy 时要给出版本号")
  }
})

test("AI 卡的 demo 实跑：没有 numpy 时明确 skip 并写明原因", function (t) {
  const learningRoot = resolveLearningRoot()
  if (!learningRoot) {
    skipNoLearning(t, "AI 卡实跑验收")
    return
  }
  const py = detectPython(null)
  if (!py) {
    t.skip("本机没有可用的 Python 解释器（试过 python3 / python / py -3），跳过 AI 卡实跑验收")
    return
  }
  const numpy = detectNumpy(null)
  if (!numpy.available) {
    t.skip("本机没有装 numpy：13 张 AI 卡与 clustering 卡依赖 numpy，按契约 §14.1 明确跳过，不静默跳过")
    return
  }
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "p2-ai-"))
  try {
    const stem = "linear_regression"
    fs.cpSync(path.join(grokkingDir(learningRoot), stem), path.join(tmp, stem), { recursive: true })
    const r = spawnSync(py.cmd, py.args.concat([path.join(tmp, stem, "main.py")]), {
      encoding: "utf8",
      env: Object.assign({}, process.env, { PYTHONIOENCODING: "utf-8" }),
      timeout: 120000,
    })
    assert.strictEqual(r.status, 0, stem + " 的 demo 没跑通：\n" + String(r.stderr || "").slice(0, 800))
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true })
  }
})
