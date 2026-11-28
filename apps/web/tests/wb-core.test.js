/**
 * core.test.js —— core 层回归测试
 *
 * 覆盖：store 工厂 CRUD、storage 导入导出、10 个工具函数、
 * 日期/格式化工具、主题解析、常量映射。
 *
 * 用法：node tests/core.test.js
 */

const assert = require('assert')
const stub = require('./wb-stub.js')

stub.installLocalStorage()

const codetools = require('../shared/workbench/codetools.js')
const dateUtil = require('../shared/core/date.js')
const fmt = require('../shared/core/format.js')
const storage = require('../shared/core/storage.js')
const store = require('../shared/core/store.js')
const theme = require('../shared/core/theme.js')
const constants = require('../shared/core/constants.js')

let pass = 0
let fail = 0
const failures = []

function test (name, fn) {
  try {
    fn()
    pass++
    console.log('  \x1b[32mPASS\x1b[0m ' + name)
  } catch (e) {
    fail++
    failures.push({ name: name, error: e.message })
    console.log('  \x1b[31mFAIL\x1b[0m ' + name + ' -> ' + e.message)
  }
}

// ==================== Base64 ====================

console.log('\n\x1b[1mBase64（纯 JS 实现）\x1b[0m')

test('ASCII 编码符合标准', () => {
  assert.strictEqual(codetools.base64Encode('hello'), 'aGVsbG8=')
})

test('中文编码为 UTF-8 base64', () => {
  assert.strictEqual(codetools.base64Encode('中文'), '5Lit5paH')
})

test('中文往返一致', () => {
  const src = '你好，DevBench！'
  assert.strictEqual(codetools.base64Decode(codetools.base64Encode(src)), src)
})

test('emoji（代理对）往返一致', () => {
  const src = '🎉 完成'
  assert.strictEqual(codetools.base64Decode(codetools.base64Encode(src)), src)
})

test('空串往返一致', () => {
  assert.strictEqual(codetools.base64Decode(codetools.base64Encode('')), '')
})

test('长度非 3 倍数时正确处理补位', () => {
  ;['a', 'ab', 'abc', 'abcd'].forEach(s => {
    assert.strictEqual(codetools.base64Decode(codetools.base64Encode(s)), s, '长度 ' + s.length)
  })
})

// ==================== URL / JSON ====================

console.log('\n\x1b[1mURL 与 JSON\x1b[0m')

test('URL 编解码往返', () => {
  const src = 'a=1&b=中文 空格'
  assert.strictEqual(codetools.urlDecode(codetools.urlEncode(src)), src)
})

test('URL 非法编码时原样返回', () => {
  assert.strictEqual(codetools.urlDecode('%E4%B8'), '%E4%B8')
})

test('JSON 美化', () => {
  assert.strictEqual(codetools.jsonTransform('{"a":1}', 'beautify', 2), '{\n  "a": 1\n}')
})

test('JSON 压缩', () => {
  assert.strictEqual(codetools.jsonTransform('{ "a" : 1 }', 'minify'), '{"a":1}')
})

test('JSON 非法输入抛错（由调用方捕获）', () => {
  assert.throws(() => codetools.jsonTransform('{bad}', 'beautify'))
})

test('空输入返回空串', () => {
  assert.strictEqual(codetools.jsonTransform('   ', 'beautify'), '')
})

// ==================== 时间戳 ====================

console.log('\n\x1b[1m时间戳\x1b[0m')

test('秒级时间戳自动识别为秒', () => {
  const d = codetools.tsToDate(1700000000)
  assert.strictEqual(d.getFullYear() >= 2023 && d.getFullYear() <= 2025, true)
})

test('毫秒级时间戳直接使用', () => {
  const d = codetools.tsToDate(1700000000000)
  assert.strictEqual(d.getTime(), 1700000000000)
})

test('fmtDate 按 pattern 输出', () => {
  const d = new Date(2026, 0, 5, 9, 8, 7)
  assert.strictEqual(codetools.fmtDate(d, 'YYYY-MM-DD HH:mm:ss'), '2026-01-05 09:08:07')
})

test('dateToTs 秒/毫秒切换', () => {
  const ms = codetools.dateToTs('2026-01-05T00:00:00')
  assert.strictEqual(Math.floor(ms / 1000), codetools.dateToTs('2026-01-05T00:00:00', 's'))
})

