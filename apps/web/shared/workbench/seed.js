/**
 * seed.js —— 首次启动的示例数据
 *
 * 每个函数返回该域的初始数组，由 createListStore 在存储为空时调用。
 * 目的是让用户首次打开就能看到完整界面，而不是一片空白。
 */

const id = require("../core/id.js")
const date = require("../core/date.js")

function now () {
  return new Date().toISOString()
}

function newId () {
  return id.genId()
}

/** 待办 */
function todos () {
  const today = date.todayStr()
  return [
    {
      id: newId(), title: '完成 DevBench 看板布局', done: false, priority: 'high',
      category: 'work', dueDate: today, createdAt: now()
    },
    {
      id: newId(), title: '读《系统设计面试》前 4 章', done: false, priority: 'med',
      category: 'learning', createdAt: now()
    },
    {
      id: newId(), title: '晨跑 5 公里', done: true, priority: 'low',
      category: 'life', dueDate: today, createdAt: now()
    }
  ]
}

/** 技术笔记 */
function techNotes () {
  return [
    {
      id: newId(),
      title: 'React Hooks 最佳实践',
      category: 'frontend',
      tags: ['react', 'hooks'],
      pinned: true,
      content: [
        '# 核心要点',
        '',
        '- 单一职责：每个 useState 只负责一个状态',
        '- 副作用放 useEffect，记得写 cleanup',
        '- 自定义 Hook 抽离可复用逻辑（以 use 开头）',
        '- 配合 memo / useMemo 减少重渲染',
        '',
        '## 反例',
        '',
        '```js',
        'useEffect(() => {',
        '  fetchData()',
        '})  // 缺少依赖数组，每次渲染都执行',
        '```'
      ].join('\n'),
      createdAt: now(),
      updatedAt: now()
    },
    {
      id: newId(),
      title: 'PostgreSQL 索引原理速记',
      category: 'database',
      tags: ['postgres', 'index'],
      pinned: false,
      content: [
        '- B-Tree 索引适合等值与范围查询',
        '- 复合索引遵循最左前缀原则',
        '- 部分索引可显著减小体积',
        '',
        '```sql',
        "CREATE INDEX idx_orders_pending ON orders(id) WHERE status = 'pending';",
        '```'
      ].join('\n'),
      createdAt: now(),
      updatedAt: now()
    }
  ]
}

/** 代码片段 */
function snippets () {
  return [
    {
      id: newId(),
      title: '防抖 debounce',
      language: 'typescript',
      description: '通用防抖函数',
      tags: ['utility'],
      favorite: true,
      code: [
        'export function debounce<T extends (...a: any[]) => any>(fn: T, ms = 300) {',
        '  let t: any',
        '  return (...args: Parameters<T>) => {',
        '    clearTimeout(t)',
        '    t = setTimeout(() => fn(...args), ms)',
        '  }',
        '}'
      ].join('\n'),
      createdAt: now()
    },
    {
      id: newId(),
      title: 'Python 异步上下文',
      language: 'python',
      description: 'asyncio 异步上下文管理器',
      tags: ['python', 'async'],
      favorite: false,
      code: [
        'import asyncio, contextlib',
        '',
        '@contextlib.asynccontextmanager',
        'async def timer(label):',
        '    start = asyncio.get_event_loop().time()',
        '    try:',
        '        yield',
        '    finally:',
        '        print(label, asyncio.get_event_loop().time() - start)'
      ].join('\n'),
      createdAt: now()
    }
  ]
}

