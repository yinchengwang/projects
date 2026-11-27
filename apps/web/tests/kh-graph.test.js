/**
 * graph.test.js —— 知识图谱布局算法单测
 *
 * 布局是纯计算（core/graph-layout.js），原版用 SVG、小程序改为 Canvas，
 * 坐标必须稳定，因此单独覆盖。
 */

const assert = require('assert')
const gl = require('../shared/knowledge/graph-layout.js')

let passed = 0
function ok (name, cond) {
  if (!cond) throw new Error('FAIL: ' + name)
  passed += 1
  process.stdout.write('  \x1b[32m·\x1b[0m ' + name + '\n')
}

const nodes = [
  { id: 'a1', name: 'A1', domain: '算法', level: 4 },
  { id: 'a2', name: 'A2', domain: '算法', level: 2 },
  { id: 's1', name: 'S1', domain: '系统设计', level: 3 },
  { id: 'd1', name: 'D1', domain: '数据库', level: 1 }
]
const links = [
  { source: 'a1', target: 'a2' },
  { source: 'd1', target: 'x-nonexistent' } // 悬空边应被丢弃
]

const geo = gl.layout(nodes, links, { width: 320, height: 360 })

ok('点位数=节点数', geo.points.length === 4)
ok('悬空边被丢弃', geo.edges.length === 1)
ok('保留有效边', geo.edges[0].from === 'a1' && geo.edges[0].to === 'a2')

// 半径随 level 递增：r = 8 + level*2
const a1 = geo.points.filter(function (p) { return p.id === 'a1' })[0]
const d1 = geo.points.filter(function (p) { return p.id === 'd1' })[0]
ok('半径按 level 递增', a1.r > d1.r)
ok('半径公式正确', a1.r === 8 + 4 * 2)

// 同域节点 y 不同
const yA1 = geo.points.filter(function (p) { return p.id === 'a1' })[0].y
const yA2 = geo.points.filter(function (p) { return p.id === 'a2' })[0].y
ok('同域节点纵向分布', yA1 !== yA2)

// 不同域 x 不同（按列）
const xA = geo.points.filter(function (p) { return p.id === 'a1' })[0].x
const xS = geo.points.filter(function (p) { return p.id === 's1' })[0].x
ok('不同域分列', xA !== xS)

// 坐标在画布内
ok('坐标在画布内', geo.points.every(function (p) {
  return p.x >= 0 && p.x <= 320 && p.y >= 0 && p.y <= 360
}))

// 每个点都有颜色
ok('每点有颜色', geo.points.every(function (p) { return typeof p.color === 'string' && p.color.charAt(0) === '#' }))

// 单节点域垂直居中
const solo = gl.layout([{ id: 'x', name: 'X', domain: 'solo', level: 2 }], [], { width: 200, height: 100 })
ok('单节点垂直居中', solo.points[0].y === 50)

// 空输入不崩
const empty = gl.layout([], [], { width: 100, height: 100 })
ok('空输入返回空', empty.points.length === 0 && empty.edges.length === 0)

console.log('\n\x1b[32m✓ kh-graph.test.js\x1b[0m  ' + passed + ' 条断言通过')