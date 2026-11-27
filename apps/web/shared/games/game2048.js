/**
 * game2048.js —— 2048 核心逻辑
 *
 * 原生小程序版。移植自 engine/apps/games/2048/ (C) 的等价 JS 实现。
 */

const CONFIG = {
  GRID_SIZE: 4,
  WIN_VALUE: 2048
}

/** 创建空棋盘 */
function createEmptyBoard () {
  const board = []
  for (let r = 0; r < CONFIG.GRID_SIZE; r++) {
    const row = []
    for (let c = 0; c < CONFIG.GRID_SIZE; c++) row.push(0)
    board.push(row)
  }
  return board
}

/** 深拷贝棋盘 */
function copyBoard (board) {
  return board.map(row => row.slice())
}

/** 收集所有空格位置 */
function getEmptyCells (board) {
  const cells = []
  for (let r = 0; r < CONFIG.GRID_SIZE; r++) {
    for (let c = 0; c < CONFIG.GRID_SIZE; c++) {
      if (board[r][c] === 0) cells.push({ row: r, col: c })
    }
  }
  return cells
}

/**
 * 在随机空格生成新数字（90% 为 2，10% 为 4）
 * @returns {boolean} 是否成功生成
 */
function spawnTile (state) {
  const emptyCells = getEmptyCells(state.board)
  if (emptyCells.length === 0) return false
  const pos = emptyCells[Math.floor(Math.random() * emptyCells.length)]
  state.board[pos.row][pos.col] = Math.random() < 0.9 ? 2 : 4
  return true
}

/** 创建初始状态 */
function createInitialState (bestScore) {
  const state = {
    board: createEmptyBoard(),
    score: 0,
    bestScore: bestScore || 0,
    isGameOver: false,
    hasWon: false,
    keepGoing: false
  }
  spawnTile(state)
  spawnTile(state)
  return state
}

/** 压缩一行（移除 0） */
function compressRow (row) {
  return row.filter(val => val !== 0)
}

/**
 * 合并一行
 * @returns {{row:number[], addedScore:number, maxMerged:number}}
 */
function mergeRow (row) {
  const result = []
  let addedScore = 0
  let maxMerged = 0
  let i = 0

  while (i < row.length) {
    if (i + 1 < row.length && row[i] === row[i + 1]) {
      const merged = row[i] * 2
      result.push(merged)
      addedScore += merged
      if (merged > maxMerged) maxMerged = merged
      i += 2
    } else {
      result.push(row[i])
      i++
    }
  }
  while (result.length < CONFIG.GRID_SIZE) result.push(0)
  return { row: result, addedScore, maxMerged }
}

/** 滑动并合并一行 */
function slideRow (row) {
  const compressed = compressRow(row)
  const merged = mergeRow(compressed)
  const moved = row.some((val, i) => val !== merged.row[i])
  return { row: merged.row, moved, addedScore: merged.addedScore, maxMerged: merged.maxMerged }
}

/** 检查棋盘是否达成 2048 */
function checkWin (state) {
  if (state.hasWon) return
  for (let r = 0; r < CONFIG.GRID_SIZE; r++) {
    for (let c = 0; c < CONFIG.GRID_SIZE; c++) {
      if (state.board[r][c] === CONFIG.WIN_VALUE) {
        state.hasWon = true
        return
      }
    }
  }
}

/**
 * 移动成功后的结算：计分 + 生成新块 + 胜利判定
 */
function settle (state, totalAddedScore, moved) {
  if (!moved) return
  state.score += totalAddedScore
  if (state.score > state.bestScore) state.bestScore = state.score
  spawnTile(state)
  checkWin(state)
}

/** 左右移动（reverse 控制方向） */
function moveHorizontal (state, reverse) {
  const originalBoard = copyBoard(state.board)
  const newBoard = []
  let totalAddedScore = 0
  let maxMergedTile = 0

  for (let r = 0; r < CONFIG.GRID_SIZE; r++) {
    let row = state.board[r].slice()
    if (reverse) row.reverse()
    const result = slideRow(row)
    if (reverse) result.row.reverse()
    newBoard.push(result.row)
    totalAddedScore += result.addedScore
    if (result.maxMerged > maxMergedTile) maxMergedTile = result.maxMerged
  }

  let moved = false
  for (let r = 0; r < CONFIG.GRID_SIZE && !moved; r++) {
    for (let c = 0; c < CONFIG.GRID_SIZE; c++) {
      if (originalBoard[r][c] !== newBoard[r][c]) { moved = true; break }
    }
  }

  if (moved) state.board = newBoard
  settle(state, totalAddedScore, moved)
  return { moved, mergedTile: maxMergedTile }
}

