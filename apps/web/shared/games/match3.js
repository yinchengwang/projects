/**
 * match3.js —— 开心消消乐核心逻辑
 *
 * 原生小程序版。功能对照原 engine/apps/web/games-mini-program：
 * 1. 章节系统（普通 / 困难 / 超难关）
 * 2. 特效系统（直线 / 爆炸 / 彩虹）
 * 3. 障碍物系统（冰块 / 蜘蛛网 / 石头）
 * 4. 特殊元素（钥匙 / 宝箱 / 松鼠 / 发条鸟 / 蜗牛 / 金币）
 * 5. 章节解锁 + 星星评价 + 步数目标
 */

const BOARD_SIZE = 9

// ==================== 常量定义 ====================

/** 宝石配色 */
const GEM_COLORS = {
  0: { primary: '#FFD93D', secondary: '#F6B93B' },
  1: { primary: '#26DE81', secondary: '#20BF6B' },
  2: { primary: '#FD79A8', secondary: '#E84393' },
  3: { primary: '#A55EEA', secondary: '#8854D0' },
  4: { primary: '#45AAF2', secondary: '#2E86DE' },
  5: { primary: '#F78FB3', secondary: '#F368E0' }
}

/** 特效类型 */
const SpecialType = {
  ROW_CLEAR: 'row',
  COL_CLEAR: 'col',
  BOMB: 'bomb',
  RAINBOW: 'rainbow'
}

/** 障碍物定义（hits = 血量） */
const OBSTACLE_INFO = {
  ice: { emoji: '❄️', hits: 1, desc: '冰块' },
  stone: { emoji: '🪨', hits: 3, desc: '石头' },
  web: { emoji: '🕸️', hits: 2, desc: '蜘蛛网' }
}

/** 障碍物渲染配色 */
const OBSTACLE_RENDER_INFO = {
  ice: { color: 'rgba(127,201,249,0.55)', borderColor: '#7FC9F9' },
  stone: { color: 'rgba(123,111,92,0.55)', borderColor: '#7B6F5C' },
  web: { color: 'rgba(158,158,158,0.45)', borderColor: '#9E9E9E' }
}

/** 特殊元素定义 */
const ELEMENT_INFO = {
  key: { emoji: '🔑', desc: '钥匙' },
  treasure: { emoji: '📦', desc: '宝箱' },
  squirrel: { emoji: '🐿️', desc: '松鼠' },
  bird: { emoji: '🐦', desc: '发条鸟' },
  snail: { emoji: '🐌', desc: '蜗牛' },
  coin: { emoji: '🪙', desc: '金币' }
}

/** 关卡类型 */
const LEVEL_TYPE_INFO = {
  normal: { name: '普通关', color: '#2E86DE', bgColor: '#87CEEB' },
  difficult: { name: '困难关', color: '#EE5A5A', bgColor: '#FFB6C1' },
  boss: { name: '超难关', color: '#9400D3', bgColor: '#4B0082' }
}

/** 章节表：6 章共 50 关 */
const CHAPTERS = [
  { id: 1, name: '魔法森林', type: 'normal', levels: [1, 9], starsRequired: 0, description: '初入魔法森林' },
  { id: 2, name: '精灵花园', type: 'normal', levels: [10, 18], starsRequired: 15, description: '精灵们的秘密花园' },
  { id: 3, name: '星光原野', type: 'normal', levels: [19, 27], starsRequired: 30, description: '星光闪烁的原野' },
  { id: 4, name: '暗夜森林', type: 'difficult', levels: [28, 36], starsRequired: 50, description: '暗夜笼罩的森林' },
  { id: 5, name: '冰霜峡谷', type: 'difficult', levels: [37, 45], starsRequired: 75, description: '寒冷的冰霜峡谷' },
  { id: 6, name: 'BOSS挑战', type: 'boss', levels: [46, 50], starsRequired: 100, description: '最终BOSS关卡' }
]

// ==================== 章节工具 ====================

