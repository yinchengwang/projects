/**
 * constants.js —— 分类 / 优先级 / 状态的标签与配色
 *
 * 设计约定：色彩承载语义。这里的 color 用于列表卡左色条、标签底色与徽标，
 * 不用于装饰。emoji 仅作为内容标识（分类徽标），不用于 UI 控件。
 */

/** 技术分类 */
const TECH_CATEGORIES = [
  { key: 'frontend', label: '前端', color: '#0284C7', emoji: '🎨' },
  { key: 'backend', label: '后端', color: '#30A46C', emoji: '⚙️' },
  { key: 'mobile', label: '移动端', color: '#7F77DD', emoji: '📱' },
  { key: 'devops', label: '运维', color: '#F5A524', emoji: '🚀' },
  { key: 'ai', label: 'AI', color: '#DB2777', emoji: '🧠' },
  { key: 'database', label: '数据库', color: '#0D9488', emoji: '🗄️' },
  { key: 'other', label: '其它', color: '#64748B', emoji: '📌' }
]

function techCategory (key) {
  for (let i = 0; i < TECH_CATEGORIES.length; i++) {
    if (TECH_CATEGORIES[i].key === key) return TECH_CATEGORIES[i]
  }
  return TECH_CATEGORIES[TECH_CATEGORIES.length - 1]
}

/** 待办优先级 */
const PRIORITIES = [
  { key: 'high', label: '高', color: '#E5484D' },
  { key: 'med', label: '中', color: '#F5A524' },
  { key: 'low', label: '低', color: '#30A46C' }
]

function priority (key) {
  for (let i = 0; i < PRIORITIES.length; i++) {
    if (PRIORITIES[i].key === key) return PRIORITIES[i]
  }
  return PRIORITIES[1]
}

/** 待办分类 */
const TODO_CATEGORIES = [
  { key: 'work', label: '工作' },
  { key: 'learning', label: '学习' },
  { key: 'life', label: '生活' }
]

function todoCategory (key) {
  for (let i = 0; i < TODO_CATEGORIES.length; i++) {
    if (TODO_CATEGORIES[i].key === key) return TODO_CATEGORIES[i]
  }
  return TODO_CATEGORIES[0]
}

/** 代码片段语言 */
const LANGUAGES = [
  { key: 'typescript', label: 'TypeScript', color: '#0284C7' },
  { key: 'javascript', label: 'JavaScript', color: '#D97706' },
  { key: 'python', label: 'Python', color: '#30A46C' },
  { key: 'go', label: 'Go', color: '#0D9488' },
  { key: 'rust', label: 'Rust', color: '#B45309' },
  { key: 'java', label: 'Java', color: '#DC2626' },
  { key: 'cpp', label: 'C/C++', color: '#7F77DD' },
  { key: 'sql', label: 'SQL', color: '#6366F1' },
  { key: 'shell', label: 'Shell', color: '#64748B' },
  { key: 'other', label: '其它', color: '#64748B' }
]

function language (key) {
  for (let i = 0; i < LANGUAGES.length; i++) {
    if (LANGUAGES[i].key === key) return LANGUAGES[i]
  }
  return LANGUAGES[LANGUAGES.length - 1]
}

/** 面试题难度 */
const DIFFICULTIES = [
  { key: 'easy', label: '简单', color: '#30A46C' },
  { key: 'med', label: '中等', color: '#F5A524' },
  { key: 'hard', label: '困难', color: '#E5484D' }
]

function difficulty (key) {
  for (let i = 0; i < DIFFICULTIES.length; i++) {
    if (DIFFICULTIES[i].key === key) return DIFFICULTIES[i]
  }
  return DIFFICULTIES[1]
}

/** 资源类型 */
const RESOURCE_TYPES = [
  { key: 'article', label: '文章' },
  { key: 'video', label: '视频' },
  { key: 'book', label: '书籍' },
  { key: 'repo', label: '仓库' },
  { key: 'tool', label: '工具' },
  { key: 'doc', label: '文档' }
]

function resourceType (key) {
  for (let i = 0; i < RESOURCE_TYPES.length; i++) {
    if (RESOURCE_TYPES[i].key === key) return RESOURCE_TYPES[i]
  }
  return RESOURCE_TYPES[0]
}

