/**
 * storage.js —— 数据持久化层（基于 localStorage）
 *
 * 键名统一前缀 games_，含版本迁移机制。
 *
 * 与小程序版的差异只有底层：wx.getStorageSync / setStorageSync / removeStorageSync
 * 换成 localStorage.getItem / setItem / removeItem，键名与全部业务逻辑一字不改。
 * games_ 前缀与其它模块的 db: 前缀不冲突。
 */

const STORAGE_PREFIX = 'games_'
const CURRENT_VERSION = 1
const VERSION_KEY = STORAGE_PREFIX + 'version'

/** 数独默认章节数据 */
function defaultSudokuData () {
  return {
    bestScore: 0,
    chapters: [
      { id: 1, unlocked: true, stars: {} },
      { id: 2, unlocked: false, stars: {} },
      { id: 3, unlocked: false, stars: {} },
      { id: 4, unlocked: false, stars: {} },
      { id: 5, unlocked: false, stars: {} }
    ]
  }
}

/** 组装存储键名 */
function getKey (game) {
  return STORAGE_PREFIX + game
}

/**
 * 读取（JSON 解析，失败回退默认值）
 *
 * localStorage 对不存在的键返回 null，小程序版返回 ''；两者在真值判断下等价。
 */
function getData (game, defaultValue) {
  try {
    const raw = localStorage.getItem(getKey(game))
    if (raw === null || raw === undefined || raw === '') return defaultValue
    return typeof raw === 'string' ? JSON.parse(raw) : raw
  } catch (e) {
    console.warn('[storage] 读取 ' + game + ' 失败:', e)
  }
  return defaultValue
}

/** 写入（JSON 序列化） */
function setData (game, data) {
  try {
    localStorage.setItem(getKey(game), JSON.stringify(data))
    return true
  } catch (e) {
    console.warn('[storage] 保存 ' + game + ' 失败:', e)
    return false
  }
}

// ==================== 贪吃蛇 ====================

function getSnakeBestScore () {
  const d = getData('snake', { bestScore: 0 })
  return d.bestScore || 0
}

function updateSnakeBestScore (score) {
  if (score > getSnakeBestScore()) {
    return setData('snake', { bestScore: score })
  }
  return true
}

// ==================== 2048 ====================

function get2048BestScore () {
  const d = getData('game2048', { bestScore: 0 })
  return d.bestScore || 0
}

function update2048BestScore (score) {
  if (score > get2048BestScore()) {
    return setData('game2048', { bestScore: score })
  }
  return true
}

// ==================== 数独 ====================

function getSudokuData () {
  return getData('sudoku', defaultSudokuData())
}

function setSudokuData (data) {
  return setData('sudoku', data)
}

/** 设置某关卡星数 */
function setSudokuChapterStars (chapterId, levelId, stars) {
  const data = getSudokuData()
  for (let i = 0; i < data.chapters.length; i++) {
    if (data.chapters[i].id === chapterId) {
      data.chapters[i].stars[levelId] = stars
      return setSudokuData(data)
    }
  }
  return false
}

/** 解锁某章节 */
function unlockSudokuChapter (chapterId) {
  const data = getSudokuData()
  for (let i = 0; i < data.chapters.length; i++) {
    if (data.chapters[i].id === chapterId) {
      data.chapters[i].unlocked = true
      return setSudokuData(data)
    }
  }
  return false
}

/** 数独已获得总星数 */
function getSudokuTotalStars () {
  const data = getSudokuData()
  let total = 0
  data.chapters.forEach(chapter => {
    Object.keys(chapter.stars).forEach(k => { total += chapter.stars[k] || 0 })
  })
  return total
}

// ==================== 消消乐 ====================

function getMatch3Data () {
  const d = getData('match3', {})
  if (!d.chapters) d.chapters = {}
  return d
}

function setMatch3Data (data) {
  return setData('match3', data)
}

/** 设置消消乐关卡星数 */
function setMatch3Stars (level, stars) {
  const data = getMatch3Data()
  const chapter = Math.ceil(level / 10)
  const key = 'c' + chapter
  if (!data.chapters[key]) data.chapters[key] = { unlocked: true, stars: {} }
  data.chapters[key].stars[level] = stars
  return setMatch3Data(data)
}

/** 消消乐已解锁章节号集合 */
function getMatch3UnlockedChapters () {
  const data = getMatch3Data()
  const set = new Set([1])
  Object.keys(data.chapters).forEach(key => {
    if (data.chapters[key].unlocked) set.add(Number(key.replace('c', '')))
  })
  return set
}

