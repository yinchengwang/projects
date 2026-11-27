/**
 * achievements.js —— 成就规则引擎
 *
 * 游戏事件触发一组规则，命中即解锁成就并广播给订阅者。
 */

const storage = require('./storage.js')

/** 成就元数据表 */
const ACHIEVEMENT_META = {
  snake_first_blood: { name: '初尝血腥', desc: '贪吃蛇首次得分达到 10', emoji: '🩸' },
  snake_score_100: { name: '贪吃大师', desc: '贪吃蛇单局得分达到 100', emoji: '🐍' },
  sudoku_first_clear: { name: '初次通关', desc: '完成任意一局数独', emoji: '🎯' },
  sudoku_no_hint: { name: '自有洞察', desc: '不使用提示完成数独', emoji: '🧠' },
  game2048_reach_2048: { name: '2048 达成', desc: '合成出 2048 方块', emoji: '🔢' },
  game2048_reach_4096: { name: '数值狂人', desc: '合成出 4096 方块', emoji: '🚀' },
  match3_first_clear: { name: '初次通关', desc: '完成任意一关消消乐', emoji: '💎' },
  match3_three_stars: { name: '完美三星', desc: '消消乐获得三星评价', emoji: '⭐' }
}

/** 全部成就 id 列表 */
const ALL_IDS = Object.keys(ACHIEVEMENT_META)

const subscribers = new Set()

/**
 * 订阅解锁事件
 * @returns {function} 取消订阅函数
 */
function subscribe (fn) {
  subscribers.add(fn)
  return () => subscribers.delete(fn)
}

/** 广播（单个订阅者异常不影响其他） */
function emit (achievement) {
  subscribers.forEach(fn => {
    try {
      fn(achievement)
    } catch (err) {
      console.error('[achievements] 订阅者异常:', err)
    }
  })
}

/** 成就规则表 */
const RULES = [
  { id: 'snake_first_blood', test: e => e.type === 'snake.eat' && e.score >= 10 },
  { id: 'snake_score_100', test: e => e.type === 'snake.gameOver' && e.score >= 100 },
  { id: 'sudoku_first_clear', test: e => e.type === 'sudoku.complete' },
  { id: 'sudoku_no_hint', test: e => e.type === 'sudoku.complete' && e.hintCount === 0 },
  { id: 'game2048_reach_2048', test: e => e.type === 'game2048.merge' && e.tileValue >= 2048 },
  { id: 'game2048_reach_4096', test: e => e.type === 'game2048.merge' && e.tileValue >= 4096 },
  { id: 'match3_first_clear', test: e => e.type === 'match3.complete' },
  { id: 'match3_three_stars', test: e => e.type === 'match3.complete' && e.stars === 3 }
]

/**
 * 触发一次事件评估
 * @param {object} event 游戏事件
 * @returns {object[]} 本次新解锁的成就
 */
function evaluate (event) {
  const unlocked = []

  for (let i = 0; i < RULES.length; i++) {
    const rule = RULES[i]
    if (!rule.test(event)) continue
    const achievement = storage.unlockAchievement(rule.id)
    if (achievement) {
      const meta = ACHIEVEMENT_META[rule.id] || { name: rule.id, desc: '', emoji: '🏆' }
      const full = Object.assign({}, achievement, meta)
      unlocked.push(full)
      emit(full)
    }
  }

  // 贪吃蛇每吃一个食物累计 +10 分进度
  if (event.type === 'snake.eat') {
    storage.recordAchievementProgress('snake_score_100', 10)
  }

  return unlocked
}

/** 成就列表（含解锁状态），供成就墙展示 */
function getAchievementList () {
  const data = storage.getAchievements()
  const progress = data.progress || {}
  const unlockedMap = {}
  data.unlocked.forEach(a => { unlockedMap[a.id] = a.unlockedAt })

  return ALL_IDS.map(id => {
    const meta = ACHIEVEMENT_META[id]
    return {
      id,
      name: meta.name,
      desc: meta.desc,
      emoji: meta.emoji,
      unlocked: !!unlockedMap[id],
      unlockedAt: unlockedMap[id] || 0,
      progress: progress[id] || 0
    }
  })
}

/** 仅供测试：清空订阅者 */
function resetSubscribers () {
  subscribers.clear()
}

module.exports = {
  ACHIEVEMENT_META,
  ALL_IDS,
  subscribe,
  evaluate,
  getAchievementList,
  resetSubscribers
}