/** 读书状态 */
const READING_STATUS = [
  { key: 'wishlist', label: '想读', color: '#64748B' },
  { key: 'reading', label: '在读', color: '#0284C7' },
  { key: 'finished', label: '已读', color: '#30A46C' },
  { key: 'paused', label: '搁置', color: '#F5A524' }
]

function readingStatus (key) {
  for (let i = 0; i < READING_STATUS.length; i++) {
    if (READING_STATUS[i].key === key) return READING_STATUS[i]
  }
  return READING_STATUS[0]
}

/** 运动类型 */
const FITNESS_TYPES = [
  { key: 'cardio', label: '有氧' },
  { key: 'strength', label: '力量' },
  { key: 'flexibility', label: '柔韧' },
  { key: 'sports', label: '球类' },
  { key: 'other', label: '其它' }
]

function fitnessType (key) {
  for (let i = 0; i < FITNESS_TYPES.length; i++) {
    if (FITNESS_TYPES[i].key === key) return FITNESS_TYPES[i]
  }
  return FITNESS_TYPES[4]
}

/** 心情等级 1-5 */
const MOOD_LEVELS = [
  { level: 1, label: '很差', color: '#E5484D', emoji: '😞' },
  { level: 2, label: '偏差', color: '#F5A524', emoji: '😕' },
  { level: 3, label: '一般', color: '#64748B', emoji: '😐' },
  { level: 4, label: '不错', color: '#0284C7', emoji: '🙂' },
  { level: 5, label: '很好', color: '#30A46C', emoji: '😄' }
]

function moodLevel (level) {
  // 注意：0 是合法数值（应夹取到 1），不能用 `|| 3` 判空
  const n = Number(level)
  const safe = isNaN(n) ? 3 : Math.max(1, Math.min(5, n))
  return MOOD_LEVELS[safe - 1]
}

/** 项目状态 */
const PROJECT_STATUS = [
  { key: 'active', label: '进行中', color: '#30A46C' },
  { key: 'wip', label: '开发中', color: '#F5A524' },
  { key: 'archived', label: '已归档', color: '#64748B' }
]

function projectStatus (key) {
  for (let i = 0; i < PROJECT_STATUS.length; i++) {
    if (PROJECT_STATUS[i].key === key) return PROJECT_STATUS[i]
  }
  return PROJECT_STATUS[0]
}

/** 主题模式 */
const THEME_MODES = [
  { key: 'light', label: '浅色' },
  { key: 'dark', label: '深色' },
  { key: 'auto', label: '跟随系统' }
]

/** 可选主色（5 种） */
const PRIMARY_COLORS = [
  { key: 'sky', label: '天蓝', light: '#0284C7', dark: '#38BDF8' },
  { key: 'violet', label: '靛紫', light: '#6366F1', dark: '#818CF8' },
  { key: 'teal', label: '青碧', light: '#0D9488', dark: '#2DD4BF' },
  { key: 'amber', label: '琥珀', light: '#D97706', dark: '#FBBF24' },
  { key: 'rose', label: '玫红', light: '#DB2777', dark: '#F472B6' }
]

module.exports = {
  TECH_CATEGORIES: TECH_CATEGORIES,
  techCategory: techCategory,
  PRIORITIES: PRIORITIES,
  priority: priority,
  TODO_CATEGORIES: TODO_CATEGORIES,
  todoCategory: todoCategory,
  LANGUAGES: LANGUAGES,
  language: language,
  DIFFICULTIES: DIFFICULTIES,
  difficulty: difficulty,
  RESOURCE_TYPES: RESOURCE_TYPES,
  resourceType: resourceType,
  READING_STATUS: READING_STATUS,
  readingStatus: readingStatus,
  FITNESS_TYPES: FITNESS_TYPES,
  fitnessType: fitnessType,
  MOOD_LEVELS: MOOD_LEVELS,
  moodLevel: moodLevel,
  PROJECT_STATUS: PROJECT_STATUS,
  projectStatus: projectStatus,
  THEME_MODES: THEME_MODES,
  PRIMARY_COLORS: PRIMARY_COLORS
}