// ==================== UUID / 随机 ====================

console.log('\n\x1b[1mUUID 与随机串\x1b[0m')

test('UUID 符合 v4 格式', () => {
  const re = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/
  assert.strictEqual(re.test(codetools.uuid()), true)
})

test('UUID 不重复', () => {
  const set = new Set()
  for (let i = 0; i < 200; i++) set.add(codetools.uuid())
  assert.strictEqual(set.size, 200)
})

test('随机串长度与字符集正确', () => {
  const s = codetools.randomString(16, 'digit')
  assert.strictEqual(s.length, 16)
  assert.strictEqual(/^\d+$/.test(s), true)
})

// ==================== 颜色 ====================

console.log('\n\x1b[1m颜色转换\x1b[0m')

test('三位 HEX 展开', () => {
  assert.deepStrictEqual(codetools.hexToRgb('#FFF'), { r: 255, g: 255, b: 255 })
})

test('六位 HEX 解析', () => {
  // 用一个不在调色板里的颜色：这个用例测的是解析器，
  // 跟设计令牌无关 —— 换色时不该跟着改
  assert.deepStrictEqual(codetools.hexToRgb('#123456'), { r: 18, g: 52, b: 86 })
})

test('八位 HEX 带 alpha', () => {
  const rgb = codetools.hexToRgb('#12345680')
  assert.strictEqual(rgb.a !== undefined, true)
  assert.strictEqual(rgb.a > 0.4 && rgb.a < 0.6, true)
})

test('非法 HEX 返回 null', () => {
  assert.strictEqual(codetools.hexToRgb('zzz'), null)
  assert.strictEqual(codetools.hexToRgb('#12'), null)
})

test('RGB 转 HEX', () => {
  assert.strictEqual(codetools.rgbToHex({ r: 255, g: 255, b: 255 }), '#ffffff')
})

test('RGB 越界值被裁剪', () => {
  assert.strictEqual(codetools.rgbToHex({ r: 300, g: -10, b: 128 }), '#ff0080')
})

test('HEX ↔ RGB 往返一致', () => {
  const hex = '#0ea5e9'
  assert.strictEqual(codetools.rgbToHex(codetools.hexToRgb(hex)), hex)
})

// ==================== 正则 ====================

console.log('\n\x1b[1m正则测试\x1b[0m')

test('全局模式统计命中次数', () => {
  const r = codetools.testRegex('\\d+', 'a1b22c333')
  assert.strictEqual(r.matched, true)
  assert.strictEqual(r.matchCount, 3)
})

test('非全局模式只命中一次', () => {
  const r = codetools.testRegex('\\d+', 'a1b22', '')
  assert.strictEqual(r.matchCount, 1)
})

test('非法正则返回 error 而不抛异常', () => {
  const r = codetools.testRegex('[', 'abc')
  assert.strictEqual(r.matched, false)
  assert.strictEqual(typeof r.error, 'string')
})

test('高亮分段拼接后等于原文', () => {
  const src = 'a1b22c'
  const r = codetools.testRegex('\\d+', src)
  assert.strictEqual(r.highlighted.map(s => s.value).join(''), src)
})

test('捕获组提取', () => {
  const r = codetools.testRegex('(\\d+)-(\\d+)', '2026-10', '')
  assert.deepStrictEqual(r.groupsArr, ['2026', '10'])
})

// ==================== Hash ====================

console.log('\n\x1b[1mHash\x1b[0m')

test('djb2 空串为 5381', () => {
  assert.strictEqual(codetools.djb2(''), '00001505')
})

test('djb2 输出固定 8 位', () => {
  assert.strictEqual(codetools.djb2('hello').length, 8)
})

test('crc32 标准校验值', () => {
  // CRC32("123456789") = 0xCBF43926
  assert.strictEqual(codetools.crc32('123456789'), 'cbf43926')
})

test('crc32 输出固定 8 位', () => {
  assert.strictEqual(codetools.crc32('').length, 8)
})

test('同输入同输出（确定性）', () => {
  assert.strictEqual(codetools.djb2('abc'), codetools.djb2('abc'))
  assert.strictEqual(codetools.crc32('abc'), codetools.crc32('abc'))
})

// ==================== Diff ====================

console.log('\n\x1b[1m文本 Diff\x1b[0m')