/** 关卡 → 所属章节 */
function getChapterByLevel (level) {
  for (let i = 0; i < CHAPTERS.length; i++) {
    const c = CHAPTERS[i]
    if (level >= c.levels[0] && level <= c.levels[1]) return c
  }
  return null
}

/** 章节已获得总星数 */
function getChapterTotalStars (chapterId, earnedStars) {
  const chapter = CHAPTERS.filter(c => c.id === chapterId)[0]
  if (!chapter) return 0
  let total = 0
  for (let lv = chapter.levels[0]; lv <= chapter.levels[1]; lv++) {
    total += (earnedStars && earnedStars[lv]) || 0
  }
  return total
}

/** 章节是否解锁 */
function isChapterUnlocked (chapter, totalStars, unlockedChapters) {
  if (chapter.id === 1) return true
  return unlockedChapters.has(chapter.id - 1) || totalStars >= chapter.starsRequired
}

// ==================== 单元格 ====================

/** 创建空格子 */
function createEmptyCell () {
  return {
    gem: null,
    special: null,
    obstacle: null,
    element: null,
    obstacleHp: 0,
    locked: false
  }
}

/** 创建宝石格子 */
function createGemCell (gem) {
  return {
    gem,
    special: null,
    obstacle: null,
    element: null,
    obstacleHp: 0,
    locked: false
  }
}

/** 对障碍物造成伤害 */
function damageObstacle (cell, amount) {
  if (!cell.obstacle) return
  cell.obstacleHp = Math.max(0, cell.obstacleHp - (amount === undefined ? 1 : amount))
}

/** 障碍物是否已摧毁 */
function isObstacleDestroyed (cell) {
  return cell.obstacle !== null && cell.obstacleHp <= 0
}

/** 清除障碍物（含 locked） */
function clearObstacle (cell) {
  cell.obstacle = null
  cell.obstacleHp = 0
  cell.locked = false
}

/** 障碍物渲染信息 */
function getObstacleRenderInfo (cell) {
  if (!cell.obstacle) return null
  const meta = OBSTACLE_INFO[cell.obstacle]
  const render = OBSTACLE_RENDER_INFO[cell.obstacle]
  return {
    type: cell.obstacle,
    emoji: meta.emoji,
    hp: cell.obstacleHp,
    maxHp: meta.hits,
    desc: meta.desc,
    color: render.color,
    borderColor: render.borderColor,
    destroyed: cell.obstacleHp <= 0
  }
}

/** 拷贝棋盘 */
function copyBoard (board) {
  return board.map(row => row.map(cell => Object.assign({}, cell)))
}

/** 随机宝石类型 */
function randomGem () {
  return Math.floor(Math.random() * 6)
}

/** 该位置放 gem 是否会立刻成三连 */
function wouldMatch (board, row, col, gem) {
  let hCount = 1
  for (let c = col - 1; c >= 0; c--) {
    if (!board[row][c] || board[row][c].gem !== gem) break
    hCount++
  }
  for (let c = col + 1; c < BOARD_SIZE; c++) {
    if (!board[row][c] || board[row][c].gem !== gem) break
    hCount++
  }
  if (hCount >= 3) return true

  let vCount = 1
  for (let r = row - 1; r >= 0; r--) {
    if (!board[r] || !board[r][col] || board[r][col].gem !== gem) break
    vCount++
  }
  for (let r = row + 1; r < BOARD_SIZE; r++) {
    if (!board[r] || !board[r][col] || board[r][col].gem !== gem) break
    vCount++
  }
  return vCount >= 3
}

/** 创建初始棋盘（避免开局即成三连） */
function createBoard () {
  const board = []
  for (let row = 0; row < BOARD_SIZE; row++) {
    board[row] = []
    for (let col = 0; col < BOARD_SIZE; col++) {
      let gem = randomGem()
      let attempts = 0
      while (attempts < 100 && wouldMatch(board, row, col, gem)) {
        gem = randomGem()
        attempts++
      }
      board[row][col] = createGemCell(gem)
    }
  }
  return board
}

/**
 * 查找所有匹配
 * @returns {Array<{cells: string[], direction: string}>}
 */
