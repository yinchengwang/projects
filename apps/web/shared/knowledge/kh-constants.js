/**
 * kh-constants.js —— Knowledge Hub 专属领域常量
 *
 * 与 PWB 共用的 core/constants.js 保持「逐字节相同」，不在此修改；
 * KH 特有的分类 / 状态 / 配色集中放这里，避免污染共享层。
 *
 * 色彩承载语义：用于列表卡左色条、标签底色、图表色板，不用于装饰。
 */

/** 题库轮换顺序（每日计划按此轮换 stack） */
const STACK_ORDER = ['c', 'cpp', 'db', 'ds', 'linux', 'py', 'vdb']

/** 技术栈元信息（标签 / 图标字母 / 主色） */
const STACK_META = [
  { key: 'c', label: 'C', icon: 'C', color: '#5C6BC0' },
  { key: 'cpp', label: 'C++', icon: 'C+', color: '#7F77DD' },
  { key: 'db', label: '数据库', icon: 'DB', color: '#0D9488' },
  { key: 'ds', label: '数据结构', icon: 'DS', color: '#0284C7' },
  { key: 'linux', label: 'Linux', icon: 'LX', color: '#F5A524' },
  { key: 'py', label: 'Python', icon: 'PY', color: '#30A46C' },
  { key: 'vdb', label: '向量数据库', icon: 'V', color: '#DB2777' }
]

function stackMeta (key) {
  for (let i = 0; i < STACK_META.length; i++) {
    if (STACK_META[i].key === key) return STACK_META[i]
  }
  return STACK_META[0]
}

/** 技术雷达象限 / 环 */
const QUADRANTS = [
  { key: 'language', label: '语言', color: '#0284C7' },
  { key: 'systems', label: '系统', color: '#7F77DD' },
  { key: 'algorithms', label: '算法', color: '#30A46C' },
  { key: 'engineering', label: '工程', color: '#F5A524' }
]

const RINGS = [
  { key: 'basic', label: '基础', color: '#30A46C' },
  { key: 'intermediate', label: '进阶', color: '#F5A524' },
  { key: 'advanced', label: '精通', color: '#E5484D' }
]

/** 知识图谱五大领域（雷达差距分析） */
const KNOWLEDGE_DOMAINS = [
  { key: '算法', label: '算法', color: '#30A46C' },
  { key: '系统设计', label: '系统设计', color: '#7F77DD' },
  { key: 'C/C++', label: 'C/C++', color: '#E5484D' },
  { key: '数据库', label: '数据库', color: '#0D9488' },
  { key: '计算机网络', label: '计算机网络', color: '#0284C7' }
]

/** 雷达目标分（gap 分析用，满分 100） */
const KNOWLEDGE_TARGETS = {
  '算法': 80,
  '系统设计': 70,
  'C/C++': 75,
  '数据库': 65,
  '计算机网络': 60
}

/** 面试题 / 面试状态 */
const INTERVIEW_STATUSES = [
  { key: '投递', label: '投递', color: '#64748B', progress: 0 },
  { key: '笔试', label: '笔试', color: '#0D9488', progress: 1 },
  { key: '一面', label: '一面', color: '#0284C7', progress: 2 },
  { key: '二面', label: '二面', color: '#7F77DD', progress: 3 },
  { key: '三面', label: '三面', color: '#DB2777', progress: 4 },
  { key: 'HR', label: 'HR', color: '#F5A524', progress: 5 },
  { key: 'offer', label: 'Offer', color: '#30A46C', progress: 6 },
  { key: '拒绝', label: '拒绝', color: '#E5484D', progress: 6 }
]

function interviewStatus (key) {
  for (let i = 0; i < INTERVIEW_STATUSES.length; i++) {
    if (INTERVIEW_STATUSES[i].key === key) return INTERVIEW_STATUSES[i]
  }
  return INTERVIEW_STATUSES[0]
}

/** 面试记录类型 */
const INTERVIEW_TYPES = [
  { key: '一面', label: '一面' },
  { key: '二面', label: '二面' },
  { key: '三面', label: '三面' },
  { key: 'HR', label: 'HR' },
  { key: '笔试', label: '笔试' }
]

/** 学习项目状态（改名后 study_projects，区别于 PWB 作品集） */
const PROJECT_STATUSES = [
  { key: '规划中', label: '规划中', color: '#64748B' },
  { key: '进行中', label: '进行中', color: '#30A46C' },
  { key: '已完成', label: '已完成', color: '#0284C7' },
  { key: '暂停', label: '暂停', color: '#F5A524' },
  { key: '废弃', label: '废弃', color: '#E5484D' }
]

function projectStatus (key) {
  for (let i = 0; i < PROJECT_STATUSES.length; i++) {
    if (PROJECT_STATUSES[i].key === key) return PROJECT_STATUSES[i]
  }
  return PROJECT_STATUSES[0]
}

const PROJECT_PRIORITIES = [
  { key: '高', label: '高', color: '#E5484D' },
  { key: '中', label: '中', color: '#F5A524' },
  { key: '低', label: '低', color: '#30A46C' }
]

