"use strict"
/**
 * charts.test.js —— 手绘 SVG 图表的纯函数层
 *
 * 这一层不碰 DOM：几何计算和 SVG 字符串拼接都是纯函数，可以直接单测。
 * 渲染层（web/diagnose.js）只负责把这里的字符串塞进 innerHTML。
 */

const test = require("node:test")
const assert = require("node:assert")

const Charts = require("../web/charts.js")

/** 断言一个 SVG 字符串里没有 NaN / undefined / Infinity */
function assertClean(svg) {
  assert.strictEqual(typeof svg, "string")
  assert.ok(svg.indexOf("NaN") === -1, "SVG 里出现了 NaN")
  assert.ok(svg.indexOf("undefined") === -1, "SVG 里出现了 undefined")
  assert.ok(svg.indexOf("Infinity") === -1, "SVG 里出现了 Infinity")
}

/* ---------------- 通用 ---------------- */

test("escapeXml 把五个特殊字符转义", function () {
  assert.strictEqual(Charts.escapeXml('a&b<c>d"e\'f'), "a&amp;b&lt;c&gt;d&quot;e&apos;f")
  assert.strictEqual(Charts.escapeXml(null), "")
})

test("num 遇到非法数返回 0，绝不产生 NaN", function () {
  assert.strictEqual(Charts.num(1.234), "1.23")
  assert.strictEqual(Charts.num(NaN), "0")
  assert.strictEqual(Charts.num(undefined), "0")
  assert.strictEqual(Charts.num(Infinity), "0")
})

test("clamp01 把值夹到 0..1", function () {
  assert.strictEqual(Charts.clamp01(0.5), 0.5)
  assert.strictEqual(Charts.clamp01(-1), 0)
  assert.strictEqual(Charts.clamp01(2), 1)
  assert.strictEqual(Charts.clamp01(NaN), 0)
})

test("formatPercent 把 0..1 转成百分数字符串", function () {
  assert.strictEqual(Charts.formatPercent(0.42), "42%")
  assert.strictEqual(Charts.formatPercent(0), "0%")
  assert.strictEqual(Charts.formatPercent(1), "100%")
  assert.strictEqual(Charts.formatPercent(NaN), "0%")
  assert.strictEqual(Charts.formatPercent(0.4213, 1), "42.1%")
})

/* ---------------- 雷达图 ---------------- */

test("radarPoints 从正上方开始顺时针排四个点", function () {
  // 四个维度都取满值 1，点才落在外圈上，能看出角度分布
  const points = Charts.radarPoints([1, 1, 1, 1], 200, { padding: 0 })
  assert.strictEqual(points.length, 4)
  assert.ok(Math.abs(points[0].x - 100) < 0.01, "x=" + points[0].x)
  assert.ok(Math.abs(points[0].y - 0) < 0.01, "y=" + points[0].y)
  assert.ok(Math.abs(points[1].x - 200) < 0.01, "x=" + points[1].x)
  assert.ok(Math.abs(points[1].y - 100) < 0.01, "y=" + points[1].y)
  assert.ok(Math.abs(points[2].x - 100) < 0.01, "x=" + points[2].x)
  assert.ok(Math.abs(points[2].y - 200) < 0.01, "y=" + points[2].y)
  assert.ok(Math.abs(points[3].x - 0) < 0.01, "x=" + points[3].x)
  assert.ok(Math.abs(points[3].y - 100) < 0.01, "y=" + points[3].y)
})

test("radarPoints 值越大离圆心越远，超出 1 会被夹住", function () {
  const near = Charts.radarPoints([0.5], 200, { padding: 0 })[0]
  const far = Charts.radarPoints([1], 200, { padding: 0 })[0]
  const over = Charts.radarPoints([5], 200, { padding: 0 })[0]
  // 圆心在 y=100：0.5 离圆心 50，1 离圆心 100
  assert.ok(Math.abs(far.y - 100) > Math.abs(near.y - 100))
  assert.strictEqual(over.y, far.y)
})

