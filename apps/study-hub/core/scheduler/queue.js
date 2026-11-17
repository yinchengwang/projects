"use strict"
/**
 * queue.js —— 队列成形：把各轨的候选交错成一条队列
 *
 * 规则（契约 §6.2）：
 *   1. 轨内顺序由调用方给，本函数不重排
 *   2. 跨轨 round-robin 交错，按轨的配额取数
 *   3. seq 从 1 开始连续
 *
 * **为什么没有「连续同轨上限」**：round-robin 的游标总指向「上一条被取走的轨的下一个位置」，
 * 所以只要有超过一条轨还有货，下一条必然换轨，连续同轨的 streak 恒为 1；
 * streak 大于 1 只可能发生在「只剩一条轨还有货」时，而那种情形下加限制会让这条轨永远排不进去。
 * 也就是说「连续同轨上限」在这一算法下**永远不可能触发**，是死代码
 * （穷举 262144 组 quota × pool 组合，触发 0 次），已按契约 §6.2 从规则清单里删掉。
 *
 * 纯函数：同输入必同输出。
 */

/**
 * 把每轨的候选单元排成一条跨轨交错的队列。
 * @param {{quota: {[track: string]: number}, pools: {[track: string]: Array<object>}}} input
 * @returns {Array<{unit: object, track: string, seq: number}>}
 */
function buildQueue(input) {
  const src = input || {}
  const quota = src.quota || {}
  const pools = src.pools || {}
  const codes = Object.keys(quota)
  if (codes.length === 0) return []

  const states = codes.map(function (code) {
    const count = Math.max(0, Math.trunc(Number(quota[code]) || 0))
    return {
      code: code,
      quota: count,
      left: count,
      pool: Array.isArray(pools[code]) ? pools[code] : [],
      index: 0,
    }
  })

  const out = []
  let cursor = 0

  while (true) {
    // 还有名额、并且池子里还有货的轨
    const alive = states.filter(function (s) {
      return s.left > 0 && s.index < s.pool.length
    })
    if (alive.length === 0) break

    // 从游标位置起，找第一条还有货的轨
    let picked = null
    for (let step = 0; step < states.length; step++) {
      const idx = (cursor + step) % states.length
      const s = states[idx]
      if (s.left <= 0 || s.index >= s.pool.length) continue
      picked = { state: s, index: idx }
      break
    }
    if (!picked) break

    const s = picked.state
    const unit = s.pool[s.index]
    s.index += 1
    s.left -= 1
    out.push({ unit: unit, track: s.code, seq: out.length + 1 })
    cursor = (picked.index + 1) % states.length
  }

  return out
}

module.exports = { buildQueue: buildQueue }
