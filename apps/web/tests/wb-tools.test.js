/**
 * tools.test.js —— 工具箱页面交互测试
 *
 * 加载真实页面模块，模拟 onOpen / onInput / 选项切换，断言输出。
 * 覆盖 10 个工具的编码→解码往返、错误输入、以及搜索过滤。
 *
 * 用法：node tests/tools.test.js
 */

const assert = require('assert')
const stub = require('./wb-stub.js')

stub.installLocalStorage()

const h = require('./wb-harness.js')

let pass = 0
let fail = 0
const failures = []

function test (name, fn) {
  try {
    stub.reset()
  if (global.localStorage && typeof global.localStorage.clear === "function") global.localStorage.clear()
    fn()
    pass++
    console.log('  \x1b[32mPASS\x1b[0m ' + name)
  } catch (e) {
    fail++
    failures.push({ name: name, error: e.message })
    console.log('  \x1b[31mFAIL\x1b[0m ' + name + ' -> ' + e.message)
  }
}

const createPage = h.createPage

/** 模拟主输入 */
function input (page, value) {
  h.fire(page, 'onInput', value)
}

/** 模拟第二输入（diff / regex 用） */
function inputB (page, value) {
  h.fire(page, 'onInputB', value)
}

/** 打开工具面板 */
function open (page, id) {
  h.tap(page, 'onOpen', { id: id })
}

function tapData (page, handler, dataset) {
  h.tap(page, handler, dataset)
}

// ==================== 搜索 ====================

console.log('\n\x1b[1m搜索过滤\x1b[0m')

test('初始显示全部 10 个工具', () => {
  const page = createPage('pages/tools/index')
  assert.strictEqual(page.data.filtered.length, 10)
})

test('按中文名过滤', () => {
  const page = createPage('pages/tools/index')
  page.onSearch({ detail: { value: '时间' } })
  assert.strictEqual(page.data.filtered.length, 1)
  assert.strictEqual(page.data.filtered[0].id, 'timestamp')
})

test('按英文名过滤（大小写不敏感）', () => {
  const page = createPage('pages/tools/index')
  page.onSearch({ detail: { value: 'UUID' } })
  assert.strictEqual(page.data.filtered[0].id, 'uuid')
})

test('按关键词过滤', () => {
  const page = createPage('pages/tools/index')
  page.onSearch({ detail: { value: '哈希' } })
  assert.strictEqual(page.data.filtered[0].id, 'hash')
})

test('无匹配时返回空列表', () => {
  const page = createPage('pages/tools/index')
  page.onSearch({ detail: { value: 'zzzz' } })
  assert.strictEqual(page.data.filtered.length, 0)
})

test('清空搜索恢复全部', () => {
  const page = createPage('pages/tools/index')
  page.onSearch({ detail: { value: 'json' } })
  page.onClearSearch()
  assert.strictEqual(page.data.filtered.length, 10)
  assert.strictEqual(page.data.query, '')
})

// ==================== JSON ====================

console.log('\n\x1b[1mJSON\x1b[0m')

test('美化输出带缩进', () => {
  const page = createPage('pages/tools/index')
  open(page, 'json')
  input(page, '{"a":1,"b":[2,3]}')
  assert.strictEqual(page.data.output.indexOf('\n  "a"') >= 0, true)
})

test('压缩输出无空格', () => {
  const page = createPage('pages/tools/index')
  open(page, 'json')
  tapData(page, 'onJsonMode', { key: 'minify' })
  input(page, '{ "a" : 1 }')
  assert.strictEqual(page.data.output, '{"a":1}')
})

test('非法 JSON 显示错误且不崩', () => {
  const page = createPage('pages/tools/index')
  open(page, 'json')
  input(page, '{bad}')
  assert.strictEqual(page.data.error.length > 0, true)
  assert.strictEqual(page.data.output, '')
})

test('空输入清空结果', () => {
  const page = createPage('pages/tools/index')
  open(page, 'json')
  input(page, '{"a":1}')
  input(page, '')
  assert.strictEqual(page.data.output, '')
  assert.strictEqual(page.data.error, '')
})