/** 消消乐各关卡星数映射 */
function getMatch3EarnedStars () {
  const data = getMatch3Data()
  const flat = {}
  Object.keys(data.chapters).forEach(key => {
    const stars = data.chapters[key].stars || {}
    Object.keys(stars).forEach(lv => { flat[lv] = stars[lv] })
  })
  return flat
}

/** 消消乐总星数 */
function getMatch3TotalStars () {
  const earned = getMatch3EarnedStars()
  let total = 0
  Object.keys(earned).forEach(k => { total += earned[k] || 0 })
  return total
}

/** 解锁某章节 */
function unlockMatch3Chapter (chapterId) {
  const data = getMatch3Data()
  const key = 'c' + chapterId
  if (!data.chapters[key]) data.chapters[key] = { unlocked: false, stars: {} }
  data.chapters[key].unlocked = true
  return setMatch3Data(data)
}

// ==================== 成就系统 ====================

function getAchievements () {
  return getData('achievements', { unlocked: [], progress: {} })
}

function setAchievements (data) {
  return setData('achievements', data)
}

/**
 * 解锁成就（幂等：已解锁返回 null）
 * @returns {object|null} 新解锁的成就
 */
function unlockAchievement (id) {
  const data = getAchievements()
  const already = data.unlocked.some(a => a.id === id)
  if (already) return null

  const achievement = { id, unlockedAt: Date.now() }
  data.unlocked.push(achievement)
  setAchievements(data)
  return achievement
}

/** 累加成就进度（只增不减） */
function recordAchievementProgress (id, delta) {
  const data = getAchievements()
  const current = data.progress[id] || 0
  data.progress[id] = Math.max(current, current + delta)
  return setAchievements(data)
}

/** 成就是否已解锁 */
function isAchievementUnlocked (id) {
  const data = getAchievements()
  return data.unlocked.some(a => a.id === id)
}

/** 已解锁数量 */
function getUnlockedCount () {
  return getAchievements().unlocked.length
}

// ==================== 汇总数据（供首页展示） ====================

/** 各游戏最高分 / 进度汇总 */
function getOverview () {
  return {
    snakeBest: getSnakeBestScore(),
    g2048Best: get2048BestScore(),
    sudokuStars: getSudokuTotalStars(),
    match3Stars: getMatch3TotalStars(),
    achievementUnlocked: getUnlockedCount()
  }
}

/** 清空全部游戏数据（只清 games_ 前缀的键） */
function resetAll () {
  const keys = []
  try {
    for (let i = 0; i < localStorage.length; i++) {
      const k = localStorage.key(i)
      if (k !== null && k !== undefined && k.indexOf(STORAGE_PREFIX) === 0) keys.push(k)
    }
  } catch (e) {
    console.warn('[storage] 列举键失败:', e)
  }
  keys.forEach(function (k) {
    try { localStorage.removeItem(k) } catch (e) { /* 忽略 */ }
  })
  migrate()
}

/** 版本迁移 + 初始化 */
function migrate () {
  try {
    const stored = localStorage.getItem(VERSION_KEY)
    const version = stored ? JSON.parse(stored) : 0
    if (version >= CURRENT_VERSION) return true

    if (version < 1) {
      const defaults = {
        snake: { bestScore: 0 },
        game2048: { bestScore: 0 },
        sudoku: defaultSudokuData(),
        match3: { chapters: {} },
        achievements: { unlocked: [], progress: {} }
      }
      Object.keys(defaults).forEach(game => {
        const raw = localStorage.getItem(getKey(game))
        if (raw === null || raw === undefined || raw === '') setData(game, defaults[game])
      })
    }

    localStorage.setItem(VERSION_KEY, JSON.stringify(CURRENT_VERSION))
    return true
  } catch (e) {
    console.error('[storage] 数据迁移失败:', e)
    return false
  }
}

module.exports = {
  getSnakeBestScore,
  updateSnakeBestScore,
  get2048BestScore,
  update2048BestScore,
  getSudokuData,
  setSudokuData,
  setSudokuChapterStars,
  unlockSudokuChapter,
  getSudokuTotalStars,
  getMatch3Data,
  setMatch3Data,
  setMatch3Stars,
  getMatch3UnlockedChapters,
  getMatch3EarnedStars,
  getMatch3TotalStars,
  unlockMatch3Chapter,
  getAchievements,
  setAchievements,
  unlockAchievement,
  recordAchievementProgress,
  isAchievementUnlocked,
  getUnlockedCount,
  getOverview,
  resetAll,
  migrate
}