function findMatches (board) {
  const results = []

  // 水平
  for (let row = 0; row < BOARD_SIZE; row++) {
    let col = 0
    while (col < BOARD_SIZE) {
      const cell = board[row][col]
      if (cell.gem !== null && !cell.obstacle) {
        let end = col
        while (end + 1 < BOARD_SIZE && board[row][end + 1].gem === cell.gem && !board[row][end + 1].obstacle) end++
        if (end - col + 1 >= 3) {
          const cells = []
          for (let c = col; c <= end; c++) cells.push(row + ',' + c)
          results.push({ cells, direction: 'h' })
        }
        col = end + 1
      } else {
        col++
      }
    }
  }

  // 垂直
  for (let col = 0; col < BOARD_SIZE; col++) {
    let row = 0
    while (row < BOARD_SIZE) {
      const cell = board[row][col]
      if (cell.gem !== null && !cell.obstacle) {
        let end = row
        while (end + 1 < BOARD_SIZE && board[end + 1][col].gem === cell.gem && !board[end + 1][col].obstacle) end++
        if (end - row + 1 >= 3) {
          const cells = []
          for (let r = row; r <= end; r++) cells.push(r + ',' + col)
          results.push({ cells, direction: 'v' })
        }
        row = end + 1
      } else {
        row++
      }
    }
  }

  return results
}

/**
 * 分析匹配形状 → 决定生成什么特效
 * @returns {{line4: string[], line5: string[], intersections: string[]}}
 */
function analyzeMatchShape (matches) {
  const line4 = []
  const line5 = []
  const intersections = []
  const countMap = {}

  for (let i = 0; i < matches.length; i++) {
    const cells = matches[i].cells
    if (cells.length === 4) {
      line4.push.apply(line4, cells)
    } else if (cells.length >= 5) {
      line5.push.apply(line5, cells)
    }
    for (let j = 0; j < cells.length; j++) {
      countMap[cells[j]] = (countMap[cells[j]] || 0) + 1
    }
  }

  Object.keys(countMap).forEach(pos => {
    if (countMap[pos] >= 2) intersections.push(pos)
  })

  return { line4, line5, intersections }
}

/**
 * 触发特效，清除指定范围内的格子
 * @returns {Set<string>} 被消除的位置
 */
function triggerSpecial (board, pos, special) {
  const parts = pos.split(',')
  const row = Number(parts[0])
  const col = Number(parts[1])
  const removed = new Set()

  // 石头不可被特效清除；冰块 / 蜘蛛网可以
  const passable = (cell) => !cell.obstacle || cell.obstacle === 'ice' || cell.obstacle === 'web'

  switch (special) {
    case SpecialType.ROW_CLEAR:
      for (let c = 0; c < BOARD_SIZE; c++) {
        if (passable(board[row][c])) removed.add(row + ',' + c)
      }
      break

    case SpecialType.COL_CLEAR:
      for (let r = 0; r < BOARD_SIZE; r++) {
        if (passable(board[r][col])) removed.add(r + ',' + col)
      }
      break

    case SpecialType.BOMB:
      for (let dr = -1; dr <= 1; dr++) {
        for (let dc = -1; dc <= 1; dc++) {
          const nr = row + dr
          const nc = col + dc
          if (nr >= 0 && nr < BOARD_SIZE && nc >= 0 && nc < BOARD_SIZE) {
            if (passable(board[nr][nc])) removed.add(nr + ',' + nc)
          }
        }
      }
      break

    case SpecialType.RAINBOW:
      const gemType = board[row][col].gem
      for (let r = 0; r < BOARD_SIZE; r++) {
        for (let c = 0; c < BOARD_SIZE; c++) {
          if (board[r][c].gem === gemType) removed.add(r + ',' + c)
        }
      }
      break
  }

  return removed
}

