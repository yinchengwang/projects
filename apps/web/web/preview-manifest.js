"use strict"
/**
 * preview-manifest.js —— 页面清单的静态副本
 *
 * 由 apps/web/preview/sync.js 从 server/preview-manifest.js 生成，**不要手改**。
 * 手改会在下次同步时被覆盖 —— 改源文件，然后跑 node apps/web/preview/sync.js。
 *
 * 做成静态文件而不是接口，是为了让预览页在 file:// 下也能用：
 * 那时 fetch 会被 CORS 拦掉，而 <script src> 不会。
 */

var manifest = [
  {
    "id": "shell",
    "name": "应用外壳",
    "pages": [
      {
        "path": "/",
        "title": "模块入口首页",
        "desc": "五个模块的入口卡片 + 两个独立服务状态"
      }
    ]
  },
  {
    "id": "workbench",
    "name": "程序员工作台",
    "pages": [
      {
        "path": "/workbench/pages/dashboard/index",
        "title": "今日面板",
        "desc": "总览与今日待办"
      },
      {
        "path": "/workbench/pages/tech/index",
        "title": "技术工作台",
        "desc": "笔记 / 片段 / 题库 / 资源"
      },
      {
        "path": "/workbench/pages/tech/notes/index",
        "title": "技术笔记 · 列表"
      },
      {
        "path": "/workbench/pages/tech/notes/detail",
        "title": "技术笔记 · 详情",
        "detail": true,
        "seed": true,
        "list": "/workbench/pages/tech/notes/index"
      },
      {
        "path": "/workbench/pages/tech/snippets/index",
        "title": "代码片段 · 列表"
      },
      {
        "path": "/workbench/pages/tech/snippets/detail",
        "title": "代码片段 · 详情",
        "detail": true,
        "seed": true,
        "list": "/workbench/pages/tech/snippets/index"
      },
      {
        "path": "/workbench/pages/tech/interview/index",
        "title": "面试题库 · 列表"
      },
      {
        "path": "/workbench/pages/tech/interview/detail",
        "title": "面试题库 · 详情",
        "detail": true,
        "seed": true,
        "list": "/workbench/pages/tech/interview/index"
      },
      {
        "path": "/workbench/pages/tech/resources/index",
        "title": "资源库"
      },
      {
        "path": "/workbench/pages/life/index",
        "title": "生活总览"
      },
      {
        "path": "/workbench/pages/life/reading/index",
        "title": "读书清单"
      },
      {
        "path": "/workbench/pages/life/fitness/index",
        "title": "健身打卡"
      },
      {
        "path": "/workbench/pages/life/mood/index",
        "title": "心情记录"
      },
      {
        "path": "/workbench/pages/life/schedule/index",
        "title": "日程安排"
      },
      {
        "path": "/workbench/pages/projects/index",
        "title": "项目集"
      },
      {
        "path": "/workbench/pages/insights/index",
        "title": "数据洞察"
      },
      {
        "path": "/workbench/pages/tools/index",
        "title": "离线工具箱",
        "desc": "10 个不用联网的小工具"
      },
      {
        "path": "/workbench/pages/profile/index",
        "title": "个人档案"
      }
    ]
  },
  {
    "id": "knowledge",
    "name": "知识中心",
    "pages": [
      {
        "path": "/knowledge/pages/dashboard/index",
        "title": "学习仪表盘"
      },
      {
        "path": "/knowledge/pages/learn/index",
        "title": "学习路径"
      },
      {
        "path": "/knowledge/pages/learn/quiz/index",
        "title": "题库练习"
      },
      {
        "path": "/knowledge/pages/learn/review/index",
        "title": "间隔复习",
        "desc": "SM-2 调度"
      },
      {
        "path": "/knowledge/pages/learn/deep/index",
        "title": "图解系列",
        "desc": "Markdown 长文阅读器"
      },
      {
        "path": "/knowledge/pages/learn/path/index",
        "title": "五年计划路径"
      },
      {
        "path": "/knowledge/pages/interview/index",
        "title": "面试总览"
      },
      {
        "path": "/knowledge/pages/interview/tracker/index",
        "title": "面试追踪"
      },
      {
        "path": "/knowledge/pages/interview/bank/index",
        "title": "面试题库"
      },
      {
        "path": "/knowledge/pages/interview/mock/index",
        "title": "模拟面试"
      },
      {
        "path": "/knowledge/pages/knowledge/index",
        "title": "知识图谱"
      },
      {
        "path": "/knowledge/pages/knowledge/graph/index",
        "title": "图谱可视化",
        "desc": "Canvas 绘制"
      },
      {
        "path": "/knowledge/pages/knowledge/excerpt/index",
        "title": "论文精读"
      },
      {
        "path": "/knowledge/pages/knowledge/radar/index",
        "title": "技术雷达",
        "desc": "Canvas 绘制"
      },
      {
        "path": "/knowledge/pages/profile/index",
        "title": "个人档案"
      },
      {
        "path": "/knowledge/pages/plan/five-year/index",
        "title": "五年计划"
      },
      {
        "path": "/knowledge/pages/plan/roadmap/index",
        "title": "能力路线图"
      }
    ]
  },
  {
    "id": "aitimer",
    "name": "AI 简报",
    "pages": [
      {
        "path": "/aitimer/pages/index",
        "title": "产物概览",
        "desc": "3 个定时任务的全部产物"
      },
      {
        "path": "/aitimer/pages/series",
        "title": "领域系列 · 数据库",
        "query": "id=db"
      },
      {
        "path": "/aitimer/pages/doc",
        "title": "文档阅读 · 每日简报",
        "query": "id=briefing/2026-11-28"
      }
    ]
  },
  {
    "id": "trip",
    "name": "足迹",
    "pages": [
      {
        "path": "/trip/pages/index",
        "title": "全部记录"
      },
      {
        "path": "/trip/pages/new-trip",
        "title": "新增记录"
      },
      {
        "path": "/trip/pages/detail",
        "title": "记录详情",
        "detail": true,
        "list": "/trip/pages/index"
      }
    ]
  },
  {
    "id": "games",
    "name": "游戏",
    "pages": [
      {
        "path": "/games/pages/index",
        "title": "游戏大厅"
      },
      {
        "path": "/games/pages/snake",
        "title": "贪吃蛇"
      },
      {
        "path": "/games/pages/game2048",
        "title": "2048"
      },
      {
        "path": "/games/pages/sudoku",
        "title": "数独"
      },
      {
        "path": "/games/pages/match3",
        "title": "消消乐"
      }
    ]
  }
]

if (typeof window !== "undefined") window.__PREVIEW_MANIFEST__ = manifest
if (typeof module === "object" && module.exports) module.exports = manifest
