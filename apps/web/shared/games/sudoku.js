/**
 * sudoku.js —— 数独核心逻辑
 *
 * 原生小程序版。移植自 engine/apps/games/sudoku/ (C++) 的等价 JS 实现。
 */

const SUDOKU_CONFIG = {
  GRID_SIZE: 9,
  BOX_SIZE: 3,
  DIFFICULTIES: [
    { name: '简单', holes: 30, maxTime: 180, description: '入门' },
    { name: '中等', holes: 40, maxTime: 300, description: '进阶' },
    { name: '困难', holes: 50, maxTime: 600, description: '挑战' }
  ]
}

/** 创建空棋盘 */
function createEmptyBoard () {
  const board = []
  for (let r = 0; r < SUDOKU_CONFIG.GRID_SIZE; r++) {
    const row = []
    for (let c = 0; c < SUDOKU_CONFIG.GRID_SIZE; c++) {
      row.push({ value: 0, isGiven: false, isConflict: false })
    }
    board.push(row)
  }
  return board
}

/** 深拷贝棋盘 */
function copyBoard (board) {
  return board.map(row => row.map(cell => ({
    value: cell.value,
    isGiven: cell.isGiven,
    isConflict: cell.isConflict
  })))
}

/** 深拷贝解 */
function copySolution (solution) {
  return solution.map(row => row.slice())
}

/** 检查在某位置放置数字是否合法（行 / 列 / 宫） */
function isValidPlacement (board, row, col, num) {
  const { GRID_SIZE, BOX_SIZE } = SUDOKU_CONFIG

  for (let c = 0; c < GRID_SIZE; c++) {
    if (c !== col && board[row][c].value === num) return false
  }
  for (let r = 0; r < GRID_SIZE; r++) {
    if (r !== row && board[r][col].value === num) return false
  }

  const boxRow = Math.floor(row / BOX_SIZE) * BOX_SIZE
  const boxCol = Math.floor(col / BOX_SIZE) * BOX_SIZE
  for (let r = boxRow; r < boxRow + BOX_SIZE; r++) {
    for (let c = boxCol; c < boxCol + BOX_SIZE; c++) {
      if (r !== row && c !== col && board[r][c].value === num) return false
    }
  }
  return true
}

/** 回溯求解 */
function solveSudoku (board) {
  const { GRID_SIZE } = SUDOKU_CONFIG

  for (let row = 0; row < GRID_SIZE; row++) {
    for (let col = 0; col < GRID_SIZE; col++) {
      if (board[row][col].value === 0) {
        for (let num = 1; num <= GRID_SIZE; num++) {
          if (isValidPlacement(board, row, col, num)) {
            board[row][col].value = num
            if (solveSudoku(board)) return true
            board[row][col].value = 0
          }
        }
        return false
      }
    }
  }
  return true
}

/**
 * 统计解的个数（最多到 limit，用于唯一解校验）
 * @returns {number}
 */
function countSolutions (board, solution, limit) {
  const { GRID_SIZE } = SUDOKU_CONFIG
  const max = limit === undefined ? 2 : limit
  let count = 0
  const tempBoard = copyBoard(board)

  for (let row = 0; row < GRID_SIZE; row++) {
    for (let col = 0; col < GRID_SIZE; col++) {
      if (tempBoard[row][col].value === 0) {
        for (let num = 1; num <= GRID_SIZE; num++) {
          if (isValidPlacement(tempBoard, row, col, num)) {
            tempBoard[row][col].value = num
            count += countSolutions(tempBoard, solution, max - count)
            tempBoard[row][col].value = 0
            if (count >= max) return count
          }
        }
        return count
      }
    }
  }

  // 找到一个完整解，保存下来
  if (count === 0 && solution) {
    for (let r = 0; r < GRID_SIZE; r++) {
      for (let c = 0; c < GRID_SIZE; c++) {
        solution[r][c] = tempBoard[r][c].value
      }
    }
  }
  return count === 0 ? 1 : count
}

/** 生成完整数独解 */
function generateFullBoard () {
  const board = createEmptyBoard()
  solveSudoku(board)
  return board.map(row => row.map(cell => cell.value))
}

/**
 * 挖洞生成题目，保证唯一解
 * @returns {number} 实际挖掉的格子数
 */
function removeNumbers (solution, board, holes) {
  const { GRID_SIZE } = SUDOKU_CONFIG
  const positions = []

  for (let r = 0; r < GRID_SIZE; r++) {
    for (let c = 0; c < GRID_SIZE; c++) positions.push({ row: r, col: c })
  }

  // Fisher-Yates 洗牌
  for (let i = positions.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1))
    const tmp = positions[i]
    positions[i] = positions[j]
    positions[j] = tmp
  }

  let removed = 0
  for (let i = 0; i < positions.length; i++) {
    if (removed >= holes) break

    const { row, col } = positions[i]
    const tempValue = solution[row][col]
    board[row][col].value = 0

    const emptySolution = []
    for (let r = 0; r < GRID_SIZE; r++) {
      const line = []
      for (let c = 0; c < GRID_SIZE; c++) line.push(0)
      emptySolution.push(line)
    }

    if (countSolutions(board, emptySolution, 2) === 1) {
      removed++
    } else {
      board[row][col].value = tempValue
    }
  }
  return removed
}