test("radarPoints 空数组返回空数组", function () {
  assert.deepStrictEqual(Charts.radarPoints([], 200), [])
  assert.deepStrictEqual(Charts.radarPoints(undefined, 200), [])
})

test("radarAxes 每个标签一条轴，右侧左对齐、左侧右对齐", function () {
  const axes = Charts.radarAxes(["上", "右", "下", "左"], 200, { padding: 0 })
  assert.strictEqual(axes.length, 4)
  assert.strictEqual(axes[0].anchor, "middle")
  assert.strictEqual(axes[1].anchor, "start")
  assert.strictEqual(axes[2].anchor, "middle")
  assert.strictEqual(axes[3].anchor, "end")
  assert.strictEqual(axes[0].label, "上")
})

test("radarGrid 返回若干同心多边形环", function () {
  const grid = Charts.radarGrid(5, 200, { padding: 0, rings: 4 })
  assert.strictEqual(grid.length, 4)
  assert.strictEqual(grid[3].level, 1)
  assert.strictEqual(grid[0].points.length, 5)
})

test("polygonPath 拼出闭合多边形", function () {
  const d = Charts.polygonPath([{ x: 0, y: 0 }, { x: 10, y: 20 }])
  assert.strictEqual(d, "M 0,0 L 10,20 Z")
  assert.strictEqual(Charts.polygonPath([]), "")
})

test("radarSvg 产出干净的 SVG，带 title 提示与两条数据系列", function () {
  const svg = Charts.radarSvg({
    labels: ["数组", "哈希表"],
    series: [
      { name: "掌握度", values: [0.4, 0.8] },
      { name: "覆盖度", values: [0.2, 0.5] },
    ],
    title: "掌握度雷达图",
  })
  assertClean(svg)
  assert.ok(svg.indexOf("<title>掌握度雷达图</title>") !== -1)
  assert.ok(svg.indexOf('viewBox="0 0 360 360"') !== -1, "默认尺寸 360")
  assert.ok(svg.indexOf("stroke-width=\"2\"") !== -1, "数据线 2px")
  assert.ok(svg.indexOf("fill-opacity=\"0.1\"") !== -1, "面积 10% 透明")
  assert.ok(svg.indexOf("stroke-dasharray") === -1, "网格不许画虚线")
  assert.ok(svg.indexOf("数组") !== -1)
})

test("radarSvg 数据为空时给占位文案而不是空白图", function () {
  const svg = Charts.radarSvg({ labels: [], series: [] })
  assertClean(svg)
  assert.ok(svg.indexOf("暂无数据") !== -1)
})

test("radarSvg 遇到 NaN 也不会写出 NaN", function () {
  const svg = Charts.radarSvg({
    labels: ["a", "b"],
    series: [{ name: "s", values: [NaN, 0.5] }],
  })
  assertClean(svg)
})

/* ---------------- 折线 + 面积 ---------------- */

test("linePoints 把数据映射到画布坐标", function () {
  const points = Charts.linePoints([0, 10, 20], 300, 100, {
    padding: { top: 0, right: 0, bottom: 0, left: 0 },
    max: 20,
  })
  assert.strictEqual(points.length, 3)
  assert.deepStrictEqual(points[0], { x: 0, y: 100 })
  assert.deepStrictEqual(points[1], { x: 150, y: 50 })
  assert.deepStrictEqual(points[2], { x: 300, y: 0 })
})

test("linePoints 只有一个点时放在中间", function () {
  const points = Charts.linePoints([5], 200, 100, {
    padding: { top: 0, right: 0, bottom: 0, left: 0 },
    max: 5,
  })
  assert.strictEqual(points.length, 1)
  assert.deepStrictEqual(points[0], { x: 100, y: 0 })
})

test("linePath 与 areaPath 的形状", function () {
  const points = [{ x: 0, y: 10 }, { x: 10, y: 0 }]
  assert.strictEqual(Charts.linePath(points), "M 0,10 L 10,0")
  assert.strictEqual(Charts.areaPath(points, 40), "M 0,40 L 0,10 L 10,0 L 10,40 Z")
  assert.strictEqual(Charts.linePath([]), "")
  assert.strictEqual(Charts.areaPath([], 40), "")
})