test('相同文本全为 eq', () => {
  const ops = codetools.diffText('abc', 'abc')
  assert.strictEqual(ops.length, 1)
  assert.strictEqual(ops[0].type, 'eq')
  assert.strictEqual(ops[0].text, 'abc')
})

test('末尾替换识别为 del + add', () => {
  const ops = codetools.diffText('abc', 'abd')
  const types = ops.map(o => o.type)
  assert.deepStrictEqual(types, ['eq', 'del', 'add'])
})

test('新增字符识别为 add', () => {
  const ops = codetools.diffText('ab', 'abc')
  assert.strictEqual(ops.some(o => o.type === 'add' && o.text === 'c'), true)
})

test('删除字符识别为 del', () => {
  const ops = codetools.diffText('abc', 'ab')
  assert.strictEqual(ops.some(o => o.type === 'del' && o.text === 'c'), true)
})

test('diff 可还原两侧原文', () => {
  const a = 'hello world'
  const b = 'hello there'
  const ops = codetools.diffText(a, b)
  const left = ops.filter(o => o.type !== 'add').map(o => o.text).join('')
  const right = ops.filter(o => o.type !== 'del').map(o => o.text).join('')
  assert.strictEqual(left, a)
  assert.strictEqual(right, b)
})

// ==================== Cron ====================

console.log('\n\x1b[1mCron\x1b[0m')

test('合法表达式返回 5 个未来时间', () => {
  const r = codetools.describeCron('0 9 * * *')
  assert.strictEqual(r.ok, true)
  assert.strictEqual(r.nextRuns.length, 5)
  r.nextRuns.forEach(t => {
    assert.strictEqual(/ 09:00:00$/.test(t), true, t + ' 应为 09:00:00')
  })
})

test('段数不足报错', () => {
  const r = codetools.describeCron('0 9 *')
  assert.strictEqual(r.ok, false)
  assert.strictEqual(/5 段/.test(r.error), true)
})

test('超出范围报错', () => {
  assert.strictEqual(codetools.describeCron('99 * * * *').ok, false)
  assert.strictEqual(codetools.describeCron('* 25 * * *').ok, false)
})

test('步长语法 */5 生效', () => {
  const r = codetools.describeCron('*/5 * * * *')
  assert.strictEqual(r.ok, true)
  r.nextRuns.forEach(t => {
    const min = Number(t.slice(14, 16))
    assert.strictEqual(min % 5, 0, t + ' 分钟应为 5 的倍数')
  })
})

test('区间语法 9-17 生效', () => {
  const r = codetools.describeCron('0 9-17 * * *')
  assert.strictEqual(r.ok, true)
  r.nextRuns.forEach(t => {
    const hour = Number(t.slice(11, 13))
    assert.strictEqual(hour >= 9 && hour <= 17, true, t + ' 小时应在 9-17')
  })
})

// ==================== Markdown ====================

console.log('\n\x1b[1mMarkdown\x1b[0m')

test('标题渲染', () => {
  assert.strictEqual(codetools.md2html('# 标题').indexOf('<h1>标题</h1>') >= 0, true)
})

test('粗体与行内码', () => {
  const html = codetools.md2html('**粗** 和 `code`')
  assert.strictEqual(html.indexOf('<strong>粗</strong>') >= 0, true)
  assert.strictEqual(html.indexOf('md-inline-code') >= 0, true)
})

test('围栏代码块渲染为 pre', () => {
  const html = codetools.md2html('```js\nconst a = 1\n```')
  assert.strictEqual(html.indexOf('<pre') >= 0, true)
  assert.strictEqual(html.indexOf('const a = 1') >= 0, true)
})

test('代码块内的 * 不被当作强调', () => {
  const html = codetools.md2html('```\n2 * 3 * 4\n```')
  assert.strictEqual(html.indexOf('<em>') < 0, true)
})

test('HTML 特殊字符被转义', () => {
  const html = codetools.md2html('a < b & c > d')
  assert.strictEqual(html.indexOf('&lt;') >= 0, true)
  assert.strictEqual(html.indexOf('&amp;') >= 0, true)
})

test('空输入返回空串', () => {
  assert.strictEqual(codetools.md2html(''), '')
})

// ==================== 日期 ====================

console.log('\n\x1b[1m日期工具\x1b[0m')

