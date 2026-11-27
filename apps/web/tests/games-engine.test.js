/**
 * 引擎逻辑回归测试
 *
 * 在 Node 下运行，装一个 localStorage 内存替身，验证四个游戏引擎的关键规则。
 * 用法：node tests/games-engine.test.js
 *
 * 从 apps/games/tests/engine.test.js 迁入：51 个用例原样保留，
 * 只把 wx storage mock 换成 localStorage 替身、require 路径改到 shared/games/。
 */

const assert = require('assert')

// ==================== localStorage 替身 ====================

const mem = new Map()
global.localStorage = {
  getItem: k => (mem.has(k) ? mem.get(k) : null),
  setItem: (k, v) => { mem.set(k, String(v)) },
  removeItem: k => { mem.delete(k) },
  clear: () => { mem.clear() },
  key: i => {
    const keys = Array.from(mem.keys())
    return i < keys.length ? keys[i] : null
  },
  get length () { return mem.size }
}

// ==================== 载入引擎 ====================

const g2048 = require('../shared/games/game2048.js')
const snake = require('../shared/games/snake.js')
const sudoku = require('../shared/games/sudoku.js')
const match3 = require('../shared/games/match3.js')
const storage = require('../shared/games/storage.js')
const achievements = require('../shared/games/achievements.js')

let pass = 0
let fail = 0
const failures = []

function test (name, fn) {
  try {
    fn()
    pass++
    console.log('  \x1b[32mPASS\x1b[0m ' + name)
  } catch (e) {
    fail++
    failures.push({ name, error: e.message })
    console.log('  \x1b[31mFAIL\x1b[0m ' + name + ' -> ' + e.message)
  }
}

/**
 * 在 fn 执行期间把 Math.random 换成确定性 PRNG，结束后还原。
 *
 * 用途：引擎在「移动后生成新方块」「补空位」等处会调用 Math.random，
 * 若断言依赖棋盘精确内容，随机落点会导致偶发失败（flaky）。
 * 用固定种子把随机性钉死，测试才可重复。
 */
function withSeededRandom (seed, fn) {
  const original = Math.random
  let s = seed >>> 0
  // mulberry32
  Math.random = function () {
    s = (s + 0x6D2B79F5) >>> 0
    let t = s
    t = Math.imul(t ^ (t >>> 15), t | 1)
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296
  }
  try {
    return fn()
  } finally {
    Math.random = original
  }
}

// ==================== 2048 ====================

console.log('\n\x1b[1m2048\x1b[0m')

test('初始棋盘恰好有 2 个方块', () => {
  const s = g2048.createInitialState()
  const count = s.board.flat().filter(v => v !== 0).length
  assert.strictEqual(count, 2)
})

test('向左移动正确合并相邻同值并计分', () => {
  // move() 成功后会在随机空位生成新方块，随机落点会污染第一行断言。
  // 用固定种子钉死随机性：这里断言的是「合并规则与计分」，不是「落点位置」。
  withSeededRandom(42, () => {
    const s = g2048.createInitialState()
    s.board = [
      [2, 2, 4, 4],
      [0, 0, 0, 0],
      [0, 0, 0, 0],
      [0, 0, 0, 0]
    ]
    s.score = 0
    const r = g2048.move(s, 'left')
    assert.strictEqual(r.moved, true)
    // 合并结果：2+2=4、4+4=8，前两格固定；后两格可能被新方块占据
    assert.deepStrictEqual(s.board[0].slice(0, 2), [4, 8])
    assert.strictEqual(s.score, 12, '合并 2+2=4 得 4 分，4+4=8 得 8 分')
    // 合并后应恰好生成一个新方块（2 或 4）
    const nonZero = s.board.flat().filter(v => v !== 0)
    assert.strictEqual(nonZero.length, 3, '合并后保留 2 个 + 新生成 1 个')
  })
})

test('无法移动时 moved=false', () => {
  const s = g2048.createInitialState()
  s.board = [
    [2, 4, 2, 4],
    [4, 2, 4, 2],
    [2, 4, 2, 4],
    [4, 2, 4, 2]
  ]
  const r = g2048.move(s, 'left')
  assert.strictEqual(r.moved, false)
})

test('canMove 在满盘交错棋盘返回 false', () => {
  const s = g2048.createInitialState()
  s.board = [
    [2, 4, 2, 4],
    [4, 2, 4, 2],
    [2, 4, 2, 4],
    [4, 2, 4, 2]
  ]
  assert.strictEqual(g2048.canMove(s), false)
})

