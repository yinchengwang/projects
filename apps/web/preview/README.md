# 前端页面预览

把合并应用（`apps/web`）的**每一个页面**放进一个可点击的预览壳里 —— 左边是完整目录树，
右边是**真实运行的应用**，点哪页就跳哪页。

不是截图，是活的：数据是真的（`ai-timer/` 里的真实简报、seed 数据），
交互是真的（按钮能点、Canvas 真的在画、游戏真的能玩）。

## 打开

```bash
node apps/web/preview/open.js
```

会启动应用服务（没启动才启动）并用默认浏览器打开 `http://127.0.0.1:5173/preview.html`。

服务已经在跑的话，直接开 <http://127.0.0.1:5173/preview.html> 也行 ——
应用首页右上角也有「页面预览」入口。

## 用法

| 操作 | 说明 |
|---|---|
| 点左侧目录 | 右侧 iframe 跳到那一页 |
| 地址栏 | 显示当前页路径 |
| 自适应 / 平板 820 / 手机 414 | 切换预览视口宽度 |
| ↻ 刷新 | 重新加载当前页 |
| ↗ 新标签 | 在新标签页打开当前页（脱离预览壳） |
| 地址栏 `#` 后面的路径 | 深链，可直接分享，比如 `#/knowledge/pages/learn/deep/index` |

浏览器前进后退也能用。

## 结构

```
apps/web/
├── web/preview.html            预览壳（左侧目录树 + 右侧 iframe）
├── web/preview-manifest.js     页面清单的静态副本 —— 预览壳读它
├── server/preview-manifest.js  页面清单的唯一数据源
└── preview/
    ├── open.js                 一键启动 + 打开浏览器
    └── sync.js                 把源文件同步成静态副本
```

**清单为什么要同步成静态文件**：预览页可能被 `file://` 直接打开（双击打开），
那时 `fetch` 会被 CORS 拦掉（Origin 是 `null`），而 `<script src>` 不会。

**清单怎么保证不漏**：改了 `server/preview-manifest.js` 后跑 `node apps/web/preview/sync.js`。
`tests/preview-manifest.test.js` 会守住两道 ——

1. 静态副本与源文件一致（同步没漏跑）
2. 清单覆盖了各模块 `browser.js` 里 `PAGES` 映射的**全部**页面
   —— 新加了页面却忘了登记进预览，测试会挂

## 详情页的 id

笔记 / 代码片段 / 面试题 / 足迹的详情页需要真实 id，
而 seed 数据的 id 是 `newId()` 随机生成的，没法预先猜。

预览壳启动时会把对应列表页在隐藏 iframe 里过一遍 —— 那会触发 store 读，
store 读不到就落 seed 数据。于是 localStorage 里就有 id 了，
再据此拼出详情页的 URL。解析不到就回落到对应的列表页。

## 已知问题

**技术笔记详情、代码片段详情、面试题库详情** 这三个页面的 `render()`
是从「健身」页复制来的 —— 读永远为空的 `state.items`，空态文案写成「暂无健身」。
来自 2026-11 的 web 迁移（commit `7e4cd4239`），是应用本身的既有缺陷，
预览壳不掩盖它。**要我修就说一声。**

## 依赖

- Node ≥ 24
- 零 npm 依赖