/** 宝石下落 + 顶部补充 */
function dropGems (board) {
  for (let col = 0; col < BOARD_SIZE; col++) {
    let writeRow = BOARD_SIZE - 1
    for (let row = BOARD_SIZE - 1; row >= 0; row--) {
      if (board[row][col].gem !== null || board[row][col].obstacle !== null) {
        if (row !== writeRow) {
          board[writeRow][col] = Object.assign({}, board[row][col])
          board[row][col] = createEmptyCell()
        }
        writeRow--
      }
    }
    for (let row = writeRow; row >= 0; row--) {
      board[row][col] = createGemCell(randomGem())
    }
  }
}

/** 交换是否合法（必须相邻且都有宝石） */
function isValidSwap (board, pos1, pos2) {
  const a = pos1.split(',').map(Number)
  const b = pos2.split(',').map(Number)
  const r1 = a[0]; const c1 = a[1]
  const r2 = b[0]; const c2 = b[1]

  // 注意：gem === 0 是合法的宝石类型（黄色），不能用真值判断
  if (board[r1][c1].gem === null || board[r2][c2].gem === null) return false
  if (Math.abs(r1 - r2) + Math.abs(c1 - c2) !== 1) return false
  return true
}

/** 执行交换 */
function doSwap (board, pos1, pos2) {
  const a = pos1.split(',').map(Number)
  const b = pos2.split(',').map(Number)
  const tmp = board[a[0]][a[1]]
  board[a[0]][a[1]] = board[b[0]][b[1]]
  board[b[0]][b[1]] = tmp
}

/** 计分：基础分 × 连击加成 */
function calculateScore (removedCount, combo) {
  const base = removedCount * 10
  const comboBonus = combo > 1 ? combo * 0.5 : 0
  return Math.floor(base * (1 + comboBonus))
}

/** 星级：按剩余步数比例 */
function calculateStars (score, targetScore, movesLeft, totalMoves) {
  if (score < targetScore) return 0
  const ratio = movesLeft / totalMoves
  if (ratio >= 0.5) return 3
  if (ratio >= 0.25) return 2
  return 1
}

// ==================== 关卡配置 ====================

/** 预定义关卡（其余关卡由 getLevelConfig 动态生成） */
const LEVEL_CONFIGS = {
  1: { moves: 25, targetScore: 1000, obstacles: [{ type: 'ice', count: 3 }], elements: [], layout: 'open' },
  2: { moves: 25, targetScore: 1200, obstacles: [{ type: 'ice', count: 5 }], elements: [], layout: 'open' },
  3: { moves: 25, targetScore: 1400, obstacles: [{ type: 'ice', count: 7 }], elements: [], layout: 'open' },
  5: { moves: 28, targetScore: 1800, obstacles: [{ type: 'ice', count: 10 }], elements: [{ type: 'coin', count: 3 }], layout: 'open' },
  9: { moves: 30, targetScore: 2500, obstacles: [{ type: 'ice', count: 15 }], elements: [{ type: 'coin', count: 5 }], layout: 'open' },
  10: { moves: 25, targetScore: 2800, obstacles: [{ type: 'ice', count: 8 }, { type: 'web', count: 5 }], elements: [{ type: 'key', count: 1 }], layout: 'open' },
  15: { moves: 28, targetScore: 3500, obstacles: [{ type: 'ice', count: 10 }, { type: 'web', count: 8 }], elements: [{ type: 'key', count: 2 }, { type: 'treasure', count: 1 }], layout: 'partial' },
  18: { moves: 30, targetScore: 4500, obstacles: [{ type: 'ice', count: 12 }, { type: 'web', count: 10 }, { type: 'web', count: 3 }], elements: [{ type: 'key', count: 2 }, { type: 'squirrel', count: 1 }], layout: 'partial' },
  20: { moves: 28, targetScore: 5000, obstacles: [{ type: 'ice', count: 10 }, { type: 'web', count: 8 }, { type: 'web', count: 5 }], elements: [{ type: 'bird', count: 1 }, { type: 'coin', count: 5 }], layout: 'partial' },
  25: { moves: 30, targetScore: 6000, obstacles: [{ type: 'ice', count: 12 }, { type: 'web', count: 10 }, { type: 'web', count: 6 }, { type: 'stone', count: 4 }], elements: [{ type: 'bird', count: 2 }, { type: 'snail', count: 1 }, { type: 'treasure', count: 2 }], layout: 'partial' },
  27: { moves: 32, targetScore: 8000, obstacles: [{ type: 'ice', count: 15 }, { type: 'web', count: 12 }, { type: 'web', count: 8 }, { type: 'stone', count: 6 }], elements: [{ type: 'bird', count: 2 }, { type: 'snail', count: 2 }, { type: 'squirrel', count: 1 }, { type: 'treasure', count: 2 }], layout: 'restricted' },
  30: { moves: 22, targetScore: 10000, obstacles: [{ type: 'ice', count: 20 }, { type: 'web', count: 15 }, { type: 'web', count: 10 }, { type: 'stone', count: 8 }], elements: [{ type: 'bird', count: 3 }, { type: 'snail', count: 2 }, { type: 'key', count: 3 }], layout: 'restricted' },
  40: { moves: 20, targetScore: 15000, obstacles: [{ type: 'ice', count: 25 }, { type: 'web', count: 20 }, { type: 'web', count: 12 }, { type: 'stone', count: 10 }], elements: [{ type: 'bird', count: 3 }, { type: 'snail', count: 3 }, { type: 'squirrel', count: 2 }, { type: 'key', count: 4 }], layout: 'restricted' },
  50: { moves: 35, targetScore: 30000, obstacles: [{ type: 'ice', count: 30 }, { type: 'web', count: 25 }, { type: 'web', count: 15 }, { type: 'stone', count: 15 }], elements: [{ type: 'bird', count: 5 }, { type: 'snail', count: 4 }, { type: 'squirrel', count: 3 }, { type: 'key', count: 5 }, { type: 'treasure', count: 5 }], layout: 'restricted' }
}

