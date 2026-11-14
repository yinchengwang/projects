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

test("粗体与斜体", function () {
  assert.equal(md.toHtml("这是**重点**"), "<p>这是<strong>重点</strong></p>")
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

test("引用块递归渲染", function () {
  assert.equal(md.toHtml("> 引用一句"), "<blockquote><p>引用一句</p></blockquote>")
})

test("HTML 注入被转义", function () {
  const got = md.toHtml("<script>alert(1)</script>")
  assert.ok(got.indexOf("<script>") === -1, got)
  assert.ok(got.indexOf("&lt;script&gt;") !== -1, got)
})

test("未知语法原样输出为文本，不丢内容", function () {
  const got = md.toHtml(":::not-a-syntax:::")
  assert.ok(got.indexOf(":::not-a-syntax:::") !== -1, got)
})

test("escapeHtml 转义五个字符", function () {
  assert.equal(md.escapeHtml("<a href=\"x\">&'</a>"), "&lt;a href=&quot;x&quot;&gt;&amp;&#39;&lt;/a&gt;")
})
