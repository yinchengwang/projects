"use strict"
/**
 * db.js —— node:sqlite schema 与仓储函数
 *
 * 四张表照设计文档 §3.3 原样落：
 *   attempts（作答流水）/ review_cards（复习调度）/ topics（知识点）/ settings（配置）
 *
 * node:sqlite 只接受 null / number / bigint / string / Uint8Array，
 * 所以布尔一律转 0/1，undefined 一律转 null。
 */

const fs = require("node:fs")
const path = require("node:path")

const { DatabaseSync } = require("node:sqlite")

/**
 * 把 units 表的一行整理成上层好用的形状。
 * topics / subtracks 在库里存的是 JSON 数组字符串，取出来时解析回数组；
 * 解析失败当空数组——宁可少几个知识点，也不要让一条脏数据把整个接口带崩。
 * @param {object} row
 * @returns {object}
 */
function parseUnitRow(row) {
  return {
    id: String(row.id),
    track: String(row.track),
    kind: String(row.kind),
    title: String(row.title || ""),
    path: String(row.path || ""),
    status: String(row.status || "todo"),
    topics: parseJsonList(row.topics),
    difficulty: String(row.difficulty || "medium"),
    subtracks: parseJsonList(row.subtracks),
  }
}

/**
 * 解析一个 JSON 数组字符串；不是合法 JSON 就当空数组。
 * @param {*} text
 * @returns {string[]}
 */
function parseJsonList(text) {
  const raw = String(text === undefined || text === null ? "" : text)
  if (raw === "") return []
  try {
    const value = JSON.parse(raw)
    if (!Array.isArray(value)) return []
    return value.map(function (item) {
      return String(item)
    })
  } catch (e) {
    return []
  }
}