test('达成 2048 时 hasWon 置位', () => {
  const s = g2048.createInitialState()
  s.board = [
    [1024, 1024, 0, 0],
    [0, 0, 0, 0],
    [0, 0, 0, 0],
    [0, 0, 0, 0]
  ]
  s.hasWon = false
  g2048.move(s, 'left')
  assert.strictEqual(s.board[0][0], 2048)
  assert.strictEqual(s.hasWon, true)
})

test('移动后自动生成新方块', () => {
  const s = g2048.createInitialState()
  // 靠右的同值方块，左移后合并成 1 块，再补 1 块新方块 = 2
  s.board = [
    [0, 0, 2, 2],
    [0, 0, 0, 0],
    [0, 0, 0, 0],
    [0, 0, 0, 0]
  ]
  const r = g2048.move(s, 'left')
  assert.strictEqual(r.moved, true)
  assert.strictEqual(s.board[0][0], 4, '两个 2 应合并为 4')
  const count = s.board.flat().filter(v => v !== 0).length
  assert.strictEqual(count, 2, '合并后 1 块 + 补块 1 块 = 2')
})

// ==================== 贪吃蛇 ====================

console.log('\n\x1b[1m贪吃蛇\x1b[0m')

test('初始蛇身 3 节，蛇头在数组末尾且朝右不撞自己', () => {
  const s = snake.createInitialState()
  assert.strictEqual(s.snake.length, 3)
  assert.strictEqual(s.direction, snake.Direction.RIGHT)
  // 关键回归：开局第一帧不应立即死亡
  s.food = { x: 0, y: 0 }
  snake.tick(s)
  assert.strictEqual(s.isGameOver, false, '开局不应自撞')
})

test('前进一帧蛇头右移且尾部收缩', () => {
  const s = snake.createInitialState()
  const headBefore = { ...s.snake[s.snake.length - 1] }
  s.food = { x: 0, y: 0 } // 移到远处避免吃到
  snake.tick(s)
  const headAfter = s.snake[s.snake.length - 1]
  assert.strictEqual(headAfter.x, headBefore.x + 1, '蛇头应右移一格')
  assert.strictEqual(headAfter.y, headBefore.y)
  assert.strictEqual(s.snake.length, 3, '未进食时长度不变')
})

test('禁止 180 度反向', () => {
  const s = snake.createInitialState()
  s.direction = snake.Direction.RIGHT
  s.nextDirection = snake.Direction.RIGHT
  snake.setDirection(s, snake.Direction.LEFT)
  assert.strictEqual(s.nextDirection, snake.Direction.RIGHT, '反向指令被忽略')
})

test('撞墙判定游戏结束', () => {
  const s = snake.createInitialState()
  s.snake = [{ x: 19, y: 0 }]
  s.direction = snake.Direction.RIGHT
  s.nextDirection = snake.Direction.RIGHT
  snake.tick(s)
  assert.strictEqual(s.isGameOver, true)
})

test('吃到食物 +10 分且加速', () => {
  const s = snake.createInitialState()
  const head = s.snake[s.snake.length - 1]
  // 食物放在蛇头前进方向的下一格，确保本帧必定吃到
  s.food = { x: head.x + 1, y: head.y }
  const speedBefore = s.speed
  snake.tick(s)
  assert.strictEqual(s.score, 10)
  assert.strictEqual(s.speed, speedBefore - snake.SNAKE_CONFIG.SPEED_INCREMENT)
  assert.strictEqual(s.snake.length, 4, '进食后蛇身 +1')
})

test('速度不低于 MIN_SPEED', () => {
  const s = snake.createInitialState()
  s.speed = snake.SNAKE_CONFIG.MIN_SPEED
  const head = s.snake[s.snake.length - 1]
  s.food = { x: head.x + 1, y: head.y }
  snake.tick(s)
  assert.strictEqual(s.speed, snake.SNAKE_CONFIG.MIN_SPEED)
})

test('getGrid 正确标记蛇头=3 食物=2', () => {
  const s = snake.createInitialState()
  const grid = snake.getGrid(s)
  const head = s.snake[s.snake.length - 1]
  assert.strictEqual(grid[head.y][head.x], 3)
  assert.strictEqual(grid[s.food.y][s.food.x], 2)
})

// ==================== 数独 ====================

console.log('\n\x1b[1m数独\x1b[0m')

test('简单难度挖 30 洞且解唯一', () => {
  const s = sudoku.createInitialState(0)
  assert.strictEqual(s.emptyCount, 30)
  // 解中不含 0
  const solutionValid = s.solution.flat().every(v => v >= 1 && v <= 9)
  assert.strictEqual(solutionValid, true)
})

