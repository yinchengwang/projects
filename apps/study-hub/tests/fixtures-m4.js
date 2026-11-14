"use strict"
/**
 * fixtures-m4.js —— M4 各测试共用的夹具（**不是测试套件**，run-all.js 只收 *.test.js）
 *
 * 这里放三样东西：
 *   1. 从 learning/progress 真实轮次文件里原样摘出来的表头与数据行（7 种变体）
 *   2. 一篇仿 r33 的轮次文件全文（写回测试用）
 *   3. 一个临时 learning 仓（带 git 仓库）的构造函数
 *
 * 写回相关的测试**只在临时副本上跑**，绝不碰真实的 learning 仓。
 */

const fs = require("node:fs")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const { tmpDir } = require("./helpers/tmp.js")

/**
 * 真实轮次文件的表头 + 首行数据（原样摘录）。
 * 勘察确认 learning/progress 下有 7 种表头变体：
 *   r25「ID | 卡片 | 主题 | 状态」
 *   r26「# | 卡片 ID | 主题 | 状态」
 *   r30「卡 ID | 名称 | 状态」
 *   r32「时间戳 | 卡 ID | commit SHA | scaffold 路径 | NOTES 路径 | 审核人」（**整张表没有「状态」列**）
 *   r33「序号 | ID | 名称 | 状态 | 完成时间」
 *   r35「序号 | ID | 名称 | 状态 | 完成时间 | commit SHA」
 *   r36/r37/r38「序号 | ID | 名称 | 语言 | 状态 | 完成时间 | commit SHA」
 */
const REAL_TABLES = [
  {
    name: "r25（ID 是序号，卡片才是标识）",
    header: "| ID | 卡片 | 主题 | 状态 |",
    row: "| 1 | epoll | epoll 事件驱动 | done |",
    expect: { id: "epoll", title: "epoll 事件驱动", status: "done", doneDate: "", commitSha: "" },
  },
  {
    name: "r26（# 是序号，卡片 ID 是标识）",
    header: "| # | 卡片 ID | 主题 | 状态 |",
    row: "| 1 | namespace | 网络命名空间(Netns) | done |",
    expect: { id: "namespace", title: "网络命名空间(Netns)", status: "done", doneDate: "", commitSha: "" },
  },
  {
    name: "r30（三列）",
    header: "| 卡 ID | 名称 | 状态 |",
    row: "| basic_types | 基础类型 | done |",
    expect: { id: "basic_types", title: "基础类型", status: "done", doneDate: "", commitSha: "" },
  },
  {
    name: "r32（整张表没有「状态」列）",
    header: "| 时间戳 | 卡 ID | commit SHA | scaffold 路径 | NOTES 路径 | 审核人 |",
    row: "| TBD-deferred | virtual_env | TBD-deferred | learning/scaffold/python/virtual_env/ | learning/scaffold/python/virtual_env/NOTES.md | yinch |",
    expect: { id: "virtual_env", title: "", status: "todo", doneDate: "", commitSha: "TBD-deferred" },
  },
  {
    name: "r33（五列）",
    header: "| 序号 | ID | 名称 | 状态 | 完成时间 |",
    row: "| 1 | big_o | 大 O 分析 | done | 2026-07-12 |",
    expect: { id: "big_o", title: "大 O 分析", status: "done", doneDate: "2026-07-12", commitSha: "" },
  },
  {
    name: "r35（六列）",
    header: "| 序号 | ID | 名称 | 状态 | 完成时间 | commit SHA |",
    row: "| 1 | monkey_testing | 混沌测试/故障注入 | done | 2026-07-12 | TBD-deferred |",
    expect: {
      id: "monkey_testing",
      title: "混沌测试/故障注入",
      status: "done",
      doneDate: "2026-07-12",
      commitSha: "TBD-deferred",
    },
  },
  {
    name: "r36（七列，含语言）",
    header: "| 序号 | ID | 名称 | 语言 | 状态 | 完成时间 | commit SHA |",
    row: "| 1 | thread_basics | 线程基础（pthread 创建/同步/竞态条件） | C | done | 2026-07-12 | TBD-deferred |",
    expect: {
      id: "thread_basics",
      title: "线程基础（pthread 创建/同步/竞态条件）",
      status: "done",
      doneDate: "2026-07-12",
      commitSha: "TBD-deferred",
    },
  },
]

/**
 * 一篇仿 r33 的轮次文件（表头与真实文件一致）。
 * 用 \n 拼；真实 learning 仓的文件是 CRLF，写回测试另有专门的 CRLF 用例。
 */
