/**
 * share.js —— 各游戏分享卡片文案
 */

/** 贪吃蛇 */
function buildSnakeShare (score, best) {
  return {
    title: '我在贪吃蛇里吃到了 ' + score + ' 分，最高 ' + best + ' 分，来挑战我！',
    path: '/pages/snake/index'
  }
}

/** 2048 */
function build2048Share (score, best) {
  return {
    title: '2048 合成到了 ' + score + ' 分，最高 ' + best + ' 分，来挑战我！',
    path: '/pages/game2048/index'
  }
}

/** 数独 */
function buildSudokuShare (chapter, level, stars) {
  return {
    title: '数独第 ' + chapter + ' 章第 ' + level + ' 关拿到 ' + stars + ' 星，你也来试试！',
    path: '/pages/sudoku/index'
  }
}

/** 消消乐 */
function buildMatch3Share (chapter, level, score) {
  return {
    title: '消消乐第 ' + chapter + '-' + level + ' 关打到 ' + score + ' 分，敢来挑战吗？',
    path: '/pages/match3/index'
  }
}

module.exports = {
  buildSnakeShare,
  build2048Share,
  buildSudokuShare,
  buildMatch3Share
}