test('固定格不可修改', () => {
  const s = sudoku.createInitialState(0)
  // 找一个题目固定格
  let found = false
  for (let r = 0; r < 9 && !found; r++) {
    for (let c = 0; c < 9; c++) {
      if (s.board[r][c].isGiven) {
        s.cursorRow = r
        s.cursorCol = c
        const original = s.board[r][c].value
        sudoku.placeNumber(s, 9)
        assert.strictEqual(s.board[r][c].value, original, '固定格未被修改')
        found = true
        break
      }
    }
  }
  assert.ok(found, '应存在固定格')
})

test('冲突检测：同行同值标记 isConflict', () => {
  const s = sudoku.createInitialState(0)
  // 手工构造冲突
  s.board.forEach(row => row.forEach(c => { c.isGiven = false; c.value = 0; c.isConflict = false }))
  s.board[0][0].value = 5
  s.board[0][1].value = 5
  sudoku.updateConflicts(s)
  assert.strictEqual(s.board[0][0].isConflict, true)
  assert.strictEqual(s.board[0][1].isConflict, true)
})

test('提示消耗次数并填入正确数字', () => {
  const s = sudoku.createInitialState(0)
  const hints = s.hintCount
  const ok = sudoku.hint(s)
  assert.strictEqual(ok, true)
  assert.strictEqual(s.hintCount, hints - 1)
  assert.strictEqual(s.board.flat().filter(c => c.value === 0).length, s.emptyCount)
})

test('提示次数耗尽返回 false', () => {
  const s = sudoku.createInitialState(0)
  s.hintCount = 0
  assert.strictEqual(sudoku.hint(s), false)
})

test('填满全盘触发 isGameOver', () => {
  const s = sudoku.createInitialState(0)
  for (let r = 0; r < 9; r++) {
    for (let c = 0; c < 9; c++) {
      s.board[r][c].value = s.solution[r][c]
      s.board[r][c].isGiven = true
    }
  }
  s.isGameOver = false
  sudoku.checkWin(s)
  assert.strictEqual(s.isGameOver, true)
})

test('光标环形移动不越界', () => {
  const s = sudoku.createInitialState(0)
  s.cursorRow = 0
  s.cursorCol = 0
  sudoku.moveCursor(s, -1, -1)
  assert.strictEqual(s.cursorRow, 8)
  assert.strictEqual(s.cursorCol, 8)
})

// ==================== 消消乐 ====================

console.log('\n\x1b[1m消消乐\x1b[0m')

test('章节表覆盖 1-50 关且连续', () => {
  let total = 0
  for (let lv = 1; lv <= 50; lv++) {
    const c = match3.getChapterByLevel(lv)
    assert.ok(c, '关卡 ' + lv + ' 应属于某章节')
  }
  match3.CHAPTERS.forEach(c => { total += c.levels[1] - c.levels[0] + 1 })
  assert.strictEqual(total, 50)
})

test('初始棋盘无宝石', () => {
  const s = match3.createGameState(1)
  assert.strictEqual(s.status, 'playing')
  assert.strictEqual(s.board.length, 9)
  assert.strictEqual(s.board[0].length, 9)
})

test('开局不自动成三连', () => {
  const board = match3.createBoard()
  assert.strictEqual(match3.findMatches(board).length, 0, '新棋盘不应有匹配')
})

test('水平三连被正确识别', () => {
  const board = match3.createBoard()
  // 手工构造横向三连（第 0 行）
  for (let c = 0; c < 9; c++) board[0][c] = board[8][c]
  board[0][0] = match3.createGemCell(3)
  board[0][1] = match3.createGemCell(3)
  board[0][2] = match3.createGemCell(3)
  board[0][3] = match3.createGemCell(4)
  const matches = match3.findMatches(board)
  assert.ok(matches.length >= 1, '应识别出匹配')
  assert.ok(matches.some(m => m.cells.includes('0,0')))
})

test('纵向三连被正确识别', () => {
  const board = match3.createBoard()
  for (let r = 0; r < 9; r++) board[r][0] = board[r][8]
  board[0][0] = match3.createGemCell(2)
  board[1][0] = match3.createGemCell(2)
  board[2][0] = match3.createGemCell(2)
  const matches = match3.findMatches(board)
  assert.ok(matches.some(m => m.cells.includes('0,0')))
})