/** 面试题 */
function interview () {
  return [
    {
      id: newId(),
      question: '什么是 React 中的虚拟 DOM？它的优势是什么？',
      answer: [
        '虚拟 DOM 是 React 内部维护的一棵 JS 对象树，描述真实 DOM 的结构。',
        '',
        '优势：',
        '1. 跨平台渲染（浏览器、Native、小程序）',
        '2. 通过 Diff 算法最小化 DOM 操作',
        '3. 声明式编程模型，UI = f(state)'
      ].join('\n'),
      category: 'frontend',
      difficulty: 'med',
      starred: true,
      reviewCount: 2,
      lastReviewed: date.daysAgo(2),
      createdAt: now()
    },
    {
      id: newId(),
      question: 'TCP 三次握手的过程？为什么是三次？',
      answer: [
        '三次握手：SYN → SYN+ACK → ACK。',
        '',
        '目的：双方同步序列号并确认收发能力。',
        '两次无法确认 B 是否收到 SYN，四次浪费。'
      ].join('\n'),
      category: 'backend',
      difficulty: 'med',
      starred: false,
      reviewCount: 0,
      createdAt: now()
    }
  ]
}

/** 资源收藏 */
function resources () {
  return [
    {
      id: newId(),
      title: 'Designing Data-Intensive Applications',
      type: 'book',
      category: 'backend',
      url: 'https://dataintensive.net',
      note: '系统设计与存储引擎必读，反复刷',
      stars: 5,
      archived: false,
      createdAt: now()
    },
    {
      id: newId(),
      title: 'MDN Web Docs',
      type: 'doc',
      category: 'frontend',
      url: 'https://developer.mozilla.org',
      note: '',
      stars: 5,
      archived: false,
      createdAt: now()
    }
  ]
}

/** 读书 */
function reading () {
  return [
    {
      id: newId(),
      title: '代码整洁之道',
      author: 'Robert C. Martin',
      totalPages: 450,
      currentPage: 120,
      status: 'reading',
      rating: 5,
      notes: '',
      startedAt: date.daysAgo(10),
      createdAt: now()
    },
    {
      id: newId(),
      title: '深入理解计算机系统',
      author: 'Bryant',
      totalPages: 800,
      currentPage: 0,
      status: 'wishlist',
      rating: 0,
      notes: '',
      createdAt: now()
    }
  ]
}

/** 健身 */
function fitness () {
  return [
    {
      id: newId(),
      date: date.todayStr(),
      type: 'cardio',
      name: '晨跑',
      durationMin: 30,
      calories: 280,
      notes: '',
      createdAt: now()
    }
  ]
}

/** 心情 */
function mood () {
  return [
    {
      id: newId(),
      date: date.todayStr(),
      level: 4,
      tags: ['work', 'code'],
      note: '今天修了几个 bug，心情不错。',
      createdAt: now()
    }
  ]
}

/** 日程 */
function schedule () {
  const tomorrow = new Date(Date.now() + 86400000)
  tomorrow.setHours(10, 0, 0, 0)
  return [
    {
      id: newId(),
      title: '团队周会',
      startAt: tomorrow.toISOString(),
      location: '线上 · 腾讯会议',
      note: '',
      reminder: true,
      done: false,
      createdAt: now()
    }
  ]
}

/** 项目集 */
function projects () {
  return [
    {
      id: newId(),
      name: 'DevBench · 程序员工作台',
      description: '为开发者量身定制的个人工作台，集成技术学习、生活记录、工具集与作品展示。',
      techStack: ['原生小程序', 'JavaScript', 'WXSS'],
      status: 'active',
      stars: 5,
      repoUrl: '',
      demoUrl: '',
      highlights: ['离线优先，数据本地化', '7 大板块一站式管理', '内置 10 个开发常用工具'],
      startDate: date.daysAgo(30),
      createdAt: now()
    }
  ]
}

/** 闪念 */
function insights () {
  return [
    {
      id: newId(),
      content: '本地优先的架构让体验稳定，先离线可用，再考虑同步。',
      tags: ['arch'],
      pinned: true,
      archived: false,
      createdAt: now()
    },
    {
      id: newId(),
      content: '把「记一下」的成本压到一步，比做十个整理功能更有用。',
      tags: ['product'],
      pinned: false,
      archived: false,
      createdAt: now()
    }
  ]
}

module.exports = {
  todos: todos,
  techNotes: techNotes,
  snippets: snippets,
  interview: interview,
  resources: resources,
  reading: reading,
  fitness: fitness,
  mood: mood,
  schedule: schedule,
  projects: projects,
  insights: insights
}
