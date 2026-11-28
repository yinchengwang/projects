# apps-web-walkthrough 设计说明

## 与 preview.html 的分工

两个预览壳，定位不同：

| | preview.html | walkthrough.html |
|---|---|---|
| 给谁看 | 开发���（要逐页查） | 用的人（想知道怎么用） |
| 左边是什么 | 47 个页面的平铺目录树 | 19 步真实使用路径 |
| 右边 | 真应用 iframe | 真应用 iframe |
| 会点吗 | 只切页 | **会真的点** |
| 点完告诉你什么 | 无 | 「点了会怎样」一句话 |

保留两个，不合并：目录树是排错用的，路径是演示用的。

## 关键约束：同源

要「真的点」真应用里的元素，就必须能拿到它的 DOM —— 跨源做不到。

所以导览页**由应用服务端提供**（`/walkthrough.html`，跟 `preview.html` 同一个目录），
iframe 与它同源。

`file://` 打开时（双击 HTML 文件）跨源，会跳到服务端版本：

```js
if (location.protocol === "file:") {
  location.replace(ORIGIN + "/walkthrough.html")
  return
}
```

## 步骤表就是数据

```js
{ url: "/workbench/pages/dashboard/index",
  pick: ".wb-check",        // 要点的元素
  label: "点待办前面的圆圈", // 给人看的
  say: "勾掉一条待办 —— 统计数字会跟着变，这行会变灰。" }  // 点完的效果
```

`peek: true` 表示「只看，不点」—— 用来框出页面里值得看的那块，
不产生副作用。需要真点的（勾待办、记心情）不写 `peek`。

## 高亮框怎么画

在真应用的 `document.body` 里插一个绝对定位的 div：

```js
box.style.position = "absolute"
box.style.zIndex = "99999"
box.style.pointerEvents = "none"       // 不挡真实点击
box.style.boxShadow = "0 0 0 9999px rgba(...)"  // 压暗四周
```

位置每帧跟一次 —— 列表重绘后元素会动，框不跟就飘了。

**踩过的坑**：`requestAnimationFrame` 从 iframe 的 window 上摘下来直接调会丢 `this`：

```js
// 错的 —— Chrome 报 "parameter 1 is not of type 'Function'"
(frame.contentWindow.requestAnimationFrame || setTimeout)(follow, 50)

// 对的
var raf = win.requestAnimationFrame.bind(win)
```

这个异常更阴险：它在 `highlight()` 里抛出，**把后面的真点击一起带没了** ——
表现是「高亮框出现了，但点了没反应」。异常把 `fn(doc)` 整个打断，
`clickForReal()` 根本没机会执行。

现在每一步的处理都包了 try/catch，一步出错不连累后面的步骤。

## 为什么能发现那两个真 bug

导览会**真的去点**，点完看浏览器控制台有没有异常 —— 异常直接暴露了
`children.indexOf is not a function`。

这个 bug 之所以在测试里是绿的：`tests/dom-stub.js` 的 `children` 是数组，
有 `indexOf` 和 `splice`；浏览器里是 `HTMLCollection`，两样都没有。

**测试替身比真东西宽松，就会藏 bug。** 所以新加的 `platform-dom.test.js`
专门用一个 `children` 只读的桩，把浏览器的约束搬进测试。

## 怎么验证导览本身是对的

写了 `build/design-review/drive.js`（临时工具，不入库）：
用 Chrome DevTools Protocol 驱动一个 headless Chrome，
逐步点导览的步骤，检查：

1. 每一步后 iframe 的 `location.pathname` 是不是预期的那个
2. 高亮框在不在（`[data-walkthrough]` 的数量）
3. **真应用的 DOM 有没有变** —— 「点了有反应」不是「点了没报错」

结果：19 步全过，0 步 iframe 打不开，0 个控制台异常。

## 已知限制

- 步骤 2（点「程序员工作台」）点完就跳走了，高亮框跟着旧页面一起没了 ——
  这是对的，导航之后高亮没有意义
- 导览不覆盖全部 47 个页面，只走主路径
- 「添加待办」那一步会真的弹出输入框 —— 需要手动取消（演示效果）