test('formatYMD 补零', () => {
  assert.strictEqual(dateUtil.formatYMD(new Date(2026, 0, 5)), '2026-01-05')
})

test('weekRange 周一至周日', () => {
  // 2026-10-22 是周四
  const r = dateUtil.weekRange(new Date(2026, 9, 22))
  assert.strictEqual(r.start, '2026-10-19')
  assert.strictEqual(r.end, '2026-10-25')
})

test('weekRange 周日归入本周', () => {
  // 2026-10-25 是周日
  const r = dateUtil.weekRange(new Date(2026, 9, 25))
  assert.strictEqual(r.start, '2026-10-19')
})

test('formatRelative 刚刚', () => {
  assert.strictEqual(dateUtil.formatRelative(new Date().toISOString()), '刚刚')
})

test('formatRelative 天前', () => {
  const d = new Date(Date.now() - 3 * 86400000)
  assert.strictEqual(dateUtil.formatRelative(d.toISOString()), '3 天前')
})

test('formatRelative 空值返回空串', () => {
  assert.strictEqual(dateUtil.formatRelative(''), '')
})

test('dateListBetween 含首尾', () => {
  const list = dateUtil.dateListBetween('2026-10-20', '2026-10-22')
  assert.strictEqual(list.length, 3)
  assert.strictEqual(list[0], '2026-10-20')
  assert.strictEqual(list[2], '2026-10-22')
})

test('humanDay 今天/昨天', () => {
  assert.strictEqual(dateUtil.humanDay(dateUtil.todayStr()), '今天')
  assert.strictEqual(dateUtil.humanDay(dateUtil.daysAgo(1)), '昨天')
})

test('greeting 返回非空', () => {
  assert.strictEqual(typeof dateUtil.greeting(), 'string')
  assert.strictEqual(dateUtil.greeting().length > 0, true)
})

// ==================== 格式化 ====================

console.log('\n\x1b[1m格式化工具\x1b[0m')

test('percent 四舍五入', () => {
  assert.strictEqual(fmt.percent(1, 3), 33)
  assert.strictEqual(fmt.percent(2, 3), 67)
})

test('percent 除零返回 0', () => {
  assert.strictEqual(fmt.percent(5, 0), 0)
})

test('percent 上限 100', () => {
  assert.strictEqual(fmt.percent(200, 100), 100)
})

test('duration 三种形态', () => {
  assert.strictEqual(fmt.duration(45), '45 分钟')
  assert.strictEqual(fmt.duration(60), '1 小时')
  assert.strictEqual(fmt.duration(80), '1 小时 20 分钟')
})

test('duration 非法输入兜底', () => {
  assert.strictEqual(fmt.duration(-5), '0 分钟')
})

test('clamp 边界', () => {
  assert.strictEqual(fmt.clamp(150, 0, 100), 100)
  assert.strictEqual(fmt.clamp(-5, 0, 100), 0)
})

test('truncate 超长截断', () => {
  assert.strictEqual(fmt.truncate('abcdef', 3), 'abc…')
  assert.strictEqual(fmt.truncate('ab', 3), 'ab')
})

test('bytes 单位换算', () => {
  assert.strictEqual(fmt.bytes(512), '512 B')
  assert.strictEqual(fmt.bytes(2048), '2.00 KB')
})

// ==================== storage ====================

console.log('\n\x1b[1mstorage\x1b[0m')

test('写入后可读回', () => {
  stub.reset()
  storage.set('todos', [{ id: 'a' }])
  assert.deepStrictEqual(storage.get('todos', []), [{ id: 'a' }])
})

test('使用 db: 前缀', () => {
  stub.reset()
  storage.set('todos', [])
  assert.strictEqual(stub.raw('db:todos') !== undefined, true)
  assert.strictEqual(stub.raw('devbench:todos'), undefined)
})

test('读不到时返回默认值', () => {
  stub.reset()
  assert.strictEqual(storage.get('nope', 'def'), 'def')
})

test('删除后读不到', () => {
  stub.reset()
  storage.set('x', 1)
  storage.remove('x')
  assert.strictEqual(storage.get('x', null), null)
})

test('导出包含 exportedAt 与 data', () => {
  stub.reset()
  storage.set('todos', [{ id: 'a' }])
  const json = storage.exportAll()
  const parsed = JSON.parse(json)
  assert.strictEqual(typeof parsed.exportedAt, 'string')
  assert.deepStrictEqual(parsed.data.todos, [{ id: 'a' }])
})