// ==================== Base64 ====================

console.log('\n\x1b[1mBase64\x1b[0m')

test('编码中文', () => {
  const page = createPage('pages/tools/index')
  open(page, 'base64')
  input(page, '中文')
  assert.strictEqual(page.data.output, '5Lit5paH')
})

test('编码→解码往返一致', () => {
  const page = createPage('pages/tools/index')
  open(page, 'base64')
  const src = '你好 DevBench 🎉'
  input(page, src)
  const encoded = page.data.output
  tapData(page, 'onBase64Mode', { key: 'decode' })
  input(page, encoded)
  assert.strictEqual(page.data.output, src)
})

// ==================== URL ====================

console.log('\n\x1b[1mURL\x1b[0m')

test('编码特殊字符', () => {
  const page = createPage('pages/tools/index')
  open(page, 'url')
  input(page, 'a=1&b=中')
  assert.strictEqual(page.data.output.indexOf('%26') >= 0, true)
})

test('编码→解码往返一致', () => {
  const page = createPage('pages/tools/index')
  open(page, 'url')
  const src = 'key=值 with space&x=1'
  input(page, src)
  const encoded = page.data.output
  tapData(page, 'onUrlMode', { key: 'decode' })
  input(page, encoded)
  assert.strictEqual(page.data.output, src)
})

// ==================== 时间戳 ====================

console.log('\n\x1b[1m时间戳\x1b[0m')

test('打开即显示当前时间戳', () => {
  const page = createPage('pages/tools/index')
  open(page, 'timestamp')
  assert.strictEqual(/^\d{13}$/.test(page.data.tsNowMs), true)
  assert.strictEqual(/^\d{10}$/.test(page.data.tsNowS), true)
})

test('毫秒时间戳转日期', () => {
  const page = createPage('pages/tools/index')
  open(page, 'timestamp')
  input(page, '1700000000000')
  assert.strictEqual(page.data.tsDate.length > 0, true)
  assert.strictEqual(page.data.tsMs, '1700000000000')
  assert.strictEqual(page.data.tsS, '1700000000')
})

test('秒级时间戳自动识别', () => {
  const page = createPage('pages/tools/index')
  open(page, 'timestamp')
  input(page, '1700000000')
  assert.strictEqual(page.data.tsMs, '1700000000000')
})

test('日期字符串转时间戳', () => {
  const page = createPage('pages/tools/index')
  open(page, 'timestamp')
  input(page, '2026-10-22 09:00:00')
  assert.strictEqual(page.data.tsDate, '2026-10-22 09:00:00')
  assert.strictEqual(Number(page.data.tsMs) > 0, true)
})

test('无法识别时提示错误', () => {
  const page = createPage('pages/tools/index')
  open(page, 'timestamp')
  input(page, 'not-a-date')
  assert.strictEqual(page.data.error.length > 0, true)
})

test('用当前时间按钮填入毫秒', () => {
  const page = createPage('pages/tools/index')
  open(page, 'timestamp')
  page.onUseNow()
  assert.strictEqual(/^\d{13}$/.test(page.data.input), true)
})

// ==================== UUID ====================

console.log('\n\x1b[1mUUID\x1b[0m')

test('打开即生成 5 个', () => {
  const page = createPage('pages/tools/index')
  open(page, 'uuid')
  assert.strictEqual(page.data.uuidList.length, 5)
})

test('切换数量重新生成', () => {
  const page = createPage('pages/tools/index')
  open(page, 'uuid')
  tapData(page, 'onUuidCount', { count: 10 })
  assert.strictEqual(page.data.uuidList.length, 10)
})

test('生成结果符合 v4 格式', () => {
  const page = createPage('pages/tools/index')
  open(page, 'uuid')
  const re = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/
  page.data.uuidList.forEach(u => assert.strictEqual(re.test(u), true, u))
})

