/**
 * graph-layout.js —— 知识图谱布局（纯计算，可单测）
 *
 * 原 Taro 版用 SVG + 按 domain 分列的树状布局。小程序不支持 SVG DOM，
 * 这里保留同一套坐标算法，输出可直接交给 <canvas type="2d"> 绘制的点位。
 *
 * 布局规则（与原版一致）：
 *   - domain 沿 x 轴均分列
 *   - 同域节点沿 y 轴自上而下均匀分布
 *   - 半径 r = 8 + level * 2
 */

const DOMAIN_COLORS = {
  '算法': '#30A46C',
  '系统设计': '#7F77DD',
  'C/C++': '#E5484D',
  '数据库': '#0D9488',
  '计算机网络': '#0284C7'
}

function domainColor (domain) {
  return DOMAIN_COLORS[domain] || '#64748B'
}

/**
 * 计算布局
 * @param {Array<{id:string,name:string,domain:string,level:number}>} nodes
 * @param {Array<{source:string,target:string}>} links
 * @param {{width:number,height:number,padding?:number}} box画布尺寸（px）
 * @returns {{points:Array,edges:Array,width:number,height:number}}
 */
function layout (nodes, links, box) {
  const width = box.width || 320
  const height = box.height || 360
  const padding = box.padding || 28

  // 按 domain 分组
  const groups = {}
  const order = []
  nodes.forEach(function (n) {
    if (!groups[n.domain]) {
      groups[n.domain] = []
      order.push(n.domain)
    }
    groups[n.domain].push(n)
  })

  const colCount = order.length || 1
  const usableW = width - padding * 2
  const usableH = height - padding * 2
  const colGap = colCount > 1 ? usableW / (colCount - 1) : 0

  const points = []
  order.forEach(function (domain, ci) {
    const list = groups[domain]
    const x = colCount > 1 ? padding + ci * colGap : width / 2
    const n = list.length
    list.forEach(function (n2, i) {
      // 单节点时垂直居中，多节点时均匀铺开
      const y = n === 1 ? height / 2 : padding + (usableH / (n - 1)) * i
      points.push({
        id: n2.id,
        name: n2.name,
        domain: domain,
        level: n2.level,
        x: x,
        y: y,
        r: 8 + n2.level * 2,
        color: domainColor(domain)
      })
    })
  })

  // 边：只画两端都存在的
  const pos = {}
  points.forEach(function (p) { pos[p.id] = p })
  const edges = []
  ;(links || []).forEach(function (l) {
    if (pos[l.source] && pos[l.target]) {
      edges.push({ from: l.source, to: l.target, type: l.type })
    }
  })

  return { points: points, edges: edges, width: width, height: height }
}

module.exports = {
  DOMAIN_COLORS: DOMAIN_COLORS,
  domainColor: domainColor,
  layout: layout
}