/** 创建初始游戏状态 */
function createInitialState (difficulty) {
  const level = difficulty || 0
  const holes = SUDOKU_CONFIG.DIFFICULTIES[level].holes
  const solution = generateFullBoard()

  const board = createEmptyBoard()
  for (let r = 0; r < SUDOKU_CONFIG.GRID_SIZE; r++) {
    for (let c = 0; c < SUDOKU_CONFIG.GRID_SIZE; c++) {
      board[r][c].value = solution[r][c]
      board[r][c].isGiven = true
    }
  }

  const removed = removeNumbers(solution, board, holes)

  return {
    board,
    solution: copySolution(solution),
    cursorRow: 0,
    cursorCol: 0,
    difficulty: level,
    emptyCount: removed,
    isGameOver: false,
    selectedNumber: 1,
    hintCount: 3
  }
}

/** 重新计算空格数 */
function recalcEmptyCount (state) {
  const { GRID_SIZE } = SUDOKU_CONFIG
  let count = 0
  for (let r = 0; r < GRID_SIZE; r++) {
    for (let c = 0; c < GRID_SIZE; c++) {
      if (state.board[r][c].value === 0) count++
    }
  }
  state.emptyCount = count
}

/** 标记所有冲突格（同行/同列同值） */
function updateConflicts (state) {
  const { GRID_SIZE } = SUDOKU_CONFIG

  for (let r = 0; r < GRID_SIZE; r++) {
    for (let c = 0; c < GRID_SIZE; c++) state.board[r][c].isConflict = false
  }

  for (let r = 0; r < GRID_SIZE; r++) {
    for (let c = 0; c < GRID_SIZE; c++) {
      const value = state.board[r][c].value
      if (value === 0) continue

      for (let cc = 0; cc < GRID_SIZE; cc++) {
        if (cc !== c && state.board[r][cc].value === value) {
          state.board[r][c].isConflict = true
          state.board[r][cc].isConflict = true
        }
      }
      for (let rr = 0; rr < GRID_SIZE; rr++) {
        if (rr !== r && state.board[rr][c].value === value) {
          state.board[r][c].isConflict = true
          state.board[rr][c].isConflict = true
        }
      }
    }
  }
}

/** 检查是否完成 */
function checkWin (state) {
  const { GRID_SIZE } = SUDOKU_CONFIG
  for (let r = 0; r < GRID_SIZE; r++) {
    for (let c = 0; c < GRID_SIZE; c++) {
      if (state.board[r][c].value === 0) return
    }
  }
  state.isGameOver = true
}

/** 在光标处放置数字 */
function placeNumber (state, num) {
  const cell = state.board[state.cursorRow][state.cursorCol]
  if (cell.isGiven) return

  cell.value = num
  updateConflicts(state)
  checkWin(state)
}

/** 擦除光标处格子 */
function eraseCell (state) {
  const cell = state.board[state.cursorRow][state.cursorCol]
  if (cell.isGiven) return
  cell.value = 0
  cell.isConflict = false
  recalcEmptyCount(state)
}

/** 移动光标（环形边界） */
function moveCursor (state, dx, dy) {
  const { GRID_SIZE } = SUDOKU_CONFIG
  state.cursorCol = (state.cursorCol + dx + GRID_SIZE) % GRID_SIZE
  state.cursorRow = (state.cursorRow + dy + GRID_SIZE) % GRID_SIZE
}

/** 直接设置光标位置 */
function setCursor (state, row, col) {
  state.cursorRow = Math.max(0, Math.min(8, row))
  state.cursorCol = Math.max(0, Math.min(8, col))
}

/** 选中数字（1-9） */
function selectNumber (state, num) {
  state.selectedNumber = Math.max(1, Math.min(9, num))
}

/** 提示：填入一个正确数字 */
function hint (state) {
  const { GRID_SIZE } = SUDOKU_CONFIG
  if (state.hintCount <= 0) return false

  for (let r = 0; r < GRID_SIZE; r++) {
    for (let c = 0; c < GRID_SIZE; c++) {
      if (state.board[r][c].value === 0) {
        state.board[r][c].value = state.solution[r][c]
        state.board[r][c].isGiven = true
        state.emptyCount--
        state.hintCount--
        updateConflicts(state)
        checkWin(state)
        return true
      }
    }
  }
  return false
}

module.exports = {
  SUDOKU_CONFIG,
  createInitialState,
  placeNumber,
  eraseCell,
  updateConflicts,
  checkWin,
  moveCursor,
  setCursor,
  selectNumber,
  hint
}