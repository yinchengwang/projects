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
     mode          TEXT NOT NULL DEFAULT 'solve'
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
     lapses         INTEGER NOT NULL DEFAULT 0
   )`,
  `CREATE INDEX IF NOT EXISTS review_cards_due ON review_cards(next_review_at)`,
  `CREATE TABLE IF NOT EXISTS topics (
     id         TEXT PRIMARY KEY,
     first_pass INTEGER NOT NULL DEFAULT 0,
     attempts   INTEGER NOT NULL DEFAULT 0,
     last_at    TEXT,
     lapses     INTEGER NOT NULL DEFAULT 0
   )`,
  `CREATE TABLE IF NOT EXISTS settings (
     key   TEXT PRIMARY KEY,
     value TEXT NOT NULL
   )`,
]

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
  return db
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
          `INSERT INTO attempts(problem_id, ts, language, code, verdict, passed_cases, total_cases, duration_ms, mode)
           VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)`
        )
        .run(
          String(attempt.problem_id),
          String(attempt.ts),
          String(attempt.language),
          String(attempt.code || ""),
          String(attempt.verdict),
          Number(attempt.passed_cases || 0),
          Number(attempt.total_cases || 0),
          Number(attempt.duration_ms || 0),
          String(attempt.mode || "solve")
        )
      return Number(result.lastInsertRowid)
    },

    listAttemptsByProblem: function (problemId) {
      return db
        .prepare("SELECT * FROM attempts WHERE problem_id = ? ORDER BY ts DESC, id DESC")
        .all(String(problemId))
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

    /* ---------- review_cards ---------- */

    upsertReviewCard: function (card) {
      db.prepare(
        `INSERT INTO review_cards(problem_id, easiness, interval_days, repetitions, last_quality, last_at, next_review_at, lapses)
         VALUES(?, ?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT(problem_id) DO UPDATE SET
           easiness = excluded.easiness,
           interval_days = excluded.interval_days,
           repetitions = excluded.repetitions,
           last_quality = excluded.last_quality,
           last_at = excluded.last_at,
           next_review_at = excluded.next_review_at,
           lapses = excluded.lapses`
      ).run(
        String(card.problem_id),
        Number(card.easiness),
        Number(card.interval_days),
        Number(card.repetitions),
        card.last_quality === null || card.last_quality === undefined ? null : Number(card.last_quality),
        card.last_at === null || card.last_at === undefined ? null : String(card.last_at),
        String(card.next_review_at),
        Number(card.lapses || 0)
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

    countReviewCards: function () {
      const row = db.prepare("SELECT COUNT(*) AS n FROM review_cards").get()
      return Number(row.n)
    },

    /* ---------- topics ---------- */

    upsertTopic: function (topic) {
      db.prepare(
        `INSERT INTO topics(id, first_pass, attempts, last_at, lapses)
         VALUES(?, ?, ?, ?, ?)
         ON CONFLICT(id) DO UPDATE SET
           first_pass = topics.first_pass + excluded.first_pass,
           attempts = topics.attempts + excluded.attempts,
           last_at = excluded.last_at,
           lapses = topics.lapses + excluded.lapses`
      ).run(
        String(topic.id),
        Number(topic.first_pass || 0),
        Number(topic.attempts || 0),
        topic.last_at === null || topic.last_at === undefined ? null : String(topic.last_at),
        Number(topic.lapses || 0)
      )
    },

    listTopics: function () {
      return db.prepare("SELECT * FROM topics ORDER BY id ASC").all()
    },

    countTopics: function () {
      const row = db.prepare("SELECT COUNT(*) AS n FROM topics").get()
      return Number(row.n)
    },
  }
}

module.exports = { SCHEMA: SCHEMA, openDb: openDb, createStore: createStore }
