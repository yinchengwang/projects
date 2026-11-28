# 前端页面预览

把合并应用（`apps/web`，5173）的**每一个页面**实拍成截图，生成一份可点击的 HTML 画廊。

```bash
node apps/web/preview/generate.js
# 打开 apps/web/preview/out/index.html
```

## 产物

```
apps/web/preview/out/
├── index.html        # 画廊：按模块分组，缩略图点开看大图
└── shots/*.png       # 47 张页面截图（1280×900）
```

## 怎么做到「实拍」

1. 起一个真实服务进程（`apps/web/server/main.js`，端口 5199），所以画面里是**真数据**
   ——`ai-timer/` 里的真实简报、seed 数据、Canvas 图谱、游戏棋盘；
2. 用 headless Chrome 逐页 `--screenshot`，页面里的 JS 会真跑一遍，
   Canvas 页面（知识图谱、技术雷达、四个游戏）也是真绘制出来的；
3. 所有页面共用同一个 Chrome profile，localStorage 在页间共享。

## detail 页的 id 怎么来的

笔记 / 代码片段 / 面试题 / 足迹的详情页需要真实 id，
而 seed 数据的 id 是 `newId()` 随机生成的，没法预先猜。

做法：先逛一遍对应列表页（它们会把 seed 写进 localStorage），
再用一个探针页把 localStorage 里的 id 导出，据此拼详情页 URL。

## 已知问题标注

预览页上带「⚠ 已知问题」角标的页面，是**应用本身的既有缺陷**，不是预览工具的问题：

| 页面 | 问题 |
|---|---|
| 技术笔记 · 详情<br>代码片段 · 详情<br>面试题库 · 详情 | `render()` 是从「健身」页复制来的 —— 读永远为空的 `state.items`，空态文案写成「暂无健身」。来自 2026-11 的 web 迁移（commit `7e4cd4239`），不在预览工具的修改范围内。 |
| 足迹 · 记录详情 | 足迹数据由用户自己创建、没有示例数据，所以详情页显示「记录不存在或已删除」—— 这是正确行为。 |

## 依赖

- Chrome 或 Edge（自动探测常见安装路径）
- Node ≥ 24

零 npm 依赖。