/** 动态生成障碍物配置 */
function generateObstacles (level, type) {
  const obstacles = []
  const difficulty = type === 'boss' ? 3 : type === 'difficult' ? 2 : 1

  const iceCount = Math.min(25, Math.floor(level * 0.8 * difficulty))
  if (iceCount > 0) obstacles.push({ type: 'ice', count: iceCount })

  if (level >= 10) {
    const webCount = Math.min(20, Math.floor((level - 10) * 0.5 * difficulty))
    if (webCount > 0) obstacles.push({ type: 'web', count: webCount })
  }
  if (level >= 20) {
    const webCount2 = Math.min(12, Math.floor((level - 20) * 0.3 * difficulty))
    if (webCount2 > 0) obstacles.push({ type: 'web', count: webCount2 })
  }
  if (level >= 30) {
    const stoneCount = Math.min(10, Math.floor((level - 30) * 0.2 * difficulty))
    if (stoneCount > 0) obstacles.push({ type: 'stone', count: stoneCount })
  }
  return obstacles
}

/** 动态生成特殊元素配置 */
function generateElements (level, type) {
  const elements = []
  const difficulty = type === 'boss' ? 3 : type === 'difficult' ? 2 : 1

  if (level >= 5) {
    const c = Math.min(8, Math.floor(level * 0.2 * difficulty))
    if (c > 0) elements.push({ type: 'coin', count: c })
  }
  if (level >= 15) {
    const c = Math.min(4, Math.floor((level - 15) * 0.1 * difficulty) + 1)
    if (c > 0) elements.push({ type: 'key', count: c })
  }
  if (level >= 25) {
    const c = Math.min(3, Math.floor((level - 25) * 0.08 * difficulty) + 1)
    if (c > 0) elements.push({ type: 'treasure', count: c })
  }
  if (level >= 35) {
    const c = Math.min(2, Math.floor((level - 35) * 0.05 * difficulty) + 1)
    if (c > 0) elements.push({ type: 'squirrel', count: c })
  }
  if (level >= 40) {
    const c = Math.min(3, Math.floor((level - 40) * 0.05 * difficulty) + 1)
    if (c > 0) elements.push({ type: 'bird', count: c })
  }
  if (level >= 46) {
    const c = Math.min(3, Math.floor((level - 46) * 0.04 * difficulty) + 1)
    if (c > 0) elements.push({ type: 'snail', count: c })
  }
  return elements
}