test('导出不含 __seeded__', () => {
  stub.reset()
  storage.set('__seeded__', true)
  storage.set('todos', [])
  const parsed = JSON.parse(storage.exportAll())
  assert.strictEqual(parsed.data.__seeded__, undefined)
})

test('导入 overwrite 覆盖已有', () => {
  stub.reset()
  storage.set('todos', [{ id: 'old' }])
  const payload = JSON.stringify({ data: { todos: [{ id: 'new' }] } })
  const r = storage.importAll(payload, 'overwrite')
  assert.strictEqual(r.ok, true)
  assert.deepStrictEqual(storage.get('todos', []), [{ id: 'new' }])
})

test('导入 merge 不覆盖已有', () => {
  stub.reset()
  storage.set('todos', [{ id: 'old' }])
  const payload = JSON.stringify({ data: { todos: [{ id: 'new' }], mood: [{ id: 'm' }] } })
  storage.importAll(payload, 'merge')
  assert.deepStrictEqual(storage.get('todos', []), [{ id: 'old' }])
  assert.deepStrictEqual(storage.get('mood', []), [{ id: 'm' }])
})

test('导入非法 JSON 返回错误', () => {
  const r = storage.importAll('{bad}', 'merge')
  assert.strictEqual(r.ok, false)
  assert.strictEqual(typeof r.error, 'string')
})

test('导入缺少 data 字段报错', () => {
  const r = storage.importAll(JSON.stringify({ foo: 1 }), 'merge')
  assert.strictEqual(r.ok, false)
})

test('clearAll 只清 db: 前缀', () => {
  stub.reset()
  storage.set('todos', [])
  wx.setStorageSync('other:key', 'keep')
  storage.clearAll()
  assert.strictEqual(storage.get('todos', null), null)
  assert.strictEqual(stub.getStorageSync('other:key'), 'keep')
})

// ==================== store 工厂 ====================

console.log('\n\x1b[1mstore 工厂\x1b[0m')

test('首次读取返回空数组', () => {
  stub.reset()
  const s = store.createListStore({ key: 'k1' })
  assert.deepStrictEqual(s.getAll(), [])
})

test('add 自动补 id 与 createdAt', () => {
  stub.reset()
  const s = store.createListStore({ key: 'k2' })
  const item = s.add({ title: 'x' })
  assert.strictEqual(typeof item.id, 'string')
  assert.strictEqual(item.id.length > 0, true)
  assert.strictEqual(typeof item.createdAt, 'string')
})

test('add 插入到列表最前', () => {
  stub.reset()
  const s = store.createListStore({ key: 'k3' })
  s.add({ title: 'first' })
  s.add({ title: 'second' })
  assert.strictEqual(s.getAll()[0].title, 'second')
})

test('update 局部更新并落盘', () => {
  stub.reset()
  const s = store.createListStore({ key: 'k4' })
  const item = s.add({ title: 'a', done: false })
  s.update(item.id, { done: true })
  assert.strictEqual(s.getAll()[0].done, true)
  assert.strictEqual(s.getAll()[0].title, 'a')
})

test('update 不存在的 id 返回 null', () => {
  stub.reset()
  const s = store.createListStore({ key: 'k5' })
  assert.strictEqual(s.update('nope', { a: 1 }), null)
})

test('remove 删除指定项', () => {
  stub.reset()
  const s = store.createListStore({ key: 'k6' })
  const a = s.add({ title: 'a' })
  s.add({ title: 'b' })
  s.remove(a.id)
  assert.strictEqual(s.getAll().length, 1)
  assert.strictEqual(s.getAll()[0].title, 'b')
})

test('remove 不存在的 id 返回 false', () => {
  stub.reset()
  const s = store.createListStore({ key: 'k7' })
  assert.strictEqual(s.remove('nope'), false)
})

test('find 按 id 查找', () => {
  stub.reset()
  const s = store.createListStore({ key: 'k8' })
  const a = s.add({ title: 'target' })
  assert.strictEqual(s.find(a.id).title, 'target')
  assert.strictEqual(s.find('nope'), null)
})

