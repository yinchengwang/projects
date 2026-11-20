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
 * 测试通过 STUDY_HUB_PYTHON 指定解释器时，loader 与 demo 实跑必须使用同一个解释器。
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

/**
 * 测试用哪个 Python 解释器。
 *
 * 与产品代码同一条通路：STUDY_HUB_PYTHON 指定了就用它（numpy 只装在 py -3 的机器上
 * 必须这样指定），没指定就走自动探测。loader 与 demo 实跑必须用同一个，
 * 否则会出现「loader 说缺 numpy、demo 其实跑得动」这种自相矛盾的结论。
 *
 * 没指定时会**找一个真的装了 numpy 的解释器**：本机常见情形是 PATH 上的 python 没装、
 * 而 py -3 装了。这里不替用户改环境变量，只在测试内部挑一个能跑的——
 * 一个都找不到才返回 null（那种情况才是「本机真的缺 numpy」，该 skip）。
 * @returns {string|null}
 */
function pythonOverride() {
  const raw = process.env.STUDY_HUB_PYTHON
  if (raw && String(raw).trim() !== "") return String(raw).trim()
  for (const candidate of ["py -3", "python3", "python"]) {
    if (detectNumpy(candidate).available) return candidate
  }
  return null
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
  const numpy = detectNumpy(pythonOverride())
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
  const numpy = detectNumpy(pythonOverride())
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

test("numpy 警告与「断言脚本在不在」无关（脚本写好了也照样要报缺 numpy）", function () {
  // 实际缺陷：numpy 警告原来嵌在 `if (options.unitsDir) { if (!fs.existsSync(script)) { ... } }`
  // 里面，于是断言脚本一写好就再也不报 numpy 缺失。这里用一个临时卡目录复现两种情形。
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "p2-numpy-warn-"))
  try {
    const cardDir = path.join(root, "scaffold", "grokking", "cnn")
    fs.mkdirSync(cardDir, { recursive: true })
    fs.writeFileSync(path.join(cardDir, "README.md"), "# 卷积神经网络\n\n讲解正文。\n", "utf8")
    fs.writeFileSync(path.join(cardDir, "main.py"), "print('ok')\n", "utf8")

    const unitsDir = path.join(root, "units")
    fs.mkdirSync(path.join(unitsDir, "ai"), { recursive: true })

    // 情形 1：脚本不存在
    let out = loaderB.parseCardDirectory(cardDir, {
      track: "ai",
      unitsDir: unitsDir,
      env: { numpy: false },
    })
    assert.ok(
      out.warnings.join("；").indexOf("numpy") !== -1,
      "脚本不存在时也要报缺 numpy：" + JSON.stringify(out.warnings)
    )

    // 情形 2：脚本存在（这才是真实生产形态——P2 起 4 张 DB 卡都有脚本了）
    fs.writeFileSync(path.join(unitsDir, "ai", "cnn.assert.py"), "exit(0)\n", "utf8")
    out = loaderB.parseCardDirectory(cardDir, {
      track: "ai",
      unitsDir: unitsDir,
      env: { numpy: false },
    })
    assert.ok(
      out.warnings.join("；").indexOf("numpy") !== -1,
      "脚本已经写好了，缺 numpy 的警告反而消失了：" + JSON.stringify(out.warnings)
    )
    assert.strictEqual(
      out.warnings.filter(function (w) { return w.indexOf("断言脚本还没写") !== -1 }).length,
      0,
      "脚本在的时候不该报「脚本还没写」"
    )
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("detectNumpy：结果可用且能清缓存", function () {
  const numpy = detectNumpy(pythonOverride())
  assert.strictEqual(typeof numpy.available, "boolean")
  assert.strictEqual(typeof numpy.version, "string")
  if (numpy.available) {
    assert.ok(numpy.version.length > 0, "有 numpy 时要给出版本号")
  }
})

/**
 * 已知跑不通的 AI 卡 demo 台账。
 *
 * **现在是空的**：2026-11-20 按契约新增的「G32 / G39 唯一例外」条款，
 * 修掉了原来这 3 张（`dimensionality_reduction` 的 f-string 没闭合、
 * `nlp` 的 `TextCNN.num_filters`、`rnn` 的 `randn(1, 8)` 解包），全部只改代码缺陷、
 * 一个讲解文字都没动。修订在 learning 仓的提交里逐条列出了原文与改后。
 *
 * 将来再发现坏 demo：先按契约同款流程在契约里登记，再修，修完再清空这里——
 * 这个表的语义是「已知且容忍的坏」，不是「懒得修的清单」。
 */
const KNOWN_BROKEN_DEMOS = {}

test("AI 卡的 demo 实跑：没有 numpy 时明确 skip 并写明原因", function (t) {
  const learningRoot = resolveLearningRoot()
  if (!learningRoot) {
    skipNoLearning(t, "AI 卡实跑验收")
    return
  }
  const py = detectPython(pythonOverride())
  if (!py) {
    t.skip("本机没有可用的 Python 解释器（试过 python3 / python / py -3），跳过 AI 卡实跑验收")
    return
  }
  const numpy = detectNumpy(pythonOverride())
  if (!numpy.available) {
    t.skip(
      "自动探测到的 Python（" +
        py.cmd +
        " " +
        py.args.join(" ") +
        "）没有 numpy，按契约 §14.1 明确跳过，不静默跳过。" +
        "若本机别的解释器装了 numpy（比如 py -3），用 STUDY_HUB_PYTHON 指定它即可跑起来"
    )
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

test("13 张 AI 卡的 demo 都能实跑（与 KNOWN_BROKEN_DEMOS 台账一致）", function (t) {
  const learningRoot = resolveLearningRoot()
  if (!learningRoot) {
    skipNoLearning(t, "AI 卡全量实跑验收")
    return
  }
  const py = detectPython(pythonOverride())
  if (!py) {
    t.skip("本机没有可用的 Python 解释器，跳过 AI 卡全量实跑验收")
    return
  }
  const numpy = detectNumpy(pythonOverride())
  if (!numpy.available) {
    t.skip("本机没有装 numpy：13 张 AI 卡与 clustering 卡依赖 numpy，按契约 §14.1 明确跳过")
    return
  }

  const ok = []
  const broken = []
  for (const stem of AI_CARDS) {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "p2-ai-all-"))
    try {
      fs.cpSync(path.join(grokkingDir(learningRoot), stem), path.join(tmp, stem), {
        recursive: true,
      })
      const r = spawnSync(py.cmd, py.args.concat([path.join(tmp, stem, "main.py")]), {
        encoding: "utf8",
        env: Object.assign({}, process.env, { PYTHONIOENCODING: "utf-8" }),
        timeout: 180000,
      })
      if (r.status === 0) ok.push(stem)
      else broken.push(stem)
    } finally {
      fs.rmSync(tmp, { recursive: true, force: true })
    }
  }

  // 跑得通的那 10 张：一张都不许新坏
  const expectedOk = AI_CARDS.filter(function (s) {
    return KNOWN_BROKEN_DEMOS[s] === undefined
  })
  assert.deepStrictEqual(
    ok.slice().sort(),
    expectedOk.slice().sort(),
    "能跑的 AI 卡清单变了。新坏的：" +
      broken.filter(function (s) { return KNOWN_BROKEN_DEMOS[s] === undefined }).join("、") +
      "；已被修好的：" +
      expectedOk.filter(function (s) { return broken.indexOf(s) !== -1 }).join("、")
  )

  // 已知坏的 3 张：仍然坏着（谁修好了就把 KNOWN_BROKEN_DEMOS 里对应行删掉）
  const stillBroken = Object.keys(KNOWN_BROKEN_DEMOS).filter(function (s) {
    return broken.indexOf(s) !== -1
  })
  assert.deepStrictEqual(
    stillBroken.sort(),
    Object.keys(KNOWN_BROKEN_DEMOS).sort(),
    "已知会坏的 demo 清单不匹配了。修好了的（请从 KNOWN_BROKEN_DEMOS 删掉）：" +
      Object.keys(KNOWN_BROKEN_DEMOS)
        .filter(function (s) { return broken.indexOf(s) === -1 })
        .join("、")
  )
})

