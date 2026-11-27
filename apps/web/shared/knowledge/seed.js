/**
 * seed.js —— Knowledge Hub 结构性种子数据
 *
 * 内容型种子（题库 / 文章 / 雷达 / 五年计划 / 面试题库 / 摘抄）在 seed-data.js；
 * 本文件放与 store 数据结构强相关的结构性种子：知识图谱节点与边、学习路径、
 * 学习项目、面试记录、模拟面试、复习记录（SM-2 初始 3 条）。
 *
 * 每个函数返回该域的初始数组，由 createListStore 在存储为空时调用。
 */

const id = require("../core/id.js")
const date = require("../core/date.js")

function newId (prefix) {
  return prefix + '-' + id.genId()
}
function now () {
  return new Date().toISOString()
}

/* ============ 知识图谱节点（14） ============ */

const KNOWLEDGE_NODES = [
  { id: 'algo-1', name: '排序与查找', domain: '算法', level: 4 },
  { id: 'algo-2', name: '字符串与哈希', domain: '算法', level: 3 },
  { id: 'algo-3', name: '图论基础', domain: '算法', level: 2 },
  { id: 'algo-4', name: '动态规划', domain: '算法', level: 2 },
  { id: 'sys-1', name: '缓存设计', domain: '系统设计', level: 3 },
  { id: 'sys-2', name: '消息队列', domain: '系统设计', level: 2 },
  { id: 'sys-3', name: '一致性与共识', domain: '系统设计', level: 2 },
  { id: 'cpp-1', name: '指针与内存', domain: 'C/C++', level: 2 },
  { id: 'cpp-2', name: '智能指针', domain: 'C/C++', level: 2 },
  { id: 'cpp-3', name: '模板与 STL', domain: 'C/C++', level: 1 },
  { id: 'db-1', name: '索引原理', domain: '数据库', level: 3 },
  { id: 'db-2', name: '事务与隔离', domain: '数据库', level: 2 },
  { id: 'net-1', name: 'TCP/UDP', domain: '计算机网络', level: 3 },
  { id: 'net-2', name: 'HTTP 协议', domain: '计算机网络', level: 2 }
]

/* 前置依赖边（9 条），静态，不进 db */
const KNOWLEDGE_LINKS = [
  { source: 'cpp-1', target: 'cpp-2', type: 'prerequisite' },
  { source: 'cpp-2', target: 'cpp-3', type: 'prerequisite' },
  { source: 'db-1', target: 'db-2', type: 'prerequisite' },
  { source: 'net-1', target: 'net-2', type: 'prerequisite' },
  { source: 'algo-1', target: 'algo-3', type: 'prerequisite' },
  { source: 'algo-2', target: 'algo-4', type: 'prerequisite' },
  { source: 'sys-1', target: 'sys-2', type: 'prerequisite' },
  { source: 'sys-2', target: 'sys-3', type: 'prerequisite' },
  { source: 'db-2', target: 'sys-1', type: 'prerequisite' }
]

/* ============ 学习路径（3） ============ */

function sub (id, title, type, h) {
  return { id: id, title: title, type: type, estimatedHours: h, done: false }
}

const LEARNING_PATHS = [
  {
    id: 'cpp-master', name: 'C++ 精通之路', icon: 'C++', difficulty: '高级',
    estimatedHours: 120,
    description: '从指针到模板，打牢系统级编程基础。',
    steps: [
      { id: 'cs-1', title: '内存与指针', description: '理解栈堆、指针运算', difficulty: '初级', estimatedHours: 20,
        resources: ['《C 程序设计语言》', 'cpprefrence 指针章节'],
        subSteps: [ sub('cs-1-a', '指针与数组', 'knowledge', 6), sub('cs-1-b', '内存布局', 'knowledge', 6), sub('cs-1-c', '手写内存池', 'project', 8) ] },
      { id: 'cs-2', title: '面向对象', description: '构造/析构/继承', difficulty: '中级', estimatedHours: 30,
        resources: ['《Effective C++》'],
        subSteps: [ sub('cs-2-a', 'RAII', 'knowledge', 8), sub('cs-2-b', '虚函数表', 'knowledge', 8), sub('cs-2-c', '实现一个 Vec', 'project', 14) ] },
      { id: 'cs-3', title: '现代 C++', description: '智能指针与模板', difficulty: '高级', estimatedHours: 40,
        resources: ['《C++ Primer》'],
        subSteps: [ sub('cs-3-a', '智能指针', 'knowledge', 10), sub('cs-3-b', '模板基础', 'knowledge', 12), sub('cs-3-c', 'STL 源码', 'practice', 18) ] }
    ],
    currentStep: 0, completedSubSteps: {}
  },
  {
    id: 'algorithm', name: '算法通关', icon: 'AL', difficulty: '中级',
    estimatedHours: 200,
    description: '刷穿常见算法题型，应对技术面试。',
    steps: [
      { id: 'al-1', title: '数组与字符串', description: '双指针 / 滑动窗口', difficulty: '初级', estimatedHours: 40,
        resources: ['LeetCode 热题'],
        subSteps: [ sub('al-1-a', '双指针', 'knowledge', 10), sub('al-1-b', '滑动窗口', 'practice', 12), sub('al-1-c', '50 题训练', 'practice', 28) ] },
      { id: 'al-2', title: '树与图', description: 'DFS / BFS / 最短路', difficulty: '中级', estimatedHours: 50,
        resources: ['《算法导论》图章'],
        subSteps: [ sub('al-2-a', '遍历', 'knowledge', 10), sub('al-2-b', '最短路', 'practice', 16), sub('al-2-c', '拓扑排序', 'practice', 14) ] },
      { id: 'al-3', title: '动态规划', description: '状态定义与转移', difficulty: '高级', estimatedHours: 60,
        resources: ['《剑指 Offer》'],
        subSteps: [ sub('al-3-a', '线性 DP', 'knowledge', 14), sub('al-3-b', '区间 DP', 'practice', 18), sub('al-3-c', '100 题训练', 'practice', 40) ] }
    ],
    currentStep: 0, completedSubSteps: {}
  },
  {
    id: 'system-design', name: '系统设计入门', icon: 'SD', difficulty: '高级',
    estimatedHours: 150,
    description: '从单应用到分布式，建立架构直觉。',
    steps: [
      { id: 'sd-1', title: '可扩展基础', description: '负载均衡 / 缓存', difficulty: '中级', estimatedHours: 40,
        resources: ['《设计数据密集型应用》'],
        subSteps: [ sub('sd-1-a', '缓存策略', 'knowledge', 12), sub('sd-1-b', 'CDN', 'knowledge', 10), sub('sd-1-c', '设计一个短链', 'project', 18) ] },
      { id: 'sd-2', title: '存储与一致性', description: '分库分表 / 共识', difficulty: '高级', estimatedHours: 60,
        resources: ['Raft 论文'],
        subSteps: [ sub('sd-2-a', '副本与一致', 'knowledge', 16), sub('sd-2-b', 'Raft', 'practice', 20), sub('sd-2-c', 'Mini-Raft', 'project', 40) ] }
    ],
    currentStep: 0, completedSubSteps: {}
  }
]