/**
 * 滑动并合并一列
 * @param {number[]} col 原始列
 * @param {boolean} toTop true=向上，false=向下
 */
function slideColumn (col, toTop) {
  const values = col.filter(val => val !== 0)
  const merged = []
  let addedScore = 0
  let maxMerged = 0
  let i = 0

  while (i < values.length) {
    if (i + 1 < values.length && values[i] === values[i + 1]) {
      const mergedVal = values[i] * 2
      merged.push(mergedVal)
      addedScore += mergedVal
      if (mergedVal > maxMerged) maxMerged = mergedVal
      i += 2
    } else {
      merged.push(values[i])
      i++
    }
  }

  const result = new Array(CONFIG.GRID_SIZE).fill(0)
  for (let j = 0; j < merged.length; j++) {
    if (toTop) {
      result[j] = merged[j]
    } else {
      result[CONFIG.GRID_SIZE - 1 - j] = merged[merged.length - 1 - j]
    }
  }
  return { col: result, addedScore, maxMerged }
}

/** 上下移动（toTop 控制方向） */
function moveVertical (state, toTop) {
  const originalBoard = copyBoard(state.board)
  const newBoard = createEmptyBoard()
  let totalAddedScore = 0
  let maxMergedTile = 0

  for (let c = 0; c < CONFIG.GRID_SIZE; c++) {
    const col = []
    for (let r = 0; r < CONFIG.GRID_SIZE; r++) col.push(state.board[r][c])
    const result = slideColumn(col, toTop)
    totalAddedScore += result.addedScore
    if (result.maxMerged > maxMergedTile) maxMergedTile = result.maxMerged
    for (let r = 0; r < CONFIG.GRID_SIZE; r++) newBoard[r][c] = result.col[r]
  }

  let moved = false
  for (let r = 0; r < CONFIG.GRID_SIZE && !moved; r++) {
    for (let c = 0; c < CONFIG.GRID_SIZE; c++) {
      if (originalBoard[r][c] !== newBoard[r][c]) { moved = true; break }
    }
  }

  if (moved) state.board = newBoard
  settle(state, totalAddedScore, moved)
  return { moved, mergedTile: maxMergedTile }
}

/** 检查是否还有可用移动 */
function canMove (state) {
  const size = CONFIG.GRID_SIZE
  for (let r = 0; r < size; r++) {
    for (let c = 0; c < size; c++) {
      if (state.board[r][c] === 0) return true
    }
  }
  for (let r = 0; r < size; r++) {
    for (let c = 0; c < size; c++) {
      const val = state.board[r][c]
      if (c + 1 < size && state.board[r][c + 1] === val) return true
      if (r + 1 < size && state.board[r + 1][c] === val) return true
    }
  }
  return false
}

/** 执行一次移动 */
function move (state, direction) {
  let result = { moved: false, mergedTile: 0 }
  switch (direction) {
    case 'left': result = moveHorizontal(state, false); break
    case 'right': result = moveHorizontal(state, true); break
    case 'up': result = moveVertical(state, true); break
    case 'down': result = moveVertical(state, false); break
  }
  if (!canMove(state)) state.isGameOver = true
  return result
}

/** 达成 2048 后继续游戏 */
function keepGoing (state) {
  state.keepGoing = true
}

/** 方块背景色 */
function getTileColor (value) {
  const colors = {
    0: '#3c3a32', 2: '#eee4da', 4: '#ede0c8', 8: '#f2b179',
    16: '#f59563', 32: '#f67c5f', 64: '#f65e3b', 128: '#edcf72',
    256: '#edcc61', 512: '#edc850', 1024: '#edc53f', 2048: '#edc22e'
  }
  if (value > 2048) return '#3c3a32'
  return colors[value] || colors[0]
}

/** 方块文字色 */
function getTextColor (value) {
  return value <= 4 ? '#776e65' : '#f9f6f2'
}

module.exports = {
  CONFIG,
  createInitialState,
  move,
  canMove,
  keepGoing,
  getTileColor,
  getTextColor
}