test('列表内无重复', () => {
  const page = createPage('pages/tools/index')
  open(page, 'uuid')
  tapData(page, 'onUuidCount', { count: 10 })
  assert.strictEqual(new Set(page.data.uuidList).size, 10)
})

// ==================== 正则 ====================

console.log('\n\x1b[1m正则\x1b[0m')

test('统计命中次数', () => {
  const page = createPage('pages/tools/index')
  open(page, 'regex')
  input(page, '\\d+')
  inputB(page, 'a1b22c333')
  assert.strictEqual(page.data.regexCount, 3)
})

test('高亮分段拼接等于原文', () => {
  const page = createPage('pages/tools/index')
  open(page, 'regex')
  input(page, '\\d+')
  inputB(page, 'a1b22')
  const joined = page.data.regexSegments.map(s => s.value).join('')
  assert.strictEqual(joined, 'a1b22')
})

test('非法正则显示错误', () => {
  const page = createPage('pages/tools/index')
  open(page, 'regex')
  input(page, '[')
  inputB(page, 'abc')
  assert.strictEqual(page.data.error.length > 0, true)
})

test('关闭 g 修饰符后只命中一次', () => {
  const page = createPage('pages/tools/index')
  open(page, 'regex')
  input(page, '\\d+')
  inputB(page, 'a1b22')
  tapData(page, 'onToggleFlag', { flag: 'g' })
  assert.strictEqual(page.data.regexFlags.indexOf('g'), -1)
  assert.strictEqual(page.data.regexCount, 1)
})

// ==================== 颜色 ====================

console.log('\n\x1b[1m颜色\x1b[0m')

test('HEX 转 RGB', () => {
  const page = createPage('pages/tools/index')
  open(page, 'color')
  input(page, '#0284C7')
  assert.strictEqual(page.data.colorHex, '#0284C7')
  assert.strictEqual(page.data.colorRgb, 'rgb(2, 132, 199)')
})

test('三位 HEX 展开', () => {
  const page = createPage('pages/tools/index')
  open(page, 'color')
  input(page, '#fff')
  assert.strictEqual(page.data.colorHex, '#FFFFFF')
})

test('RGB 转 HEX', () => {
  const page = createPage('pages/tools/index')
  open(page, 'color')
  input(page, 'rgb(2, 132, 199)')
  assert.strictEqual(page.data.colorHex, '#0284C7')
})

test('HEX ↔ RGB 往返一致', () => {
  const page = createPage('pages/tools/index')
  open(page, 'color')
  input(page, '#30A46C')
  const hex = page.data.colorHex
  input(page, page.data.colorRgb)
  assert.strictEqual(page.data.colorHex, hex)
})

test('非法输入提示错误', () => {
  const page = createPage('pages/tools/index')
  open(page, 'color')
  input(page, 'zzz')
  assert.strictEqual(page.data.error.length > 0, true)
})

// ==================== Hash ====================

console.log('\n\x1b[1mHash\x1b[0m')

test('同时输出 djb2 与 crc32', () => {
  const page = createPage('pages/tools/index')
  open(page, 'hash')
  input(page, 'hello')
  assert.strictEqual(page.data.hashDjb2.length, 8)
  assert.strictEqual(page.data.hashCrc32.length, 8)
})

test('crc32 标准校验值', () => {
  const page = createPage('pages/tools/index')
  open(page, 'hash')
  input(page, '123456789')
  assert.strictEqual(page.data.hashCrc32, 'cbf43926')
})

test('不同输入产生不同哈希', () => {
  const page = createPage('pages/tools/index')
  open(page, 'hash')
  input(page, 'a')
  const h1 = page.data.hashDjb2
  input(page, 'b')
  assert.notStrictEqual(page.data.hashDjb2, h1)
})

// ==================== Diff ====================

console.log('\n\x1b[1mDiff\x1b[0m')

test('识别新增与删除', () => {
  const page = createPage('pages/tools/index')
  open(page, 'diff')
  input(page, 'abc')
  inputB(page, 'abd')
  assert.strictEqual(page.data.diffOps.some(o => o.type === 'add'), true)
  assert.strictEqual(page.data.diffOps.some(o => o.type === 'del'), true)
})