const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS attempts (
     id            INTEGER PRIMARY KEY,
     problem_id    TEXT NOT NULL,
     ts            TEXT NOT NULL,
     language      TEXT NOT NULL,
     code          TEXT NOT NULL,
     verdict       TEXT NOT NULL,
     passed_cases  INTEGER NOT NULL DEFAULT 0,
     total_cases   INTEGER NOT NULL DEFAULT 0,
     duration_ms   INTEGER NOT NULL DEFAULT 0,
     mode          TEXT NOT NULL DEFAULT 'solve',
     unit_id       TEXT,
     track         TEXT NOT NULL DEFAULT 'algorithm',
     assert_total  INTEGER NOT NULL DEFAULT 0,
     assert_passed INTEGER NOT NULL DEFAULT 0
   )`,
  `CREATE INDEX IF NOT EXISTS attempts_problem ON attempts(problem_id)`,
  `CREATE INDEX IF NOT EXISTS attempts_ts ON attempts(ts)`,
  `CREATE TABLE IF NOT EXISTS review_cards (
     problem_id     TEXT PRIMARY KEY,
     easiness       REAL NOT NULL DEFAULT 2.5,
     interval_days  INTEGER NOT NULL DEFAULT 1,
     repetitions    INTEGER NOT NULL DEFAULT 0,
     last_quality   INTEGER,
     last_at        TEXT,
     next_review_at TEXT NOT NULL,
     lapses         INTEGER NOT NULL DEFAULT 0,
     unit_id        TEXT,
     units_since    INTEGER NOT NULL DEFAULT 0
   )`,
  `CREATE INDEX IF NOT EXISTS review_cards_due ON review_cards(next_review_at)`,
  `CREATE TABLE IF NOT EXISTS topics (
     id         TEXT PRIMARY KEY,
     first_pass INTEGER NOT NULL DEFAULT 0,
     attempts   INTEGER NOT NULL DEFAULT 0,
     last_at    TEXT,
     lapses     INTEGER NOT NULL DEFAULT 0,
     track      TEXT NOT NULL DEFAULT 'algorithm'
   )`,
  `CREATE TABLE IF NOT EXISTS settings (
     key   TEXT PRIMARY KEY,
     value TEXT NOT NULL
   )`,
  // —— P0 新增：单元索引（物化视图，跨轨搜索与统计靠它，不每次扫盘）——
  `CREATE TABLE IF NOT EXISTS units (
     id         TEXT PRIMARY KEY,
     track      TEXT NOT NULL,
     kind       TEXT NOT NULL,
     title      TEXT NOT NULL DEFAULT '',
     path       TEXT NOT NULL DEFAULT '',
     status     TEXT NOT NULL DEFAULT 'todo',
     topics     TEXT NOT NULL DEFAULT '',
     difficulty TEXT NOT NULL DEFAULT 'medium',
     subtracks  TEXT NOT NULL DEFAULT '',
     updated    TEXT NOT NULL DEFAULT ''
   )`,
  `CREATE INDEX IF NOT EXISTS units_track ON units(track)`,
  `CREATE INDEX IF NOT EXISTS units_status ON units(status)`,
  // —— P0 新增：会话（按次数计，不按天）——
  `CREATE TABLE IF NOT EXISTS sessions (
     id            TEXT PRIMARY KEY,
     started_at    TEXT NOT NULL,
     ended_at      TEXT,
     planned_count INTEGER NOT NULL DEFAULT 0,
     done_count    INTEGER NOT NULL DEFAULT 0,
     tracks        TEXT NOT NULL DEFAULT ''
   )`,
  `CREATE INDEX IF NOT EXISTS sessions_started ON sessions(started_at)`,
  // —— P0 新增：会话队列（落库，中断不丢）——
  `CREATE TABLE IF NOT EXISTS session_queue (
     session_id TEXT NOT NULL,
     unit_id    TEXT NOT NULL,
     seq        INTEGER NOT NULL,
     state      TEXT NOT NULL DEFAULT 'pending',
     PRIMARY KEY (session_id, unit_id)
   )`,
  `CREATE INDEX IF NOT EXISTS session_queue_order ON session_queue(session_id, seq)`,
  // —— P0 新增：掌握度（三层聚合的派生值）——
  `CREATE TABLE IF NOT EXISTS mastery (
     scope      TEXT NOT NULL,
     scope_id   TEXT NOT NULL,
     level      REAL NOT NULL DEFAULT 0,
     updated_at TEXT NOT NULL DEFAULT '',
     PRIMARY KEY (scope, scope_id)
   )`,
  // —— P0 新增：单元学习进度 ——
  `CREATE TABLE IF NOT EXISTS unit_progress (
     unit_id     TEXT PRIMARY KEY,
     progress    TEXT NOT NULL DEFAULT 'todo',
     last_at     TEXT NOT NULL DEFAULT '',
     updated_at  TEXT NOT NULL DEFAULT ''
   )`,
]

/**
 * P0 要加的列。纯加法：只加，不改既有列，不动既有数据。
 * NOT NULL 的列必须带 DEFAULT，否则老库 ALTER 会失败。
 */
const ADD_COLUMNS = [
  { table: "attempts", column: "unit_id", type: "TEXT" },
  { table: "attempts", column: "track", type: "TEXT NOT NULL DEFAULT 'algorithm'" },
  { table: "attempts", column: "assert_total", type: "INTEGER NOT NULL DEFAULT 0" },
  { table: "attempts", column: "assert_passed", type: "INTEGER NOT NULL DEFAULT 0" },
  { table: "review_cards", column: "unit_id", type: "TEXT" },
  { table: "review_cards", column: "units_since", type: "INTEGER NOT NULL DEFAULT 0" },
  { table: "topics", column: "track", type: "TEXT NOT NULL DEFAULT 'algorithm'" },
]

/**
 * 迁移：给老库补上缺的列。
 *
 * 幂等：同一列第二次跑会被跳过，所以 openDb 对同一个文件连开两次结果一致。
 * **P0 阶段一条数据都不改**——数据迁移留给 P1（attempts/review_cards 的 unit_id）
 * 与 P3（topics 的复合键化）。
 *
 * @param {import("node:sqlite").DatabaseSync} db
 * @returns {{applied: string[], warnings: string[]}}
 */
function migrate(db) {
  const applied = []
  const warnings = []
  ADD_COLUMNS.forEach(function (item) {
    let info = []
    try {
      info = db.prepare("PRAGMA table_info(" + item.table + ")").all()
    } catch (err) {
      warnings.push("读不到表结构：" + item.table)
      return
    }
    const exists = info.some(function (row) {
      return String(row.name) === item.column
    })
    if (exists) return
    const stmt = "ALTER TABLE " + item.table + " ADD COLUMN " + item.column + " " + item.type
    try {
      db.exec(stmt)
      applied.push(stmt)
    } catch (err) {
      warnings.push("加列失败：" + item.table + "." + item.column + "：" + String(err && err.message ? err.message : err))
    }
  })

  // P3：知识点复合键化。三条都幂等（跑第二遍 WHERE 条件一条都不匹配）。
  // 第一条把裸知识点名并入算法轨——历史数据全是算法轨，复合键化后 id 形如 algorithm:数组。
  // 第二条补齐 P0 加的 track 列（正常情况下默认值就是 'algorithm'，这条是兜底）。
  // 第三条把知识点级掌握度一并复合键化——mastery.scope='topic' 的 scope_id 与 topics.id 同形，
  // 不一起改就会出现两套键。
  //
  // 只有**真的改了行**才记进 applied：这三条是 DML，不像 ALTER TABLE 那样能靠「列已存在」
  // 判断做过没做过。第二遍跑时 changes 全是 0，applied 里就什么都不多——`unit-db.test.js`
  // 靠这个断言幂等（「第二次什么都不做」）。
  //
  // `mastery` 表是 P0 的 SCHEMA 建的，migrate 本身不建表。真实调用点（openDb）里 SCHEMA
  // 先跑，所以表一定在；但单独拿一个 P0 之前的库调 migrate 时它不存在——那种情况静默跳过，
  // 不记 warning（「旧库还没这张表」不是迁移失败，建表是 SCHEMA 的活）。
  const tableExists = function (name) {
    try {
      return db.prepare("PRAGMA table_info(" + name + ")").all().length > 0
    } catch (err) {
      return false
    }
  }
  const p3Statements = [
    { table: "topics", sql: "UPDATE topics SET id = 'algorithm:' || id WHERE id NOT LIKE '%:%'" },
    { table: "topics", sql: "UPDATE topics SET track = 'algorithm' WHERE track IS NULL OR track = ''" },
    {
      table: "mastery",
      sql: "UPDATE mastery SET scope_id = 'algorithm:' || scope_id WHERE scope = 'topic' AND scope_id NOT LIKE '%:%'",
    },
  ]
  p3Statements.forEach(function (item) {
    if (!tableExists(item.table)) return
    try {
      const info = db.prepare(item.sql).run()
      const changes = Number(info && info.changes ? info.changes : 0)
      if (changes > 0) applied.push(item.sql + "（" + changes + " 行）")
    } catch (err) {
      warnings.push("P3 数据迁移失败：" + item.sql + "：" + String(err && err.message ? err.message : err))
    }
  })

  return { applied: applied, warnings: warnings }
}

/**
 * 打开数据库（不存在则建目录建库）并建表。
 * @param {string} filePath 文件路径；":memory:" 表示内存库（测试用）
 * @returns {import("node:sqlite").DatabaseSync}
 */
function openDb(filePath) {
  const target = filePath || ":memory:"
  if (target !== ":memory:") {
    fs.mkdirSync(path.dirname(target), { recursive: true })
  }
  const db = new DatabaseSync(target)
  SCHEMA.forEach(function (stmt) {
    db.exec(stmt)
  })
  // P0 的 migrate(db)：补列（列不存在就 ALTER TABLE ADD COLUMN）
  if (typeof migrate === "function") {
    migrate(db)
  }
  // P1 的 migrateUnitIds(db)：补列兜底 + 会话两张表 + 老行回填 unit_id
  migrateUnitIds(db)
  return db
}

/**
 * 会话行 → 会话对象（tracks 从 JSON 字符串还原成数组）。
 * @param {object|null} row
 * @returns {object|undefined}
 */
function toSession(row) {
  if (!row) return undefined
  let tracks = []
  try {
    const parsed = JSON.parse(String(row.tracks || "[]"))
    if (Array.isArray(parsed)) {
      tracks = parsed.map(function (t) {
        return String(t)
      })
    }
  } catch (err) {
    tracks = []
  }
  return {
    id: String(row.id),
    started_at: String(row.started_at),
    ended_at: row.ended_at === null || row.ended_at === undefined ? null : String(row.ended_at),
    planned_count: Number(row.planned_count || 0),
    done_count: Number(row.done_count || 0),
    tracks: tracks,
  }
}

/**
 * P1 的数据迁移（幂等）：
 *   1. 补齐 P0 该加的列（列不存在就 ALTER TABLE ADD COLUMN）
 *   2. 确保 sessions / session_queue 两张表在（DDL 抄自契约 §4.2）
 *   3. 给老行回填 unit_id（'algo:' || problem_id）
 *
 * 第 2 步是兜底：P0 的 SCHEMA 里已经有这两张表，CREATE TABLE IF NOT EXISTS 是幂等的，
 * 不会改动 P0 建出来的表；万一 P0 的 migrate 没跑到，P1 自己也活得下来。
 * @param {import("node:sqlite").DatabaseSync} db
 * @returns {{applied: string[], warnings: string[]}}
 */
function migrateUnitIds(db) {
  const applied = []
  const warnings = []

  const columnNames = function (table) {
    try {
      return db
        .prepare("PRAGMA table_info(" + table + ")")
        .all()
        .map(function (row) {
          return String(row.name)
        })
    } catch (err) {
      warnings.push("读不到 " + table + " 的列信息：" + String(err && err.message ? err.message : err))
      return []
    }
  }

  const ensureColumn = function (table, column, ddl) {
    if (columnNames(table).indexOf(column) !== -1) return
    db.exec(ddl)
    applied.push(ddl)
  }

  ensureColumn("attempts", "unit_id", "ALTER TABLE attempts ADD COLUMN unit_id TEXT")
  ensureColumn(
    "attempts",
    "track",
    "ALTER TABLE attempts ADD COLUMN track TEXT NOT NULL DEFAULT 'algorithm'"
  )
  ensureColumn(
    "attempts",
    "assert_total",
    "ALTER TABLE attempts ADD COLUMN assert_total INTEGER NOT NULL DEFAULT 0"
  )
  ensureColumn(
    "attempts",
    "assert_passed",
    "ALTER TABLE attempts ADD COLUMN assert_passed INTEGER NOT NULL DEFAULT 0"
  )
  ensureColumn("review_cards", "unit_id", "ALTER TABLE review_cards ADD COLUMN unit_id TEXT")
  ensureColumn(
    "review_cards",
    "units_since",
    "ALTER TABLE review_cards ADD COLUMN units_since INTEGER NOT NULL DEFAULT 0"
  )

  const sessionTables = [
    `CREATE TABLE IF NOT EXISTS sessions (
       id            TEXT PRIMARY KEY,
       started_at    TEXT NOT NULL,
       ended_at      TEXT,
       planned_count INTEGER NOT NULL DEFAULT 0,
       done_count    INTEGER NOT NULL DEFAULT 0,
       tracks        TEXT NOT NULL DEFAULT ''
     )`,
    `CREATE TABLE IF NOT EXISTS session_queue (
       session_id TEXT NOT NULL,
       unit_id    TEXT NOT NULL,
       seq        INTEGER NOT NULL,
       state      TEXT NOT NULL DEFAULT 'pending',
       PRIMARY KEY (session_id, unit_id)
     )`,
  ]
  sessionTables.forEach(function (ddl) {
    db.exec(ddl)
  })

  const backfill = [
    "UPDATE attempts SET unit_id = 'algo:' || problem_id WHERE unit_id IS NULL AND problem_id NOT LIKE '%:%'",
    "UPDATE review_cards SET unit_id = 'algo:' || problem_id WHERE unit_id IS NULL AND problem_id NOT LIKE '%:%'",
  ]
  backfill.forEach(function (sql) {
    db.exec(sql)
    applied.push(sql)
  })

  return { applied: applied, warnings: warnings }
}

/**
 * 仓储层：把 SQL 收在一处，上层不写 SQL 字符串。
 * @param {import("node:sqlite").DatabaseSync} db
 */
function createStore(db) {
  return {
    close: function () {
      db.close()
    },

    /* ---------- settings ---------- */

    getSetting: function (key, fallback) {
      const row = db.prepare("SELECT value FROM settings WHERE key = ?").get(key)
      if (!row) return fallback === undefined ? null : fallback
      return row.value
    },

    setSetting: function (key, value) {
      db.prepare(
        "INSERT INTO settings(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value"
      ).run(key, String(value))
    },

    allSettings: function () {
      const rows = db.prepare("SELECT key, value FROM settings ORDER BY key").all()
      const out = {}
      rows.forEach(function (row) {
        out[row.key] = row.value
      })
      return out
    },

    /* ---------- attempts ---------- */

    insertAttempt: function (attempt) {
      const result = db
        .prepare(
          `INSERT INTO attempts(problem_id, unit_id, track, ts, language, code, verdict, passed_cases, total_cases, duration_ms, mode, assert_total, assert_passed)
           VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`
        )
        .run(
          String(attempt.problem_id),
          attempt.unit_id === undefined || attempt.unit_id === null ? null : String(attempt.unit_id),
          String(attempt.track || "algorithm"),
          String(attempt.ts),
          String(attempt.language),
          String(attempt.code || ""),
          String(attempt.verdict),
          Number(attempt.passed_cases || 0),
          Number(attempt.total_cases || 0),
          Number(attempt.duration_ms || 0),
          String(attempt.mode || "solve"),
          Number(attempt.assert_total || 0),
          Number(attempt.assert_passed || 0)
        )
      return Number(result.lastInsertRowid)
    },

    listAttemptsByProblem: function (problemId) {
      return db
        .prepare("SELECT * FROM attempts WHERE problem_id = ? ORDER BY ts DESC, id DESC")
        .all(String(problemId))
    },

    /**
     * 按单元 id 查流水（契约 §4.1：P2 起新增的别名）。
     *
     * 老行只有 `problem_id`（无前缀），P1 做过一次性回填
     * （`UPDATE attempts SET unit_id = 'algo:' || problem_id WHERE unit_id IS NULL AND problem_id NOT LIKE '%:%'`），
     * 所以正常库两边都查得到；为防漏网，这里把无前缀的同名题号也带上。
     * @param {string} unitId 形如 algo:lc-00007 / db:mvcc
     * @returns {object[]}
     */
    listAttemptsByUnit: function (unitId) {
      const id = String(unitId)
      const legacy = id.indexOf(":") === -1 ? id : id.slice(id.indexOf(":") + 1)
      return db
        .prepare(
          "SELECT * FROM attempts WHERE unit_id = ? OR (unit_id IS NULL AND problem_id = ?) " +
            "ORDER BY ts DESC, id DESC"
        )
        .all(id, legacy)
    },

    countAttempts: function () {
      const row = db.prepare("SELECT COUNT(*) AS n FROM attempts").get()
      return Number(row.n)
    },

    /**
     * 列出全部作答记录（按时间升序，第一条就是某题的最早一次尝试）。
     * 排程器推下一题时要算「首次通过率」，靠的就是这个顺序。
     * @returns {object[]}
     */
    listAttempts: function () {
      return db.prepare("SELECT * FROM attempts ORDER BY ts ASC, id ASC").all()
    },

    /**
     * 统计某个时刻之后的作答条数（今日页的进度条要用）。
     * attempts.ts 存的是 ISO 字符串（new Date().toISOString()），
     * 同一格式的字符串可以直接比大小，不用转成时间戳。
     * @param {string} sinceIso 起始时刻（ISO）
     * @returns {number}
     */
    countAttemptsSince: function (sinceIso) {
      const row = db
        .prepare("SELECT COUNT(*) AS n FROM attempts WHERE ts >= ?")
        .get(String(sinceIso || ""))
      return Number(row.n)
    },

    /* ---------- attempts（M5 追加：诊断用的只读方法） ---------- */

    /**
     * 列出全部作答的摘要（诊断统计用）。
     * **不带 code 列**——诊断只需要知道「哪题、什么时候、什么结果」，
     * 把源码全捞进内存没必要，几百条流水就是几百份源码。
     *
     * P1 起返回项**新增 `track` 与 `unit_id`**（契约 §4.3）：P3 的诊断页按 `unit_id`
     * 归单元，缺了它就只能退回 `problem_id`，而 `problem_id` 不带前缀 ——
     * 「db:mvcc」会被误归成算法轨的「algo:mvcc」。
     * @returns {Array<{id: number, problem_id: string, ts: string, verdict: string, track: string, unit_id: string|null}>}
     */
    listAttemptSummaries: function () {
      return db
        .prepare(
          "SELECT id, problem_id, ts, verdict, track, unit_id FROM attempts ORDER BY problem_id ASC, ts ASC, id ASC"
        )
        .all()
        .map(function (row) {
          // node:sqlite 返回的是 null 原型对象，转成普通对象方便比较与序列化
          return {
            id: Number(row.id),
            problem_id: String(row.problem_id),
            ts: String(row.ts),
            verdict: String(row.verdict),
            track: String(row.track || "algorithm"),
            unit_id: row.unit_id === null || row.unit_id === undefined ? null : String(row.unit_id),
          }
        })
    },

    /**
     * 按天统计提交次数（每日题量曲线用）。
     * ts 存的是 ISO 字符串，substr(ts,1,10) 就是 UTC 日期部分。
     * @returns {Array<{date: string, count: number}>}
     */
    countAttemptsByDay: function () {
      return db
        .prepare("SELECT substr(ts,1,10) AS date, COUNT(*) AS count FROM attempts GROUP BY date ORDER BY date ASC")
        .all()
        .map(function (row) {
          return { date: String(row.date), count: Number(row.count) }
        })
    },

    /* ---------- review_cards ---------- */

    upsertReviewCard: function (card) {
      // 缺省时保留旧值：老链路（/api/review/:id/grade）不传这两个字段，
      // 传 NULL 会把新链路的 unit_id 与 units_since 抹掉。
      const existing = db
        .prepare("SELECT unit_id, units_since FROM review_cards WHERE problem_id = ?")
        .get(String(card.problem_id))
      const unitId =
        card.unit_id === undefined || card.unit_id === null || String(card.unit_id) === ""
          ? existing && existing.unit_id
            ? String(existing.unit_id)
            : null
          : String(card.unit_id)
      const unitsSince =
        card.units_since === undefined || card.units_since === null
          ? existing
            ? Number(existing.units_since || 0)
            : 0
          : Math.max(0, Math.trunc(Number(card.units_since)))
      db.prepare(
        `INSERT INTO review_cards(problem_id, unit_id, easiness, interval_days, repetitions, last_quality, last_at, next_review_at, lapses, units_since)
         VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT(problem_id) DO UPDATE SET
           unit_id = excluded.unit_id,
           easiness = excluded.easiness,
           interval_days = excluded.interval_days,
           repetitions = excluded.repetitions,
           last_quality = excluded.last_quality,
           last_at = excluded.last_at,
           next_review_at = excluded.next_review_at,
           lapses = excluded.lapses,
           units_since = excluded.units_since`
      ).run(
        String(card.problem_id),
        unitId,
        Number(card.easiness),
        Number(card.interval_days),
        Number(card.repetitions),
        card.last_quality === null || card.last_quality === undefined ? null : Number(card.last_quality),
        card.last_at === null || card.last_at === undefined ? null : String(card.last_at),
        String(card.next_review_at),
        Number(card.lapses || 0),
        unitsSince
      )
    },

    getReviewCard: function (problemId) {
      return db.prepare("SELECT * FROM review_cards WHERE problem_id = ?").get(String(problemId))
    },

    listDueCards: function (nowIso) {
      return db
        .prepare("SELECT * FROM review_cards WHERE next_review_at <= ? ORDER BY next_review_at ASC")
        .all(String(nowIso))
    },

    /**
     * 列出全部复习卡（含 unit_id / units_since）。
     * 次数通道要靠它算到期集合，不能只按日期查。
     * @returns {object[]}
     */
    listReviewCards: function () {
      return db.prepare("SELECT * FROM review_cards ORDER BY problem_id ASC").all()
    },

    /**
     * 次数通道：units_since >= k 的卡片（日期没到也可能到期）。
     * @param {number} k 阈值 K
     * @returns {object[]}
     */
    listDueCardsByCount: function (k) {
      return db
        .prepare("SELECT * FROM review_cards WHERE units_since >= ? ORDER BY units_since DESC, problem_id ASC")
        .all(Number(k))
    },

    countReviewCards: function () {
      const row = db.prepare("SELECT COUNT(*) AS n FROM review_cards").get()
      return Number(row.n)
    },

    /* ---------- topics ---------- */

    upsertTopic: function (topic) {
      db.prepare(
        `INSERT INTO topics(id, track, first_pass, attempts, last_at, lapses)
         VALUES(?, ?, ?, ?, ?, ?)
         ON CONFLICT(id) DO UPDATE SET
           first_pass = topics.first_pass + excluded.first_pass,
           attempts = topics.attempts + excluded.attempts,
           last_at = excluded.last_at,
           lapses = topics.lapses + excluded.lapses`
      ).run(
        String(topic.id),
        String(topic.track || "algorithm"),
        Number(topic.first_pass || 0),
        Number(topic.attempts || 0),
        topic.last_at === null || topic.last_at === undefined ? null : String(topic.last_at),
        Number(topic.lapses || 0)
      )
    },

    listTopics: function () {
      return db.prepare("SELECT * FROM topics ORDER BY id ASC").all()
    },

    /**
     * 整体覆盖 topics 表（M5 追加，P3 起写复合键与轨）。
     *
     * M1 的 upsertTopic 是**累加**写法，M3/M4 可能还在用它往 topics 里写增量；
     * M5 每次算诊断都从原始流水重新派生，所以用这个整体覆盖把口径统一，
     * 两种写法不会互相污染——派生值永远是全量，不是增量。
     *
     * P3 起 id 是复合键 <track>:<topic>，并多写一列 track。
     *
     * @param {Array<{id: string, track?: string, first_pass: number, attempts: number, last_at: string|null, lapses: number}>} rows
     */
    replaceTopics: function (rows) {
      db.exec("DELETE FROM topics")
      const stmt = db.prepare(
        "INSERT INTO topics(id, track, first_pass, attempts, last_at, lapses) VALUES(?, ?, ?, ?, ?, ?)"
      )
      ;(rows || []).forEach(function (row) {
        stmt.run(
          String(row.id),
          String(row.track || "algorithm"),
          Number(row.first_pass || 0),
          Number(row.attempts || 0),
          row.last_at === null || row.last_at === undefined ? null : String(row.last_at),
          Number(row.lapses || 0)
        )
      })
    },

    /**
     * 整体覆盖 mastery 表（P3 新增）。
     *
     * 与 replaceTopics 同一个语义：物化视图是全量快照，不是增量。
     * 三层掌握度（unit / topic / track）一次写全，重复调用不累加。
     *
     * @param {Array<{scope: "unit"|"topic"|"track", scope_id: string, level: number, updated_at: string}>} rows
     */
    replaceMastery: function (rows) {
      db.exec("DELETE FROM mastery")
      const stmt = db.prepare(
        "INSERT INTO mastery(scope, scope_id, level, updated_at) VALUES(?, ?, ?, ?)"
      )
      ;(rows || []).forEach(function (row) {
        stmt.run(
          String(row.scope),
          String(row.scope_id),
          Number(row.level || 0),
          String(row.updated_at || "")
        )
      })
    },

    countTopics: function () {
      const row = db.prepare("SELECT COUNT(*) AS n FROM topics").get()
      return Number(row.n)
    },

    /* ---------- units（单元索引，P0 新增） ---------- */

    upsertUnit: function (unit) {
      db.prepare(
        `INSERT INTO units(id, track, kind, title, path, status, topics, difficulty, subtracks, updated)
         VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT(id) DO UPDATE SET
           track = excluded.track,
           kind = excluded.kind,
           title = excluded.title,
           path = excluded.path,
           status = excluded.status,
           topics = excluded.topics,
           difficulty = excluded.difficulty,
           subtracks = excluded.subtracks,
           updated = excluded.updated`
      ).run(
        String(unit.id),
        String(unit.track),
        String(unit.kind),
        String(unit.title || ""),
        String(unit.path || ""),
        String(unit.status || "todo"),
        String(unit.topics || ""),
        String(unit.difficulty || "medium"),
        String(unit.subtracks || ""),
        String(unit.updated || "")
      )
    },

    /**
     * 整体覆盖 units 表（物化视图语义，与 replaceTopics 一致）。
     * 派生值永远是全量，不是增量。
     * @param {Array<{id: string, track: string, kind: string, title: string, path: string, status: string, topics: string, difficulty: string, subtracks: string, updated: string}>} rows
     */
    replaceUnits: function (rows) {
      db.exec("DELETE FROM units")
      const stmt = db.prepare(
        "INSERT INTO units(id, track, kind, title, path, status, topics, difficulty, subtracks, updated) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
      )
      ;(rows || []).forEach(function (row) {
        stmt.run(
          String(row.id),
          String(row.track),
          String(row.kind),
          String(row.title || ""),
          String(row.path || ""),
          String(row.status || "todo"),
          String(row.topics || ""),
          String(row.difficulty || "medium"),
          String(row.subtracks || ""),
          String(row.updated || "")
        )
      })
    },

    /**
     * 列出单元。filter 三个字段都可省。
     * 返回的 topics / subtracks 已经从 JSON 字符串解析回数组。
     * @param {{track?: string, status?: string, kind?: string}} [filter]
     * @returns {object[]}
     */
    listUnits: function (filter) {
      const f = filter || {}
      const where = []
      const args = []
      if (f.track) {
        where.push("track = ?")
        args.push(String(f.track))
      }
      if (f.status) {
        where.push("status = ?")
        args.push(String(f.status))
      }
      if (f.kind) {
        where.push("kind = ?")
        args.push(String(f.kind))
      }
      const sql =
        "SELECT * FROM units" +
        (where.length > 0 ? " WHERE " + where.join(" AND ") : "") +
        " ORDER BY id ASC"
      return db
        .prepare(sql)
        .all(...args)
        .map(parseUnitRow)
    },

    getUnit: function (id) {
      const row = db.prepare("SELECT * FROM units WHERE id = ?").get(String(id))
      return row === undefined ? undefined : parseUnitRow(row)
    },

    countUnits: function () {
      const row = db.prepare("SELECT COUNT(*) AS n FROM units").get()
      return Number(row.n)
    },

    /* ---------- sessions ---------- */

    insertSession: function (s) {
      db.prepare(
        `INSERT INTO sessions(id, started_at, ended_at, planned_count, done_count, tracks)
         VALUES(?, ?, ?, ?, ?, ?)`
      ).run(
        String(s.id),
        String(s.started_at),
        s.ended_at === undefined || s.ended_at === null ? null : String(s.ended_at),
        Number(s.planned_count || 0),
        Number(s.done_count || 0),
        JSON.stringify(Array.isArray(s.tracks) ? s.tracks : [])
      )
    },

    updateSession: function (id, patch) {
      const p = patch || {}
      const sets = []
      const args = []
      if (p.ended_at !== undefined) {
        sets.push("ended_at = ?")
        args.push(p.ended_at === null ? null : String(p.ended_at))
      }
      if (p.planned_count !== undefined) {
        sets.push("planned_count = ?")
        args.push(Number(p.planned_count))
      }
      if (p.done_count !== undefined) {
        sets.push("done_count = ?")
        args.push(Number(p.done_count))
      }
      if (p.tracks !== undefined) {
        sets.push("tracks = ?")
        args.push(JSON.stringify(Array.isArray(p.tracks) ? p.tracks : []))
      }
      if (sets.length === 0) return
      args.push(String(id))
      db.prepare("UPDATE sessions SET " + sets.join(", ") + " WHERE id = ?").run(...args)
    },

    getSession: function (id) {
      return toSession(db.prepare("SELECT * FROM sessions WHERE id = ?").get(String(id)))
    },

    listSessions: function (limit) {
      const n = limit === undefined || limit === null ? 50 : Math.max(0, Math.trunc(Number(limit)))
      return db
        .prepare("SELECT * FROM sessions ORDER BY started_at DESC LIMIT ?")
        .all(n)
        .map(toSession)
    },

    getLatestOpenSession: function () {
      const row = db
        .prepare("SELECT * FROM sessions WHERE ended_at IS NULL ORDER BY started_at DESC LIMIT 1")
        .get()
      return toSession(row)
    },

    /* ---------- session_queue ---------- */

    replaceQueue: function (sessionId, items) {
      const sid = String(sessionId)
      db.prepare("DELETE FROM session_queue WHERE session_id = ?").run(sid)
      const stmt = db.prepare(
        "INSERT INTO session_queue(session_id, unit_id, seq, state) VALUES(?, ?, ?, ?)"
      )
      ;(items || []).forEach(function (item) {
        stmt.run(sid, String(item.unit_id), Number(item.seq), String(item.state || "pending"))
      })
    },

    listQueue: function (sessionId) {
      return db
        .prepare("SELECT * FROM session_queue WHERE session_id = ? ORDER BY seq ASC")
        .all(String(sessionId))
        .map(function (row) {
          return {
            session_id: String(row.session_id),
            unit_id: String(row.unit_id),
            seq: Number(row.seq),
            state: String(row.state),
          }
        })
    },

    updateQueueItem: function (sessionId, unitId, patch) {
      const p = patch || {}
      const sets = []
      const args = []
      if (p.state !== undefined) {
        sets.push("state = ?")
        args.push(String(p.state))
      }
      if (p.seq !== undefined) {
        sets.push("seq = ?")
        args.push(Number(p.seq))
      }
      if (sets.length === 0) return
      args.push(String(sessionId), String(unitId))
      db.prepare(
        "UPDATE session_queue SET " + sets.join(", ") + " WHERE session_id = ? AND unit_id = ?"
      ).run(...args)
    },

    countQueueDone: function (sessionId) {
      const row = db
        .prepare("SELECT COUNT(*) AS n FROM session_queue WHERE session_id = ? AND state = 'done'")
        .get(String(sessionId))
      return Number(row.n)
    },

    /* ---------- mastery（P0 新增） ---------- */

    upsertMastery: function (scope, scopeId, level, updatedAt) {
      db.prepare(
        `INSERT INTO mastery(scope, scope_id, level, updated_at) VALUES(?, ?, ?, ?)
         ON CONFLICT(scope, scope_id) DO UPDATE SET
           level = excluded.level,
           updated_at = excluded.updated_at`
      ).run(
        String(scope),
        String(scopeId),
        Number(level || 0),
        String(updatedAt || "")
      )
    },

    listMastery: function (scope) {
      if (scope === undefined || scope === null) {
        return db.prepare("SELECT * FROM mastery ORDER BY scope ASC, scope_id ASC").all()
      }
      return db
        .prepare("SELECT * FROM mastery WHERE scope = ? ORDER BY scope_id ASC")
        .all(String(scope))
    },

    /* ---------- unit_progress（P0 新增） ---------- */

    upsertUnitProgress: function (unitId, progress, lastAt) {
      const now = new Date().toISOString()
      db.prepare(
        `INSERT INTO unit_progress(unit_id, progress, last_at, updated_at) VALUES(?, ?, ?, ?)
         ON CONFLICT(unit_id) DO UPDATE SET
           progress = excluded.progress,
           last_at = excluded.last_at,
           updated_at = excluded.updated_at`
      ).run(
        String(unitId),
        String(progress || "todo"),
        lastAt === undefined || lastAt === null ? "" : String(lastAt),
        now
      )
    },

    listUnitProgress: function () {
      return db.prepare("SELECT * FROM unit_progress ORDER BY unit_id ASC").all()
    },
  }
}

module.exports = {
  SCHEMA: SCHEMA,
  ADD_COLUMNS: ADD_COLUMNS,
  openDb: openDb,
  createStore: createStore,
  migrate: migrate,
  migrateUnitIds: migrateUnitIds,
}
