/**
 * radar-layout.js —— 技术雷达布局（纯计算，可单测）
 *
 * 原版用 SVG 绘制气泡图 + 8 轮力导向碰撞，小程序不支持 SVG DOM，
 * 这里保留同一套布局算法，输出交给 <canvas type="2d"> 绘制。
 *
 * 两种视图：
 *   1. quadrant —— 按「象限 × 环」分桶的气泡图
 *   2. stack    —— 各技术栈的掌握度雷达多边形
 */

const QUADRANT_META = {
  language: { label: '语言', color: '#0284C7' },
  systems: { label: '系统', color: '#7F77DD' },
  algorithms: { label: '算法', color: '#30A46C' },
  engineering: { label: '工程', color: '#F5A524' }
}

const RING_META = {
  basic: { label: '基础', r: 40 },
  intermediate: { label: '进阶', r: 70 },
  advanced: { label: '精通', r: 100 }
}

const QUADRANT_ORDER = ['language', 'systems', 'algorithms', 'engineering']
const RING_ORDER = ['basic', 'intermediate', 'advanced']

/**
 * 象限气泡布局
 * 四象限均分画布，环决定半径基准，每象限内点位均分角度
 */
function quadrantBubbles (items, box) {
  const width = box.width || 320
  const height = box.height || 320
  const cx = width / 2
  const cy = height / 2
  const radius = Math.min(width, height) / 2 - 24

  const out = []
  QUADRANT_ORDER.forEach(function (qKey, qi) {
    const list = items.filter(function (it) { return it.quadrant === qKey })
    if (!list.length) return
    const qAngle = (qi / QUADRANT_ORDER.length) * Math.PI * 2
    list.forEach(function (it, i) {
      const ring = RING_META[it.ring] || RING_META.basic
      // 环内再均分一小段角度，避免完全重叠
      const spread = (Math.PI / QUADRANT_ORDER.length) * 0.7
      const step = list.length > 1 ? (i - (list.length - 1) / 2) * (spread / list.length) : 0
      const angle = qAngle + step
      const rr = (ring.r / 100) * radius
      out.push({
        id: it.id,
        title: it.title,
        quadrant: qKey,
        quadrantLabel: QUADRANT_META[qKey].label,
        ring: it.ring,
        ringLabel: ring.label,
        color: QUADRANT_META[qKey].color,
        r: 6 + (it.recommend || 0) * 1.2,
        x: cx + Math.cos(angle) * rr,
        y: cy + Math.sin(angle) * rr
      })
    })
  })
  return { points: out, width: width, height: height, cx: cx, cy: cy, radius: radius }
}

/**
 * 8 轮两两碰撞推开（原版的力导向简化版）
 * 返回调整后的点位，避免气泡重叠
 */
function resolveCollisions (points, box) {
  const minGap = box.minGap || 26
  const rounds = box.rounds || 8
  const out = points.map(function (p) { return { x: p.x, y: p.y, r: p.r, id: p.id } })

  for (let round = 0; round < rounds; round++) {
    for (let i = 0; i < out.length; i++) {
      for (let j = i + 1; j < out.length; j++) {
        const a = out[i]
        const b = out[j]
        const dx = b.x - a.x
        const dy = b.y - a.y
        const dist = Math.sqrt(dx * dx + dy * dy) || 0.01
        const overlap = a.r + b.r + minGap - dist
        if (overlap > 0) {
          const nx = dx / dist
          const ny = dy / dist
          const push = overlap / 2
          a.x -= nx * push
          a.y -= ny * push
          b.x += nx * push
          b.y += ny * push
        }
      }
    }
  }
  return out
}

/**
 * 技术栈雷达多边形
 * @param {Array<{key:string,label:string,color:string,percent:number}>} rows
 */
function stackPolygon (rows, box) {
  const width = box.width || 320
  const height = box.height || 320
  const cx = width / 2
  const cy = height / 2
  const radius = Math.min(width, height) / 2 - 30
  const n = rows.length
  if (!n) return { points: [], axes: [], width: width, height: height }

  const points = rows.map(function (r, i) {
    const angle = (i / n) * Math.PI * 2 - Math.PI / 2
    const rr = (r.percent / 100) * radius
    return { key: r.key, label: r.label, color: r.color, percent: r.percent, x: cx + Math.cos(angle) * rr, y: cy + Math.sin(angle) * rr }
  })

  // 轴（外圈）
  const axes = rows.map(function (r, i) {
    const angle = (i / n) * Math.PI * 2 - Math.PI / 2
    return {
      key: r.key,
      label: r.label,
      x: cx + Math.cos(angle) * radius,
      y: cy + Math.sin(angle) * radius
    }
  })

  return { points: points, axes: axes, width: width, height: height, cx: cx, cy: cy, radius: radius }
}

module.exports = {
  QUADRANT_META: QUADRANT_META,
  RING_META: RING_META,
  QUADRANT_ORDER: QUADRANT_ORDER,
  RING_ORDER: RING_ORDER,
  quadrantBubbles: quadrantBubbles,
  resolveCollisions: resolveCollisions,
  stackPolygon: stackPolygon
}