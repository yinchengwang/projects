# apps-web-design-system：合并应用的界面重做

**变更名称：** 合并应用设计系统与界面重做
**创建日期：** 2026-11-28
**状态：** 进行中
**关联模块：** `apps/web/web/`、`apps/web/shared/core/`

---

## 背景

合并应用（`apps/web`，47 个页面）能用，但**界面难看**。用量化体检定位问题，
不是凭感觉改：

| 症状 | 实测 | 人眼感受 |
|---|---|---|
| 整页没色彩 | 平均饱和度 **1.5%**；页面底/卡面/凹槽三色占 95%+ | 一片灰 |
| 卡片糊在背景里 | 卡面 `#FFFFFF` 对页面底 `#F7F8FA` 亮度差 **2%**；边框 `0.5px` | 卡片没立起来，页面是平的 |
| 辅助文字看不清 | `--text-muted` 对比度 **2.31:1**（AA 要 4.5） | 大片灰字糊成一片 |
| 字号偏小 | 11px×3 / 12px×5 / 13px×6，正文 14px | 长时间阅读费劲 |
| 没有交互反馈 | `transition` 1 次、`:focus` **0 次**、`box-shadow` 2 次 | 死板、键盘不可达 |

另外两个功能级缺陷：

- **暗色模式半生不熟** —— 主题类挂在 `#app-main` 上，页头在它外面，
  暗色只染黑了正文区，页头还是亮的
- **`statGrid` 的 `tone` 是死类** —— `ui.js` 会发 `tone-accent/ok/warn/bad`，
  但没有任何 CSS 接住，选中态和普通态长得一样

## What Changes

### 共用层（`apps/web/web/style.css`）

| 改动 | 说明 |
|---|---|
| 令牌体系 | 间距 `--sp-1..8`、圆角 `--r-sm..full`、阴影 `--shadow-1..3`、动效 `--ease/--dur` 三套刻度，不临时造值 |
| 页面底与卡面拉开 | 页面底 `#EEF2F7` 比卡面 `#FFFFFF` 深，卡片才立得起来 |
| 边框 1px | 替掉全部 `0.5px` —— 2x 屏上 0.5px 只有半个物理像素，等于没画 |
| 正文 15px | 14px 在中文正文里偏小 |
| 对比度修正 | `--text-muted` 提到 **4.84:1**（浅色）/ **6.01:1**（暗色），双主题过 AA |
| 品牌色 | `#0284C7` → `#0369A1`（配白字从 4.1:1 提到 5.93:1）；暗色用 `#38BDF8` |
| 焦点可见 | 全局 `:focus-visible` 焦点环 |
| 表单基线 | 没给 class 的 `input/textarea/select` 也长成同一副样子 |
| 交互反馈 | 按钮、卡片、构件统一挂 `transition` |
| 色彩锚点 | 模块入口卡片顶部彩条（五个模块各一色），整页唯一但一眼分得出 |
| `tone-*` 样式 | 补上 `stat.tone-accent/ok/warn/bad` |
| 减弱动效 | `prefers-reduced-motion` 尊重系统设置 |

### 主题机制（`apps/web/shared/core/`）

| 文件 | 改动 |
|---|---|
| `with-theme.js` | 主题类改挂 `document.body`（原来挂 `#app-main`，导致页头不跟随） |
| `theme.js` | `navFront/navBg` 跟新令牌对齐 |
| `constants.js` | 主色深一档（配白字过 AA）；分类色、优先级色、心情色统一到新色板 |

### 模块层（`apps/web/web/*/style.css`）

- 全部 `0.5px` → `1px`（aitimer 2 处 / games 4 / knowledge 8 / trip 2 / workbench 14）
- 各自补上交互过渡

### 语义色落地

- 优先级药丸按 `data-priority` 上色（高红 / 中琥珀 / 低绿）
- 心情按钮按 `data-level` 上色（1 红 → 5 绿），并渲染 `MOOD_LEVELS` 里本来就有、
  从来没人用过的 emoji

### 测试

| 文件 | 说明 |
|---|---|
| `apps/web/tests/design-system.test.js`（新增） | 28 条断言，把可测的设计约束钉死：对比度、边框、字号、焦点、构件齐全 |
| `apps/web/tests/wb-core.test.js` | 主色断言同步新色板；HEX 解析用例改用与调色板无关的颜色 |
| `apps/web/tests/wb-pages.test.js` | 靛紫主色断言同步 |

## 不做

- 不重写任何页面逻辑 —— 只动样式与主题挂载点
- 不引入 CSS 框架 —— 保持零 npm 依赖
- 不做响应式断点 —— `style.css` 里一个 `@media` 都没有，这是既有的技术选择，
  这次不扩大范围（只加了 `prefers-reduced-motion` 与 `print`）
- 不动那 3 个坏掉的详情页（notes / snippets / interview）—— 独立问题，另开变更
