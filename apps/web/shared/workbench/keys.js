"use strict"
/**
 * keys.js —— workbench 的存储键表
 *
 * 与 knowledge-hub 的 `db:settings` 隔开：workbench 用 `db:wb_settings`。
 * 理由：两个模块的 settings 域默认值不同（workbench 的 primaryColor 是
 * 'sky'，knowledge-hub 是 'violet'），共用一个键会互相覆盖。
 */

/** 12 个数据域的存储键 */
const KEYS = {
  todos: "todos",
  techNotes: "tech_notes",
  snippets: "snippets",
  interview: "interview",
  resources: "resources",
  reading: "reading",
  fitness: "fitness",
  mood: "mood",
  schedule: "schedule",
  projects: "projects",
  insights: "insights",
  settings: "wb_settings",
  seeded: "__seeded__"
}

module.exports = { KEYS: KEYS }