test('count 支持谓词', () => {
  stub.reset()
  const s = store.createListStore({ key: 'k9' })
  s.add({ done: true })
  s.add({ done: false })
  assert.strictEqual(s.count(), 2)
  assert.strictEqual(s.count(t => t.done), 1)
})

test('seed 仅在首次读取时写入', () => {
  stub.reset()
  const s = store.createListStore({ key: 'k10', seed: () => [{ id: 's1' }] })
  assert.strictEqual(s.getAll().length, 1)
  s.clear()
  assert.strictEqual(s.getAll().length, 0, '清空后不应再次 seed')
})

test('createObjectStore 合并默认值', () => {
  stub.reset()
  const s = store.createObjectStore({ key: 'o1', defaultValue: { a: 1, b: 2 } })
  assert.deepStrictEqual(s.get(), { a: 1, b: 2 })
})

test('createObjectStore patch 局部合并', () => {
  stub.reset()
  const s = store.createObjectStore({ key: 'o2', defaultValue: { a: 1, b: 2 } })
  s.patch({ a: 9 })
  assert.deepStrictEqual(s.get(), { a: 9, b: 2 })
})

test('createObjectStore reset 恢复默认', () => {
  stub.reset()
  const s = store.createObjectStore({ key: 'o3', defaultValue: { a: 1 } })
  s.patch({ a: 9 })
  s.reset()
  assert.deepStrictEqual(s.get(), { a: 1 })
})

// ==================== 主题 ====================

console.log('\n\x1b[1m主题解析\x1b[0m')

test('light 模式解析为浅色', () => {
  const d = theme.build({ theme: 'light', primaryColor: 'sky' })
  assert.strictEqual(d.themeClass, 'theme-light')
  assert.strictEqual(d.isDark, false)
})

test('dark 模式解析为深色', () => {
  const d = theme.build({ theme: 'dark', primaryColor: 'sky' })
  assert.strictEqual(d.themeClass, 'theme-dark')
  assert.strictEqual(d.isDark, true)
})

test('auto 模式跟随系统（桩为 light）', () => {
  const d = theme.build({ theme: 'auto', primaryColor: 'sky' })
  assert.strictEqual(d.isDark, false)
})

test('主色随主题取不同色值', () => {
  const light = theme.build({ theme: 'light', primaryColor: 'sky' })
  const dark = theme.build({ theme: 'dark', primaryColor: 'sky' })
  // light 一律取深一档的色 —— 配白字要过 WCAG AA（4.5:1），
  // #0369A1 只有 4.1:1，够不到
  assert.strictEqual(light.brand, '#0369A1')
  assert.strictEqual(dark.brand, '#38BDF8')
})

test('未知主色回退到第一个', () => {
  const d = theme.build({ theme: 'light', primaryColor: 'unknown' })
  assert.strictEqual(d.brand, '#0369A1')
})

test('brandSoft 为 rgba', () => {
  const d = theme.build({ theme: 'light', primaryColor: 'sky' })
  assert.strictEqual(/^rgba\(/.test(d.brandSoft), true)
})

test('导航栏前景色随主题切换', () => {
  assert.strictEqual(theme.build({ theme: 'light' }).navFront, '#0B1220')
  assert.strictEqual(theme.build({ theme: 'dark' }).navFront, '#E8EDF5')
})

// ==================== 常量 ====================

console.log('\n\x1b[1m常量映射\x1b[0m')

test('技术分类命中', () => {
  assert.strictEqual(constants.techCategory('frontend').label, '前端')
})

test('未知分类回退到其它', () => {
  assert.strictEqual(constants.techCategory('nope').key, 'other')
})

test('优先级命中', () => {
  assert.strictEqual(constants.priority('high').color, '#DC2626')
})

test('心情等级越界被夹取', () => {
  assert.strictEqual(constants.moodLevel(0).level, 1)
  assert.strictEqual(constants.moodLevel(99).level, 5)
})

test('主色表含 5 项', () => {
  assert.strictEqual(constants.PRIMARY_COLORS.length, 5)
})

test('每个主色都有浅深两版', () => {
  constants.PRIMARY_COLORS.forEach(c => {
    assert.strictEqual(/^#[0-9A-Fa-f]{6}$/.test(c.light), true, c.key + ' light')
    assert.strictEqual(/^#[0-9A-Fa-f]{6}$/.test(c.dark), true, c.key + ' dark')
  })
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