test("niceScale 把上界取整到整齐的刻度", function () {
  const scale = Charts.niceScale(5, 3)
  assert.strictEqual(scale.step, 2)
  assert.strictEqual(scale.max, 6)
  const scale2 = Charts.niceScale(12, 3)
  assert.strictEqual(scale2.step, 4)
  assert.strictEqual(scale2.max, 12)
})

test("sparseIndexes 至少保住首尾", function () {
  const idx = Charts.sparseIndexes(30, 3)
  assert.strictEqual(idx.length, 3)
  assert.strictEqual(idx[0], 0)
  assert.strictEqual(idx[idx.length - 1], 29)
  assert.deepStrictEqual(Charts.sparseIndexes(2, 5), [0, 1])
  assert.deepStrictEqual(Charts.sparseIndexes(0, 3), [])
})

test("lineSvg 产出干净的 SVG，带 hairline 网格与末点标记", function () {
  const svg = Charts.lineSvg({
    points: [
      { date: "2026-11-01", count: 0 },
      { date: "2026-11-02", count: 3 },
      { date: "2026-11-03", count: 1 },
    ],
    title: "每日题量曲线",
  })
  assertClean(svg)
  assert.ok(svg.indexOf("<title>每日题量曲线</title>") !== -1)
  assert.ok(svg.indexOf("stroke-dasharray") === -1, "网格不许画虚线")
  assert.ok(svg.indexOf('class="chart-point"') !== -1, "末点要有标记")
  assert.ok(svg.indexOf("2026-11-02") !== -1, "x 轴要有日期")
})

test("lineSvg 数据为空时给占位文案", function () {
  const svg = Charts.lineSvg({ points: [] })
  assertClean(svg)
  assert.ok(svg.indexOf("暂无数据") !== -1)
})

/* ---------------- 水平条形 ---------------- */

test("barRows 的条宽按比例，标签区在左、数值区在右", function () {
  const rows = Charts.barRows(
    [
      { label: "数组", value: 5, display: "50%" },
      { label: "哈希表", value: 10, display: "100%" },
    ],
    { width: 200, labelWidth: 50, valueWidth: 50, rowHeight: 20, barHeight: 10 }
  )
  assert.strictEqual(rows.length, 2)
  assert.strictEqual(rows[0].x, 50)
  assert.strictEqual(rows[0].width, 50)
  assert.strictEqual(rows[0].y, 5)
  assert.strictEqual(rows[1].width, 100)
  assert.strictEqual(rows[1].y, 25)
  assert.strictEqual(rows[0].display, "50%")
})

test("barPath 数据端圆角、基线端方角", function () {
  const d = Charts.barPath(0, 0, 100, 16, 4)
  assert.ok(d.indexOf("M 0,0") === 0, d)
  assert.ok(d.indexOf("A 4,4") !== -1, "数据端要有圆弧")
  assert.ok(d.indexOf("Z") !== -1)
})

test("barSvg 产出干净的 SVG，标签与数值都在", function () {
  const svg = Charts.barSvg({
    rows: [{ label: "数组", value: 0.5, display: "50%" }],
    title: "弱项榜",
  })
  assertClean(svg)
  assert.ok(svg.indexOf("<title>弱项榜</title>") !== -1)
  assert.ok(svg.indexOf("数组") !== -1)
  assert.ok(svg.indexOf("50%") !== -1)
})

test("barSvg 标签里的尖括号被转义", function () {
  const svg = Charts.barSvg({ rows: [{ label: "<script>", value: 1 }] })
  assertClean(svg)
  assert.ok(svg.indexOf("&lt;script&gt;") !== -1)
  assert.ok(svg.indexOf("<script>") === -1, "不能把标签当 HTML 塞进去")
})

test("barSvg 数据为空时给占位文案", function () {
  const svg = Charts.barSvg({ rows: [] })
  assertClean(svg)
  assert.ok(svg.indexOf("暂无数据") !== -1)
})