/** 生成关卡目标（得分 + 各特殊元素收集） */
function buildGoals (config) {
  const goals = [{ type: 'score', target: config.targetScore, emoji: '⭐' }]
  for (let i = 0; i < config.elements.length; i++) {
    const el = config.elements[i]
    if (el.count > 0) {
      const emoji = (ELEMENT_INFO[el.type] && ELEMENT_INFO[el.type].emoji) || '❓'
      goals.push({ type: el.type, target: el.count, emoji })
    }
  }
  return goals
}

/** 获取关卡配置 */
function getLevelConfig (level) {
  const chapter = getChapterByLevel(level)
  const chapterId = chapter ? chapter.id : 1
  const type = chapter ? chapter.type : 'normal'

  if (LEVEL_CONFIGS[level]) {
    const preset = LEVEL_CONFIGS[level]
    const config = {
      level,
      chapter: chapterId,
      type,
      moves: preset.moves,
      targetScore: preset.targetScore,
      obstacles: preset.obstacles,
      elements: preset.elements,
      layout: preset.layout
    }
    config.goals = buildGoals(config)
    return config
  }

  const baseMoves = type === 'boss' ? 30 : type === 'difficult' ? 25 : 30
  const movesReduction = Math.floor((level - 1) / 10)
  const moves = Math.max(15, baseMoves - movesReduction)

  const baseScore = type === 'boss' ? 20000 : type === 'difficult' ? 8000 : 1000
  const targetScore = baseScore + level * (type === 'boss' ? 500 : type === 'difficult' ? 300 : 100)

  const config = {
    level,
    chapter: chapterId,
    type,
    moves,
    targetScore,
    obstacles: generateObstacles(level, type),
    elements: generateElements(level, type),
    layout: level % 10 === 0 ? 'restricted' : level % 5 === 0 ? 'partial' : 'open'
  }
  config.goals = buildGoals(config)
  return config
}

/** 估算关卡可通关性（20% 容差） */
function verifyLevelBeatable (config) {
  let obstaclePenalty = 0
  for (let i = 0; i < config.obstacles.length; i++) {
    const obs = config.obstacles[i]
    if (obs.type === 'ice') obstaclePenalty += obs.count * 1
    if (obs.type === 'web') obstaclePenalty += obs.count * 1.5
    if (obs.type === 'stone') obstaclePenalty += obs.count * 3
  }
  let elementPenalty = 0
  for (let i = 0; i < config.elements.length; i++) {
    elementPenalty += config.elements[i].count * 2
  }
  const effectiveMoves = config.moves - obstaclePenalty - elementPenalty
  return effectiveMoves * 50 >= config.targetScore * 0.8
}

// ==================== 游戏状态 ====================

/** 找一个可放置的位置 */
function findEmptyPosition (board, allowGem) {
  const positions = []
  for (let row = 0; row < BOARD_SIZE; row++) {
    for (let col = 0; col < BOARD_SIZE; col++) {
      if (allowGem) {
        positions.push(row + ',' + col)
      } else if (board[row][col].gem === null && !board[row][col].obstacle && !board[row][col].element) {
        positions.push(row + ',' + col)
      }
    }
  }
  if (positions.length === 0) return null
  return positions[Math.floor(Math.random() * positions.length)]
}

/** 放置障碍物 */
function placeObstacles (board, obstacles) {
  for (let i = 0; i < obstacles.length; i++) {
    const obs = obstacles[i]
    if (!obs.type) continue
    for (let n = 0; n < obs.count; n++) {
      const pos = findEmptyPosition(board, true)
      if (pos) {
        const p = pos.split(',').map(Number)
        board[p[0]][p[1]].obstacle = obs.type
        board[p[0]][p[1]].obstacleHp = OBSTACLE_INFO[obs.type].hits
        board[p[0]][p[1]].gem = randomGem()
      }
    }
  }
}