function projectPriority (key) {
  for (let i = 0; i < PROJECT_PRIORITIES.length; i++) {
    if (PROJECT_PRIORITIES[i].key === key) return PROJECT_PRIORITIES[i]
  }
  return PROJECT_PRIORITIES[1]
}

/** 题库题型 */
const QUIZ_TYPES = [
  { key: '选择题', label: '选择题' },
  { key: '判断题', label: '判断题' },
  { key: '填空题', label: '填空题' },
  { key: '简答题', label: '简答题' },
  { key: '编程题', label: '编程题' }
]

/** 题库难度 */
const QUIZ_DIFFICULTIES = [
  { key: '入门', label: '入门', color: '#30A46C' },
  { key: '初级', label: '初级', color: '#0D9488' },
  { key: '中级', label: '中级', color: '#F5A524' },
  { key: '高级', label: '高级', color: '#E5484D' },
  { key: '专家', label: '专家', color: '#DB2777' }
]

function quizDifficulty (key) {
  for (let i = 0; i < QUIZ_DIFFICULTIES.length; i++) {
    if (QUIZ_DIFFICULTIES[i].key === key) return QUIZ_DIFFICULTIES[i]
  }
  return QUIZ_DIFFICULTIES[2]
}

/** 学习路径 / 步骤难度 */
const LEARNING_DIFFICULTIES = [
  { key: '入门', label: '入门', color: '#30A46C' },
  { key: '初级', label: '初级', color: '#0D9488' },
  { key: '中级', label: '中级', color: '#F5A524' },
  { key: '高级', label: '高级', color: '#E5484D' },
  { key: '专家', label: '专家', color: '#DB2777' }
]

/** 子步骤类型 */
const SUB_STEP_TYPES = [
  { key: 'knowledge', label: '知识', color: '#0284C7' },
  { key: 'practice', label: '练习', color: '#30A46C' },
  { key: 'project', label: '项目', color: '#7F77DD' }
]

function subStepType (key) {
  for (let i = 0; i < SUB_STEP_TYPES.length; i++) {
    if (SUB_STEP_TYPES[i].key === key) return SUB_STEP_TYPES[i]
  }
  return SUB_STEP_TYPES[0]
}

/** 摘抄状态 */
const EXCERPT_STATUS = [
  { key: 'reading', label: '在读', color: '#0284C7' },
  { key: 'read', label: '读过', color: '#30A46C' },
  { key: 'mastered', label: '精读', color: '#7F77DD' },
  { key: 'wishlist', label: '想读', color: '#64748B' }
]

function excerptStatus (key) {
  for (let i = 0; i < EXCERPT_STATUS.length; i++) {
    if (EXCERPT_STATUS[i].key === key) return EXCERPT_STATUS[i]
  }
  return EXCERPT_STATUS[0]
}

/** 五年计划年份 */
const FIVE_YEAR_YEARS = [2026, 2027, 2028, 2029, 2030]

/** 通知类型 */
const NOTIFICATION_TYPES = [
  { key: 'review', label: '复习', icon: '📚', iconBg: 'rgba(2,132,199,0.14)' },
  { key: 'interview', label: '面试', icon: '💼', iconBg: 'rgba(127,119,221,0.14)' },
  { key: 'streak', label: '打卡', icon: '🔥', iconBg: 'rgba(245,165,36,0.14)' },
  { key: 'wrongbook', label: '错题', icon: '✏️', iconBg: 'rgba(229,72,77,0.14)' },
  { key: 'system', label: '系统', icon: '⚙️', iconBg: 'rgba(100,116,139,0.14)' }
]

module.exports = {
  STACK_ORDER: STACK_ORDER,
  STACK_META: STACK_META,
  stackMeta: stackMeta,
  QUADRANTS: QUADRANTS,
  RINGS: RINGS,
  KNOWLEDGE_DOMAINS: KNOWLEDGE_DOMAINS,
  KNOWLEDGE_TARGETS: KNOWLEDGE_TARGETS,
  INTERVIEW_STATUSES: INTERVIEW_STATUSES,
  interviewStatus: interviewStatus,
  INTERVIEW_TYPES: INTERVIEW_TYPES,
  PROJECT_STATUSES: PROJECT_STATUSES,
  projectStatus: projectStatus,
  PROJECT_PRIORITIES: PROJECT_PRIORITIES,
  projectPriority: projectPriority,
  QUIZ_TYPES: QUIZ_TYPES,
  QUIZ_DIFFICULTIES: QUIZ_DIFFICULTIES,
  quizDifficulty: quizDifficulty,
  LEARNING_DIFFICULTIES: LEARNING_DIFFICULTIES,
  SUB_STEP_TYPES: SUB_STEP_TYPES,
  subStepType: subStepType,
  EXCERPT_STATUS: EXCERPT_STATUS,
  excerptStatus: excerptStatus,
  FIVE_YEAR_YEARS: FIVE_YEAR_YEARS,
  NOTIFICATION_TYPES: NOTIFICATION_TYPES
}