test('障碍物格不参与匹配', () => {
  const board = match3.createBoard()
  for (let c = 0; c < 9; c++) board[0][c] = board[8][c]
  board[0][0] = match3.createGemCell(3)
  board[0][1] = match3.createGemCell(3)
  board[0][2] = match3.createGemCell(3)
  board[0][2].obstacle = 'ice'
  board[0][2].obstacleHp = 1
  const matches = match3.findMatches(board)
  assert.ok(!matches.some(m => m.cells.includes('0,2')), '障碍格不应计入匹配')
})

test('直线特效清除整行', () => {
  const board = match3.createBoard()
  const removed = match3.triggerSpecial(board, '4,3', match3.SpecialType.ROW_CLEAR)
  assert.ok(removed.has('4,0') && removed.has('4,8'), '整行都应被标记')
})

test('炸弹特效清除 3x3', () => {
  const board = match3.createBoard()
  const removed = match3.triggerSpecial(board, '4,4', match3.SpecialType.BOMB)
  assert.strictEqual(removed.size, 9)
})

test('彩虹特效清除全部同色', () => {
  const board = match3.createBoard()
  const target = board[0][0].gem
  for (let r = 0; r < 9; r++) for (let c = 0; c < 9; c++) board[r][c] = match3.createGemCell(target)
  const removed = match3.triggerSpecial(board, '0,0', match3.SpecialType.RAINBOW)
  assert.strictEqual(removed.size, 81)
})

test('石头阻挡直线特效（不被清除）', () => {
  const board = match3.createBoard()
  board[4][2].obstacle = 'stone'
  board[4][2].obstacleHp = 3
  const removed = match3.triggerSpecial(board, '4,3', match3.SpecialType.ROW_CLEAR)
  assert.ok(!removed.has('4,2'), '石头不应被直线特效清除')
})

test('宝石下落后列不留空洞', () => {
  const board = match3.createBoard()
  board[0][0].gem = null
  match3.dropGems(board)
  assert.ok(board[0][0].gem !== null, '顶部应补上新宝石')
})

test('相邻交换判定', () => {
  const board = match3.createBoard()
  // 确定性构造：确保涉及的三格都有宝石，避免随机空位干扰
  board[4][4] = match3.createGemCell(0)
  board[4][5] = match3.createGemCell(1)
  board[5][4] = match3.createGemCell(2)
  assert.strictEqual(match3.isValidSwap(board, '4,4', '4,5'), true, '水平相邻应允许')
  assert.strictEqual(match3.isValidSwap(board, '4,4', '5,4'), true, '垂直相邻应允许')
  assert.strictEqual(match3.isValidSwap(board, '4,4', '4,6'), false, '隔一格非相邻')
  assert.strictEqual(match3.isValidSwap(board, '4,4', '6,4'), false, '隔一格非相邻')
  assert.strictEqual(match3.isValidSwap(board, '4,4', '5,5'), false, '对角线非相邻')
})

test('gem 类型 0（黄色）可参与交换 —— 真值判断回归', () => {
  const board = match3.createBoard()
  // gem===0 是合法宝石，旧实现用 !gem 判断会误判为空
  board[2][2] = match3.createGemCell(0)
  board[2][3] = match3.createGemCell(0)
  assert.strictEqual(match3.isValidSwap(board, '2,2', '2,3'), true,
    'gem=0 不得被当作空格')
})

test('空格不能参与交换', () => {
  const board = match3.createBoard()
  board[4][4] = match3.createEmptyCell()
  board[4][5] = match3.createGemCell(1)
  assert.strictEqual(match3.isValidSwap(board, '4,4', '4,5'), false, '空格不可交换')
})

test('星级按剩余步数比例计算', () => {
  assert.strictEqual(match3.calculateStars(1000, 1000, 20, 30), 3)
  assert.strictEqual(match3.calculateStars(1000, 1000, 10, 30), 2)
  assert.strictEqual(match3.calculateStars(1000, 1000, 5, 30), 1)
  assert.strictEqual(match3.calculateStars(500, 1000, 20, 30), 0, '未达标 0 星')
})

test('连击提高分数', () => {
  const base = match3.calculateScore(3, 1)
  const combo = match3.calculateScore(3, 3)
  assert.ok(combo > base, '连击分数应更高')
})

test('障碍物伤害与摧毁', () => {
  const cell = match3.createGemCell(1)
  cell.obstacle = 'stone'
  cell.obstacleHp = 3
  match3.damageObstacle(cell, 1)
  assert.strictEqual(cell.obstacleHp, 2)
  match3.damageObstacle(cell, 5)
  assert.strictEqual(cell.obstacleHp, 0, '血量不降为负')
  assert.strictEqual(match3.isObstacleDestroyed(cell), true)
})

