/**
 * snake.js —— 贪吃蛇核心逻辑
 *
 * 原生小程序版。移植自 engine/apps/games/snake/ (C) 的等价 JS 实现。
 */

const SNAKE_CONFIG = {
  GRID_WIDTH: 20,
  GRID_HEIGHT: 15,
  INITIAL_SPEED: 150,   // 毫秒
  SPEED_INCREMENT: 2,   // 每吃一个食物减少的毫秒数
  MIN_SPEED: 50         // 最快速度
}

/** 方向枚举 */
const Direction = {
  UP: 0,
  RIGHT: 1,
  DOWN: 2,
  LEFT: 3
}

const DIR_VECTORS = {
  [Direction.UP]: { x: 0, y: -1 },
  [Direction.DOWN]: { x: 0, y: 1 },
  [Direction.LEFT]: { x: -1, y: 0 },
  [Direction.RIGHT]: { x: 1, y: 0 }
}

const OPPOSITE_DIR = {
  [Direction.UP]: Direction.DOWN,
  [Direction.DOWN]: Direction.UP,
  [Direction.LEFT]: Direction.RIGHT,
  [Direction.RIGHT]: Direction.LEFT
}

/** 判断位置是否在蛇身上 */
function isSnakePosition (snake, pos) {
  return snake.some(segment => segment.x === pos.x && segment.y === pos.y)
}

/**
 * 生成新食物位置
 * @param {number} maxAttempts 最大尝试次数，防止死循环
 */
function generateFood (state, maxAttempts) {
  const attempts = maxAttempts || 100
  const { GRID_WIDTH, GRID_HEIGHT } = SNAKE_CONFIG
  let newFood = { x: 0, y: 0 }
  let count = 0

  do {
    newFood = {
      x: Math.floor(Math.random() * GRID_WIDTH),
      y: Math.floor(Math.random() * GRID_HEIGHT)
    }
    count++
  } while (isSnakePosition(state.snake, newFood) && count < attempts)

  return newFood
}

/** 创建初始游戏状态 */
function createInitialState () {
  const centerX = Math.floor(SNAKE_CONFIG.GRID_WIDTH / 2)
  const centerY = Math.floor(SNAKE_CONFIG.GRID_HEIGHT / 2)

  // 蛇身数组按「尾 → 头」排列，snake[snake.length - 1] 是蛇头。
  // 初始朝右，故蛇头在右侧，身体向左依次递减，避免第一帧就撞到自己。
  const state = {
    snake: [
      { x: centerX - 2, y: centerY },
      { x: centerX - 1, y: centerY },
      { x: centerX, y: centerY }
    ],
    food: { x: 0, y: 0 },
    direction: Direction.RIGHT,
    nextDirection: Direction.RIGHT,
    score: 0,
    isGameOver: false,
    speed: SNAKE_CONFIG.INITIAL_SPEED
  }

  state.food = generateFood(state)
  return state
}

/** 设置方向（阻止 180° 反向） */
function setDirection (state, newDir) {
  if (newDir !== OPPOSITE_DIR[state.direction]) {
    state.nextDirection = newDir
  }
}

/** 执行一帧游戏逻辑 */
function tick (state) {
  if (state.isGameOver) return

  const { GRID_WIDTH, GRID_HEIGHT } = SNAKE_CONFIG
  state.direction = state.nextDirection

  const head = state.snake[state.snake.length - 1]
  const dir = DIR_VECTORS[state.direction]
  const newHead = { x: head.x + dir.x, y: head.y + dir.y }

  // 撞墙
  if (newHead.x < 0 || newHead.x >= GRID_WIDTH ||
      newHead.y < 0 || newHead.y >= GRID_HEIGHT) {
    state.isGameOver = true
    return
  }

  // 撞自己（尾部会移动，故排除最后一节）
  if (isSnakePosition(state.snake.slice(0, -1), newHead)) {
    state.isGameOver = true
    return
  }

  state.snake.push(newHead)

  if (newHead.x === state.food.x && newHead.y === state.food.y) {
    state.score += 10
    state.food = generateFood(state)
    state.speed = Math.max(SNAKE_CONFIG.MIN_SPEED, state.speed - SNAKE_CONFIG.SPEED_INCREMENT)
  } else {
    state.snake.shift()
  }
}

/**
 * 获取网格状态（用于渲染）
 * @returns {number[][]} 0=空格 1=蛇身 2=食物 3=蛇头
 */
function getGrid (state) {
  const { GRID_WIDTH, GRID_HEIGHT } = SNAKE_CONFIG
  const grid = []
  for (let r = 0; r < GRID_HEIGHT; r++) {
    const row = []
    for (let c = 0; c < GRID_WIDTH; c++) row.push(0)
    grid.push(row)
  }

  grid[state.food.y][state.food.x] = 2

  const last = state.snake.length - 1
  state.snake.forEach((segment, index) => {
    grid[segment.y][segment.x] = index === last ? 3 : 1
  })

  return grid
}

module.exports = {
  SNAKE_CONFIG,
  Direction,
  createInitialState,
  generateFood,
  setDirection,
  tick,
  getGrid
}