/* ============ 学习项目（study_projects，3） ============ */

const STUDY_PROJECTS = [
  { id: newId('project'), name: '手写 STL 容器库', description: '实现 vector / list / map，理解底层数据结构。',
    status: '进行中', priority: '高', tags: ['cpp', 'ds'], knowledgeLinks: ['cpp-3', 'algo-1'],
    createdAt: now(), updatedAt: now() },
  { id: newId('project'), name: '分布式缓存系统', description: '支持 LRU 淘汰与多节点复制。',
    status: '规划中', priority: '中', tags: ['sys', 'db'], knowledgeLinks: ['sys-1', 'db-2'],
    createdAt: now(), updatedAt: now() },
  { id: newId('project'), name: 'LeetCode 刷题笔记', description: '按题型整理解题模板与套路。',
    status: '已完成', priority: '中', tags: ['algo'], knowledgeLinks: ['algo-4'],
    createdAt: now(), updatedAt: now() }
]

/* ============ 面试记录（interviews，2） ============ */

const INTERVIEW_COMPANIES = [
  { id: newId('company'), name: '字节跳动', position: '后端开发', status: '二面', salary: '35-50K',
    notes: '侧重系统工程与算法，手写代码较多。',
    timeline: [
      { id: newId('rec'), date: date.daysAgo(10), type: '一面', content: '算法：二叉树层序遍历；项目：缓存设计' },
      { id: newId('rec'), date: date.daysAgo(3), type: '二面', content: '系统设计：短链服务；八股：HTTP 与 TCP' }
    ], createdAt: now() },
  { id: newId('company'), name: '阿里巴巴', position: '后端开发', status: '一面',
    notes: '更看重业务理解。',
    timeline: [
      { id: newId('rec'), date: date.daysAgo(5), type: '一面', content: '算法：最长递增子序列；中间件相关问题' }
    ], createdAt: now() }
]

/* ============ 模拟面试（mock_interviews，1） ============ */

const MOCK_SESSIONS = [
  { id: newId('session'), date: date.daysAgo(1), role: '后端开发', difficulty: '中级',
    questions: [
      { question: '如何设计一个限流系统？', answer: '令牌桶 / 漏桶，结合 Redis 计数。',
        evaluation: { score: 85, feedback: '思路清晰，补充了突发流量处理。', suggestions: ['提一下分布式限流'] } }
    ], totalScore: 85, duration: 20 }
]

/* ============ 复习记录（reviews，SM-2 初始 3 条） ============ */

const REVIEW_SEED = [
  { id: newId('review'), question: '什么是时间复杂度？',
    answer: '时间复杂度是算法执行时间与输入规模之间关系的度量，用大 O 表示法描述。常见的有 O(1)、O(log n)、O(n)、O(n log n)、O(n²) 等。',
    domain: '算法', easiness: 2.5, interval: 1, repetitions: 0,
    nextReviewDate: now(), lastReviewDate: '' },
  { id: newId('review'), question: 'C++ 中智能指针有哪些？',
    answer: 'unique_ptr（独占所有权）、shared_ptr（共享，引用计数）、weak_ptr（弱引用，避免环）。',
    domain: 'C/C++', easiness: 2.5, interval: 1, repetitions: 0,
    nextReviewDate: now(), lastReviewDate: '' },
  { id: newId('review'), question: 'TCP 和 UDP 的区别是什么？',
    answer: 'TCP 面向连接、可靠、有拥塞与流量控制；UDP 无连接、不可靠但高效。TCP 适合可靠传输，UDP 适合实时。',
    domain: '计算机网络', easiness: 2.5, interval: 1, repetitions: 0,
    nextReviewDate: now(), lastReviewDate: '' }
]

module.exports = {
  KNOWLEDGE_NODES: KNOWLEDGE_NODES,
  KNOWLEDGE_LINKS: KNOWLEDGE_LINKS,
  LEARNING_PATHS: LEARNING_PATHS,
  STUDY_PROJECTS: STUDY_PROJECTS,
  INTERVIEW_COMPANIES: INTERVIEW_COMPANIES,
  MOCK_SESSIONS: MOCK_SESSIONS,
  REVIEW_SEED: REVIEW_SEED
}