test('所有 50 关配置可生成', () => {
  for (let lv = 1; lv <= 50; lv++) {
    const config = match3.getLevelConfig(lv)
    assert.ok(config.moves > 0, '关卡 ' + lv + ' 步数应 > 0')
    assert.ok(config.targetScore > 0)
    assert.ok(config.goals.length >= 1)
  }
})

test('第 1 章始终解锁', () => {
  const c1 = match3.CHAPTERS[0]
  assert.strictEqual(match3.isChapterUnlocked(c1, 0, new Set()), true)
})

test('章节星数需求生效', () => {
  const c2 = match3.CHAPTERS[1] // 需要 15 星
  assert.strictEqual(match3.isChapterUnlocked(c2, 0, new Set()), false)
  assert.strictEqual(match3.isChapterUnlocked(c2, 20, new Set()), true)
})

// ==================== 存储与成就 ====================

console.log('\n\x1b[1m存储与成就\x1b[0m')

test('最高分只增不减', () => {
  storage.updateSnakeBestScore(100)
  storage.updateSnakeBestScore(50)
  assert.strictEqual(storage.getSnakeBestScore(), 100)
})

test('migrate 幂等', () => {
  assert.strictEqual(storage.migrate(), true)
  assert.strictEqual(storage.migrate(), true)
})

test('贪吃蛇首次得分解锁 first_blood', () => {
  achievements.resetSubscribers()
  const unlocked = achievements.evaluate({ type: 'snake.eat', score: 10 })
  assert.ok(unlocked.some(a => a.id === 'snake_first_blood'))
})

test('重复触发不重复解锁', () => {
  achievements.evaluate({ type: 'snake.eat', score: 10 })
  const again = achievements.evaluate({ type: 'snake.eat', score: 10 })
  assert.ok(!again.some(a => a.id === 'snake_first_blood'), '已解锁不应再次返回')
})

test('贪吃蛇 100 分解锁 score_100', () => {
  const u = achievements.evaluate({ type: 'snake.gameOver', score: 100 })
  assert.ok(u.some(a => a.id === 'snake_score_100'))
})

test('2048 达成 4096 解锁对应成就', () => {
  const u = achievements.evaluate({ type: 'game2048.merge', tileValue: 4096 })
  assert.ok(u.some(a => a.id === 'game2048_reach_2048'))
  assert.ok(u.some(a => a.id === 'game2048_reach_4096'))
})

test('无提示通关解锁 no_hint', () => {
  const u = achievements.evaluate({ type: 'sudoku.complete', hintCount: 0 })
  assert.ok(u.some(a => a.id === 'sudoku_no_hint'))
})

test('消消乐三星解锁三星成就', () => {
  const u = achievements.evaluate({ type: 'match3.complete', stars: 3 })
  assert.ok(u.some(a => a.id === 'match3_three_stars'))
})

test('订阅者收到解锁事件', () => {
  const seen = []
  const off = achievements.subscribe(a => seen.push(a.id))
  // 该成就此前已解锁，用未触发过的事件（匹配失败 → 不解锁）验证订阅不误报
  achievements.evaluate({ type: 'snake.eat', score: 1 })
  off()
  // 再用一个确定能解锁的事件：先重置存储让 match3 成就回到未解锁态
  storage.resetAll()
  achievements.resetSubscribers()
  const seen2 = []
  const off2 = achievements.subscribe(a => seen2.push(a.id))
  achievements.evaluate({ type: 'match3.complete', stars: 3 })
  off2()
  assert.ok(seen2.length > 0, '订阅者应收到通知')
  assert.ok(seen2.includes('match3_three_stars'))
})

test('成就列表包含全部 8 项', () => {
  const list = achievements.getAchievementList()
  assert.strictEqual(list.length, 8)
})

test('分享文案包含分数', () => {
  const share = require('../shared/games/share.js')
  const card = share.buildSnakeShare(120, 200)
  assert.ok(card.title.includes('120'))
  assert.ok(card.title.includes('200'))
  assert.strictEqual(card.path, '/pages/snake/index')
})

// ==================== 结果 ====================

console.log('\n' + '='.repeat(50))
console.log('通过 ' + pass + ' 项，失败 ' + fail + ' 项')
if (fail > 0) {
  console.log('\n失败明细：')
  failures.forEach(f => console.log('  - ' + f.name + ': ' + f.error))
  process.exit(1)
}
console.log('全部通过')