/** 放置特殊元素 */
function placeElements (board, elements) {
  for (let i = 0; i < elements.length; i++) {
    const el = elements[i]
    for (let n = 0; n < el.count; n++) {
      const pos = findEmptyPosition(board, false)
      if (pos) {
        const p = pos.split(',').map(Number)
        board[p[0]][p[1]].element = el.type
      }
    }
  }
}

/** 创建关卡游戏状态 */
function createGameState (level, earnedStars, unlockedChapters) {
  const stars = earnedStars || {}
  const unlocked = unlockedChapters || new Set([1])

  const config = getLevelConfig(level)
  const board = createBoard()
  placeObstacles(board, config.obstacles)
  placeElements(board, config.elements)

  const goals = config.goals.map(g => ({ type: g.type, current: 0, target: g.target }))

  const snails = []
  for (let i = 0; i < config.elements.length; i++) {
    if (config.elements[i].type !== 'snail') continue
    for (let n = 0; n < config.elements[i].count; n++) {
      snails.push({
        row: Math.floor(Math.random() * 3),
        col: Math.floor(Math.random() * BOARD_SIZE),
        movesLeft: 15 + Math.floor(Math.random() * 10)
      })
    }
  }

  let totalStars = 0
  Object.keys(stars).forEach(k => { totalStars += stars[k] || 0 })

  return {
    level,
    chapter: config.chapter,
    board,
    moves: config.moves,
    maxMoves: config.moves,
    score: 0,
    collected: {},
    goals,
    status: 'playing',
    stars: 0,
    snails,
    unlockedChapters: unlocked,
    earnedStars: stars,
    totalStars
  }
}

/** 每步推进蜗牛；蜗牛落出棋盘即失败 */
function updateSnails (state) {
  const snailRemoved = []
  let gameOver = false

  for (let i = 0; i < state.snails.length; i++) {
    const snail = state.snails[i]
    snail.movesLeft--
    snail.row++
    if (snail.row >= BOARD_SIZE) {
      snailRemoved.push((snail.row - 1) + ',' + snail.col)
      gameOver = true
    }
  }
  return { snailRemoved, gameOver }
}

/** 所有目标是否达成 */
function checkGoalsComplete (state) {
  return state.goals.every(g => g.current >= g.target)
}

/** 特殊元素触发效果 */
function processElementEffect (board, pos, element) {
  const removed = new Set()
  const p = pos.split(',').map(Number)
  const row = p[0]
  const col = p[1]

  switch (element) {
    case 'bird': // 发条鸟：消除整列
      for (let r = 0; r < BOARD_SIZE; r++) {
        if (!board[r][col].obstacle || board[r][col].obstacle === 'ice') {
          removed.add(r + ',' + col)
        }
      }
      break

    case 'squirrel': // 松鼠：消除 5x5 内所有宝石
      for (let dr = -2; dr <= 2; dr++) {
        for (let dc = -2; dc <= 2; dc++) {
          const nr = row + dr
          const nc = col + dc
          if (nr >= 0 && nr < BOARD_SIZE && nc >= 0 && nc < BOARD_SIZE) {
            if (board[nr][nc].gem !== null) removed.add(nr + ',' + nc)
          }
        }
      }
      break

    case 'snail': // 蜗牛：只消自身
      removed.add(pos)
      break
  }

  return removed
}

module.exports = {
  BOARD_SIZE,
  GEM_COLORS,
  SpecialType,
  OBSTACLE_INFO,
  OBSTACLE_RENDER_INFO,
  ELEMENT_INFO,
  LEVEL_TYPE_INFO,
  CHAPTERS,

  getChapterByLevel,
  getChapterTotalStars,
  isChapterUnlocked,

  createEmptyCell,
  createGemCell,
  damageObstacle,
  isObstacleDestroyed,
  clearObstacle,
  getObstacleRenderInfo,
  copyBoard,
  createBoard,
  findMatches,
  analyzeMatchShape,
  triggerSpecial,
  dropGems,
  isValidSwap,
  doSwap,
  calculateScore,
  calculateStars,

  getLevelConfig,
  verifyLevelBeatable,
  createGameState,
  placeObstacles,
  updateSnails,
  checkGoalsComplete,
  processElementEffect
}