const ROUND_FILE = [
  "# R33 Grokking 算法篇 — 学习进度",
  "",
  "## 基本信息",
  "",
  "| 字段 | 值 |",
  "|------|-----|",
  "| 开始时间 | 2026-07-12 |",
  "| 目标 | Grokking 算法篇 16 张卡片 |",
  "| 状态 | TBD-deferred |",
  "",
  "## 16 张卡片清单",
  "",
  "| 序号 | ID | 名称 | 状态 | 完成时间 |",
  "|------|-----|------|------|----------|",
  "| 1 | big_o | 大 O 分析 | done | 2026-07-12 |",
  "| 2 | arrays | 数组技巧 | todo |  |",
  "| 3 | linked_lists | 链表技巧 | todo |  |",
  "",
].join("\n")

/** 一篇没有「完成时间」列的轮次文件（仿 r30） */
const ROUND_FILE_NO_DONE_DATE = [
  "# R30 Python 基础 — 进度追踪",
  "",
  "| 卡 ID | 名称 | 状态 |",
  "|-------|------|------|",
  "| basic_types | 基础类型 | todo |",
  "",
].join("\n")

/** 一道题的形状（M1 的 Problem） */
const PROBLEM = {
  id: "lc-00001",
  title: "两数之和",
  difficulty: "medium",
  topics: ["哈希表", "数组"],
  tracks: ["interview", "fundamentals"],
}

/**
 * 造一个只含清单表的轮次文件。
 * @param {string} header 表头行
 * @param {string[]} rows 数据行
 * @returns {string}
 */
function fileWithTable(header, rows) {
  return [
    "# R99 测试轮次 — 学习进度",
    "",
    "## 基本信息",
    "",
    "| 字段 | 值 |",
    "|------|-----|",
    "| 开始时间 | 2026-11-01 |",
    "| 目标 | 测试轮次 |",
    "",
    header,
    "|---|---|",
  ]
    .concat(rows)
    .join("\n")
}

/**
 * 造一个临时 progress 目录（只放文件，不带 git）。
 * @param {Object<string,string>} files 文件名 → 内容
 * @returns {string} 目录绝对路径
 */
function tempProgress(files) {
  const dir = tmpDir("study-hub-progress-")
  Object.keys(files).forEach(function (name) {
    fs.writeFileSync(path.join(dir, name), files[name], "utf8")
  })
  return dir
}

/**
 * 造一个干净的临时 learning 仓（带 git 仓库与一个轮次文件）。
 * 用 helpers/tmp.js 的 tmpDir，进程退出时自动删掉。
 * @param {Object<string,string>} [extraFiles] 额外要放进去的文件（相对仓库根）
 * @returns {string} 仓库根目录绝对路径
 */
function tempLearning(extraFiles) {
  const root = tmpDir("study-hub-learning-")
  const quiet = { stdio: ["ignore", "pipe", "pipe"] }
  execFileSync("git", ["init", "-q"], Object.assign({ cwd: root }, quiet))
  execFileSync("git", ["config", "user.email", "test@example.com"], Object.assign({ cwd: root }, quiet))
  execFileSync("git", ["config", "user.name", "test"], Object.assign({ cwd: root }, quiet))
  execFileSync("git", ["config", "commit.gpgsign", "false"], Object.assign({ cwd: root }, quiet))

  const progress = path.join(root, "progress")
  fs.mkdirSync(progress, { recursive: true })
  fs.writeFileSync(path.join(progress, "r33-progress.md"), ROUND_FILE, "utf8")
  fs.mkdirSync(path.join(root, "notes"), { recursive: true })

  const files = extraFiles || {}
  Object.keys(files).forEach(function (name) {
    const target = path.join(root, name)
    fs.mkdirSync(path.dirname(target), { recursive: true })
    fs.writeFileSync(target, files[name], "utf8")
  })

  execFileSync("git", ["add", "-A"], Object.assign({ cwd: root }, quiet))
  execFileSync("git", ["commit", "-q", "-m", "初始化"], Object.assign({ cwd: root }, quiet))
  return root
}

/**
 * 把仓库弄脏（写一个未提交的文件）。
 * @param {string} root 仓库根
 * @param {string} name 相对仓库根的路径
 */
function dirty(root, name) {
  const target = path.join(root, name)
  fs.mkdirSync(path.dirname(target), { recursive: true })
  fs.writeFileSync(target, "改一下\n", "utf8")
}

/**
 * 把当前改动提交掉（模拟用户自己提交）。
 * @param {string} root 仓库根
 * @param {string} [message] 提交信息
 */
function commitAll(root, message) {
  const quiet = { stdio: ["ignore", "pipe", "pipe"] }
  execFileSync("git", ["add", "-A"], Object.assign({ cwd: root }, quiet))
  execFileSync("git", ["commit", "-q", "-m", message || "提交"], Object.assign({ cwd: root }, quiet))
}

module.exports = {
  REAL_TABLES: REAL_TABLES,
  ROUND_FILE: ROUND_FILE,
  ROUND_FILE_NO_DONE_DATE: ROUND_FILE_NO_DONE_DATE,
  PROBLEM: PROBLEM,
  fileWithTable: fileWithTable,
  tempProgress: tempProgress,
  tempLearning: tempLearning,
  dirty: dirty,
  commitAll: commitAll,
}
