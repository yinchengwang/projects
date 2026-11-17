"use strict"
/**
 * quota.js —— 跨轨配额分配（最大余数法 / Hare quota）
 *
 * 一次会话的 N 个单元要分给三条轨。分法（契约 §6.1，逐字实现）：
 *   1. 需求度   demand[t] = 1 + min(1, due[t] / 5)   复习积压多的轨加权
 *   2. 有效权重 w'[t] = w[t] × demand[t]
 *   3. 配额     n[t] = floor(N × w'[t] / Σw')      余数按小数从大到小补给前面
 *   4. 回收     某轨候选不够时，多余配额按 w' 比例分给仍有候选的轨
 *   5. 兜底     所有轨都没候选 → needMore = true
 *
 * 纯函数：同输入必同输出（不读时钟、不用随机数）。
 */

const { labelOf } = require("../track/tracks.js")

/**
 * 最大余数法：把 total 个名额按 weight 比例分给 items。
 * 余数相同时按 items 的声明顺序优先。
 * @param {Array<{key: string, weight: number}>} items
 * @param {number} total
 * @returns {{[key: string]: number}}
 */
function largestRemainder(items, total) {
  const list = items || []
  const out = {}
  list.forEach(function (item) {
    out[item.key] = 0
  })
  const count = Math.max(0, Math.trunc(Number(total) || 0))
  if (list.length === 0 || count === 0) return out

  const sum = list.reduce(function (acc, item) {
    return acc + Math.max(0, Number(item.weight) || 0)
  }, 0)

  if (sum <= 0) {
    // 权重全为 0：各轨等分，余数按声明顺序补给靠前的轨
    const each = Math.floor(count / list.length)
    let rest = count - each * list.length
    list.forEach(function (item) {
      out[item.key] = each
      if (rest > 0) {
        out[item.key] += 1
        rest -= 1
      }
    })
    return out
  }

  const rests = []
  let used = 0
  list.forEach(function (item, index) {
    const weight = Math.max(0, Number(item.weight) || 0)
    const exact = (count * weight) / sum
    const base = Math.floor(exact)
    out[item.key] = base
    used += base
    rests.push({ key: item.key, index: index, rest: exact - base })
  })
  rests.sort(function (a, b) {
    if (b.rest !== a.rest) return b.rest - a.rest
    return a.index - b.index
  })
  let left = count - used
  for (let i = 0; i < rests.length && left > 0; i++) {
    out[rests[i].key] += 1
    left -= 1
  }
  return out
}

/**
 * 分配一次会话的跨轨配额。
 * @param {{n: number, tracks: Array<{track: string, weight: number, due: number, candidates: number}>, minPerTrack?: number}} input
 * @returns {{quota: {[track: string]: number}, demand: {[track: string]: number}, weights: {[track: string]: number}, needMore: boolean, reasons: string[]}}
 */
function allocateQuota(input) {
  const src = input || {}
  const tracks = Array.isArray(src.tracks) ? src.tracks : []
  const n = Math.max(0, Math.trunc(Number(src.n) || 0))
  const minPerTrack =
    src.minPerTrack === undefined || src.minPerTrack === null
      ? 0
      : Math.max(0, Math.trunc(Number(src.minPerTrack)))

  const demand = {}
  const weights = {}
  const quota = {}
  const reasons = []

  if (tracks.length === 0) {
    return {
      quota: quota,
      demand: demand,
      weights: weights,
      needMore: true,
      reasons: ["没有可用的轨道，无法分配配额"],
    }
  }

  const rows = tracks.map(function (t) {
    const item = t || {}
    const track = String(item.track === undefined || item.track === null ? "" : item.track)
    const due = Math.max(0, Number(item.due) || 0)
    const candidates = Math.max(0, Math.trunc(Number(item.candidates) || 0))
    const weight = Math.max(0, Number(item.weight) || 0)
    const d = 1 + Math.min(1, due / 5)
    demand[track] = d
    weights[track] = weight * d
    quota[track] = 0
    return { track: track, due: due, candidates: candidates, weight: weights[track] }
  })

  const live = rows.filter(function (row) {
    return row.candidates > 0
  })
  const totalCandidates = rows.reduce(function (acc, row) {
    return acc + row.candidates
  }, 0)

  if (totalCandidates === 0) {
    reasons.push("没有可学的单元，建议去供给流水线补内容")
    return { quota: quota, demand: demand, weights: weights, needMore: true, reasons: reasons }
  }

  const weightItems = live.map(function (row) {
    return { key: row.track, weight: row.weight }
  })

  // 第一步：minPerTrack 保底（默认 0，等于不保底），剩下的名额按有效权重分
  let alloc = {}
  if (minPerTrack > 0) {
    // 保底阶段：按声明顺序**尽力**给到 minPerTrack（受候选数限制），名额用完就停；
    // 然后剩下的名额继续按有效权重分给还有空位的轨——**一个名额都不许丢**。
    // （历史 bug：名额不够保底时这里「每轨只给 1 个」、剩下的直接丢掉，
    //   候选有 30 个却只排出 3 个。现在保证 Σquota = min(n, Σcandidates)。）
    let left = n
    live.forEach(function (row) {
      const take = Math.min(minPerTrack, row.candidates, left)
      alloc[row.track] = take
      left -= take
    })
    while (left > 0) {
      const receivers = live.filter(function (row) {
        return (alloc[row.track] || 0) < row.candidates
      })
      if (receivers.length === 0) break
      const give = largestRemainder(
        receivers.map(function (row) {
          return { key: row.track, weight: row.weight }
        }),
        left
      )
      let moved = 0
      receivers.forEach(function (row) {
        const room = row.candidates - (alloc[row.track] || 0)
        const take = Math.min(give[row.track] || 0, room)
        alloc[row.track] = (alloc[row.track] || 0) + take
        moved += take
      })
      if (moved === 0) break
      left -= moved
    }
  } else {
    alloc = largestRemainder(weightItems, n)
  }

  // 第二步：候选不够的轨压到候选数，多出来的名额按有效权重分给仍有候选的轨
  let deficit = 0
  live.forEach(function (row) {
    if (alloc[row.track] > row.candidates) {
      deficit += alloc[row.track] - row.candidates
      alloc[row.track] = row.candidates
      reasons.push(
        labelOf(row.track) + "轨可学的单元不够（只有 " + row.candidates + " 个）"
      )
    }
  })

  while (deficit > 0) {
    const receivers = live.filter(function (row) {
      return alloc[row.track] < row.candidates
    })
    if (receivers.length === 0) break
    const give = largestRemainder(
      receivers.map(function (row) {
        return { key: row.track, weight: row.weight }
      }),
      deficit
    )
    let moved = 0
    receivers.forEach(function (row) {
      const room = row.candidates - alloc[row.track]
      const take = Math.min(give[row.track], room)
      alloc[row.track] += take
      moved += take
    })
    if (moved === 0) break
    deficit -= moved
  }

  live.forEach(function (row) {
    quota[row.track] = alloc[row.track] || 0
  })

  const planned = live.reduce(function (acc, row) {
    return acc + quota[row.track]
  }, 0)
  if (planned < n) {
    reasons.push("可学的单元不够 " + n + " 个，这次只排 " + planned + " 个")
  }

  return {
    quota: quota,
    demand: demand,
    weights: weights,
    needMore: totalCandidates === 0 || totalCandidates < n,
    reasons: reasons,
  }
}

module.exports = { allocateQuota: allocateQuota, largestRemainder: largestRemainder }
