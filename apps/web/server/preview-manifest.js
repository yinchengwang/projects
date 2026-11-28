"use strict"
/**
 * preview-manifest.js —— 前端页面清单
 *
 * 预览页（/preview.html）左侧那棵目录树的数据源。
 * 放服务端是为了让预览页只管渲染，不内嵌一份会过期的清单。
 *
 * 每一项：
 *   { path, title, desc?, detail? }
 *   path   —— 应用内的路由路径
 *   title  —— 侧栏显示名
 *   desc   —— 副标题（可选，鼠标悬停显示）
 *   detail —— true 表示详情页，需要 id；id 从浏览器 localStorage 里解析，
 *             解析不到时回落到它所属的列表页
 *   seed   —— true 表示该域的 seed 数据需要先访问一次列表页才会落到 localStorage
 *   list   —— detail 页对应的列表页路径（解析不到 id 时的回落目标）
 */

const WORKBENCH = "workbench"
const KNOWLEDGE = "knowledge"
const AITIMER = "aitimer"
const TRIP = "trip"
const GAMES = "games"

/**
 * 构造清单。
 * @returns {Array} 模块数组
 */
function buildManifest () {
  return [
    {
      id: "shell",
      name: "应用外壳",
      pages: [
        { path: "/", title: "模块入口首页", desc: "五个模块的入口卡片 + 两个独立服务状态" }
      ]
    },
    {
      id: WORKBENCH,
      name: "程序员工作台",
      pages: [
        { path: "/" + WORKBENCH + "/pages/dashboard/index", title: "今日面板", desc: "总览与今日待办" },
        { path: "/" + WORKBENCH + "/pages/tech/index", title: "技术工作台", desc: "笔记 / 片段 / 题库 / 资源" },
        { path: "/" + WORKBENCH + "/pages/tech/notes/index", title: "技术笔记 · 列表" },
        { path: "/" + WORKBENCH + "/pages/tech/notes/detail", title: "技术笔记 · 详情", detail: true, seed: true, list: "/" + WORKBENCH + "/pages/tech/notes/index" },
        { path: "/" + WORKBENCH + "/pages/tech/snippets/index", title: "代码片段 · 列表" },
        { path: "/" + WORKBENCH + "/pages/tech/snippets/detail", title: "代码片段 · 详情", detail: true, seed: true, list: "/" + WORKBENCH + "/pages/tech/snippets/index" },
        { path: "/" + WORKBENCH + "/pages/tech/interview/index", title: "面试题库 · 列表" },
        { path: "/" + WORKBENCH + "/pages/tech/interview/detail", title: "面试题库 · 详情", detail: true, seed: true, list: "/" + WORKBENCH + "/pages/tech/interview/index" },
        { path: "/" + WORKBENCH + "/pages/tech/resources/index", title: "资源库" },
        { path: "/" + WORKBENCH + "/pages/life/index", title: "生活总览" },
        { path: "/" + WORKBENCH + "/pages/life/reading/index", title: "读书清单" },
        { path: "/" + WORKBENCH + "/pages/life/fitness/index", title: "健身打卡" },
        { path: "/" + WORKBENCH + "/pages/life/mood/index", title: "心情记录" },
        { path: "/" + WORKBENCH + "/pages/life/schedule/index", title: "日程安排" },
        { path: "/" + WORKBENCH + "/pages/projects/index", title: "项目集" },
        { path: "/" + WORKBENCH + "/pages/insights/index", title: "数据洞察" },
        { path: "/" + WORKBENCH + "/pages/tools/index", title: "离线工具箱", desc: "10 个不用联网的小工具" },
        { path: "/" + WORKBENCH + "/pages/profile/index", title: "个人档案" }
      ]
    },
    {
      id: KNOWLEDGE,
      name: "知识中心",
      pages: [
        { path: "/" + KNOWLEDGE + "/pages/dashboard/index", title: "学习仪表盘" },
        { path: "/" + KNOWLEDGE + "/pages/learn/index", title: "学习路径" },
        { path: "/" + KNOWLEDGE + "/pages/learn/quiz/index", title: "题库练习" },
        { path: "/" + KNOWLEDGE + "/pages/learn/review/index", title: "间隔复习", desc: "SM-2 调度" },
        { path: "/" + KNOWLEDGE + "/pages/learn/deep/index", title: "图解系列", desc: "Markdown 长文阅读器" },
        { path: "/" + KNOWLEDGE + "/pages/learn/path/index", title: "五年计划路径" },
        { path: "/" + KNOWLEDGE + "/pages/interview/index", title: "面试总览" },
        { path: "/" + KNOWLEDGE + "/pages/interview/tracker/index", title: "面试追踪" },
        { path: "/" + KNOWLEDGE + "/pages/interview/bank/index", title: "面试题库" },
        { path: "/" + KNOWLEDGE + "/pages/interview/mock/index", title: "模拟面试" },
        { path: "/" + KNOWLEDGE + "/pages/knowledge/index", title: "知识图谱" },
        { path: "/" + KNOWLEDGE + "/pages/knowledge/graph/index", title: "图谱可视化", desc: "Canvas 绘制" },
        { path: "/" + KNOWLEDGE + "/pages/knowledge/excerpt/index", title: "论文精读" },
        { path: "/" + KNOWLEDGE + "/pages/knowledge/radar/index", title: "技术雷达", desc: "Canvas 绘制" },
        { path: "/" + KNOWLEDGE + "/pages/profile/index", title: "个人档案" },
        { path: "/" + KNOWLEDGE + "/pages/plan/five-year/index", title: "五年计划" },
        { path: "/" + KNOWLEDGE + "/pages/plan/roadmap/index", title: "能力路线图" }
      ]
    },
    {
      id: AITIMER,
      name: "AI 简报",
      pages: [
        { path: "/" + AITIMER + "/pages/index", title: "产物概览", desc: "3 个定时任务的全部产物" },
        { path: "/" + AITIMER + "/pages/series", title: "领域系列 · 数据库", query: "id=db" },
        { path: "/" + AITIMER + "/pages/doc", title: "文档阅读 · 每日简报", query: "id=briefing/2026-11-28" }
      ]
    },
    {
      id: TRIP,
      name: "足迹",
      pages: [
        { path: "/" + TRIP + "/pages/index", title: "全部记录" },
        { path: "/" + TRIP + "/pages/new-trip", title: "新增记录" },
        { path: "/" + TRIP + "/pages/detail", title: "记录详情", detail: true, list: "/" + TRIP + "/pages/index" }
      ]
    },
    {
      id: GAMES,
      name: "游戏",
      pages: [
        { path: "/" + GAMES + "/pages/index", title: "游戏大厅" },
        { path: "/" + GAMES + "/pages/snake", title: "贪吃蛇" },
        { path: "/" + GAMES + "/pages/game2048", title: "2048" },
        { path: "/" + GAMES + "/pages/sudoku", title: "数独" },
        { path: "/" + GAMES + "/pages/match3", title: "消消乐" }
      ]
    }
  ]
}

module.exports = { buildManifest: buildManifest }
