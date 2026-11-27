"use strict"
/**
 * keys.js —— knowledge-hub 的存储键表
 *
 * 14 个数据域沿用小程序版的键名。settings 保持 `settings`（即 db:settings）——
 * programmer-workbench 已改用 `db:wb_settings`，两个模块不再抢同一个键。
 */

/** 14 个数据域的存储键 */
const KEYS = {
  daily: "daily",
  excerpts: "excerpts",
  five_year_plan: "five_year_plan",
  interview_bank: "interview_bank",
  interviews: "interviews",
  knowledge: "knowledge",
  learning_paths: "learning_paths",
  mock_interviews: "mock_interviews",
  notifications: "notifications",
  study_projects: "study_projects",
  quizzes: "quizzes",
  reviews: "reviews",
  settings: "settings",
  user: "user",
  seeded: "__seeded__"
}

module.exports = { KEYS: KEYS }