test('统计文案包含新增与删除段数', () => {
  const page = createPage('pages/tools/index')
  open(page, 'diff')
  input(page, 'abc')
  inputB(page, 'abd')
  assert.strictEqual(/新增/.test(page.data.diffNote), true)
  assert.strictEqual(/删除/.test(page.data.diffNote), true)
})

test('diff 结果可还原两侧原文', () => {
  const page = createPage('pages/tools/index')
  open(page, 'diff')
  const a = 'hello world'
  const b = 'hello there'
  input(page, a)
  inputB(page, b)
  const ops = page.data.diffOps
  const left = ops.filter(o => o.type !== 'add').map(o => o.text).join('')
  const right = ops.filter(o => o.type !== 'del').map(o => o.text).join('')
  assert.strictEqual(left, a)
  assert.strictEqual(right, b)
})

test('超长文本被拦截并提示', () => {
  const page = createPage('pages/tools/index')
  open(page, 'diff')
  input(page, 'x'.repeat(2500))
  inputB(page, 'y')
  assert.strictEqual(page.data.diffOps.length, 0)
  assert.strictEqual(/2000/.test(page.data.diffNote), true)
})

test('两侧都空时无结果', () => {
  const page = createPage('pages/tools/index')
  open(page, 'diff')
  assert.strictEqual(page.data.diffOps.length, 0)
  assert.strictEqual(page.data.diffNote, '')
})

// ==================== Cron ====================

console.log('\n\x1b[1mCron\x1b[0m')

test('合法表达式给出 5 次触发', () => {
  const page = createPage('pages/tools/index')
  open(page, 'cron')
  input(page, '0 9 * * *')
  assert.strictEqual(page.data.cronNext.length, 5)
  page.data.cronNext.forEach(t => assert.strictEqual(/ 09:00:00$/.test(t), true, t))
})

test('段数不足提示错误', () => {
  const page = createPage('pages/tools/index')
  open(page, 'cron')
  input(page, '0 9 *')
  assert.strictEqual(page.data.cronError.length > 0, true)
  assert.strictEqual(page.data.cronNext.length, 0)
})

test('范围语法生效', () => {
  const page = createPage('pages/tools/index')
  open(page, 'cron')
  input(page, '0 9-17 * * *')
  assert.strictEqual(page.data.cronNext.length, 5)
  page.data.cronNext.forEach(t => {
    const h = Number(t.slice(11, 13))
    assert.strictEqual(h >= 9 && h <= 17, true, t)
  })
})

// ==================== 面板开关 ====================

console.log('\n\x1b[1m面板开关\x1b[0m')

test('打开工具设置标题与符号', () => {
  const page = createPage('pages/tools/index')
  open(page, 'cron')
  assert.strictEqual(page.data.active, 'cron')
  assert.strictEqual(page.data.activeLabel, 'Cron')
  assert.strictEqual(page.data.activeSymbol, '*')
})

test('关闭面板清空 active', () => {
  const page = createPage('pages/tools/index')
  open(page, 'json')
  page.onClose()
  assert.strictEqual(page.data.active, '')
})

test('切换工具时清空上一次的输入与结果', () => {
  const page = createPage('pages/tools/index')
  open(page, 'json')
  input(page, '{"a":1}')
  open(page, 'base64')
  assert.strictEqual(page.data.input, '')
  assert.strictEqual(page.data.output, '')
})

test('未知工具 id 不崩溃', () => {
  const page = createPage('pages/tools/index')
  open(page, 'nope')
  assert.strictEqual(page.data.active, '')
})

// ==================== 结果 ====================

console.log('\n' + '='.repeat(52))
console.log('通过 ' + pass + ' 项，失败 ' + fail + ' 项')
if (fail > 0) {
  console.log('\n失败明细：')
  failures.forEach(f => console.log('  - ' + f.name + ': ' + f.error))
  process.exit(1)
}
console.log('全部通过')
