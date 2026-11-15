"use strict"
/**
 * unit-markdown.test.js —— core/unit/markdown.js
 *
 * 零依赖铁律：不引 Markdown 库，自己写一个极简转换器。
 * 契约 §8.1 要求处理标题、段落、列表、粗体、行内代码、代码块、表格、引用；
 * 未知语法**原样输出为文本**（不报错、不丢内容）。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const md = require("../core/unit/markdown.js")

test("空输入返回空串", function () {
  assert.equal(md.toHtml(""), "")
  assert.equal(md.toHtml(null), "")
  assert.equal(md.toHtml(undefined), "")
})

test("标题转成 h1~h6", function () {
  assert.equal(md.toHtml("# 一级"), "<h1>一级</h1>")
  assert.equal(md.toHtml("### 三级"), "<h3>三级</h3>")
  assert.equal(md.toHtml("###### 六级"), "<h6>六级</h6>")
})

test("段落原样成 p，行内换行变 br", function () {
  assert.equal(md.toHtml("普通一段"), "<p>普通一段</p>")
  assert.equal(md.toHtml("第一行\n第二行"), "<p>第一行<br>第二行</p>")
})

test("无序列表与有序列表", function () {
  assert.equal(md.toHtml("- 甲\n- 乙"), "<ul><li>甲</li><li>乙</li></ul>")
  assert.equal(md.toHtml("1. 甲\n2. 乙"), "<ol><li>甲</li><li>乙</li></ol>")
  assert.equal(md.toHtml("* 甲\n* 乙"), "<ul><li>甲</li><li>乙</li></ul>")
})

test("粗体转成 strong", function () {
  assert.equal(md.toHtml("这是**重点**"), "<p>这是<strong>重点</strong></p>")
})

test("斜体转成 em", function () {
  // 契约 §8.1 没要求斜体，是实现自己多做的；但这一支以前**零覆盖**，
  // 把 <em> 分支打残（变异体 E6）测试依然全绿。测试名以前写「粗体与斜体」
  // 却只断言粗体——名字与断言对不上，后来人会被误导。
  assert.equal(md.toHtml("这是*斜体*"), "<p>这是<em>斜体</em></p>")
  assert.equal(md.toHtml("*只有斜体*"), "<p><em>只有斜体</em></p>")
})

test("行内代码用 code 包起来", function () {
  assert.equal(md.toHtml("调用 `foo()` 就行"), "<p>调用 <code>foo()</code> 就行</p>")
})

test("代码块整体转义，不解析里面的 markdown", function () {
  const got = md.toHtml("```cpp\nint a = 1 < 2;\n```")
  assert.equal(got, "<pre><code>int a = 1 &lt; 2;</code></pre>")
})

test("表格转成 table", function () {
  const got = md.toHtml("| a | b |\n|---|---|\n| 1 | 2 |")
  assert.equal(
    got,
    "<table><thead><tr><th>a</th><th>b</th></tr></thead>" +
      "<tbody><tr><td>1</td><td>2</td></tr></tbody></table>"
  )
})

test("单元格里的 \\| 渲染成字面量竖线", function () {
  // 题面里出现 `\|` 的表格以前渲染错（变异体 E11 存活，零覆盖）。
  // 期望值是跑出来的实际输出：竖线进单元格内部，不当分隔符。
  const got = md.toHtml("| a | b |\n|---|---|\n| x \\| y | 2 |")
  assert.equal(
    got,
    "<table><thead><tr><th>a</th><th>b</th></tr></thead>" +
      "<tbody><tr><td>x | y</td><td>2</td></tr></tbody></table>"
  )
})

test("数据行列数少于表头时输出畸形表格（断言实现的实际形态）", function () {
  // 这条断言的是「实现现在是什么样」，不是「实现应该是什么样」——
  // 本轮只补测试，不改实现。数据行的 <td> 数比表头的 <th> 少，表格是畸形的。
  const got = md.toHtml("| a | b | c |\n|---|---|---|\n| 1 |")
  assert.equal(
    got,
    "<table><thead><tr><th>a</th><th>b</th><th>c</th></tr></thead>" +
      "<tbody><tr><td>1</td></tr></tbody></table>"
  )
})

test("引用块递归渲染", function () {
  assert.equal(md.toHtml("> 引用一句"), "<blockquote><p>引用一句</p></blockquote>")
})

test("`>` 后必须带空白，否则是普通段落不是引用块", function () {
  // GFM 要求引用块的 `>` 后必须有空格或 tab。题面里 `>5 秒`、`>100ms` 这类写法很常见，
  // 以前会被误渲染成引用块（变异体 E12）。
  assert.equal(md.toHtml("> 引用一句"), "<blockquote><p>引用一句</p></blockquote>")
  assert.equal(md.toHtml(">5 秒"), "<p>&gt;5 秒</p>")
  assert.equal(md.toHtml(">100ms 完成"), "<p>&gt;100ms 完成</p>")
})

test("HTML 注入被转义", function () {
  const got = md.toHtml("<script>alert(1)</script>")
  // 精确匹配：以前用 indexOf 断全串，被包进别的标签也发现不了。
  assert.equal(got, "<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>")
})

test("未知语法原样输出为文本，不丢内容", function () {
  const got = md.toHtml(":::not-a-syntax:::")
  // 精确匹配：这条以前最弱——只要输出里**含**这串字就绿，
  // 被包进别的标签（比如 <code>）也照样绿。
  assert.equal(got, "<p>:::not-a-syntax:::</p>")
})

test("escapeHtml 转义五个字符", function () {
  assert.equal(md.escapeHtml("<a href=\"x\">&'</a>"), "&lt;a href=&quot;x&quot;&gt;&amp;&#39;&lt;/a&gt;")
})
