# apps/ 合并为单一 Web 应用 —— 设计文档

- 日期：2026-11-26
- 状态：待评审
- 范围：`apps/` 目录下 6 个应用，不涉及 `engine/`（C/C++ 工程轨）

---

## 1. 背景与目标

`apps/` 下现有 6 个应用，技术栈与运行形态各不相同：

| 应用 | 形态 | 技术栈 | 规模 | 依赖 |
|---|---|---|---|---|
| `games` | 微信小程序 | 原生 JS + WXML/WXSS | 5 页 / 37 文件 / 144 KB | 零 |
| `knowledge-hub` | 微信小程序 | 同上 | 17 页 / 132 文件 / 324 KB | 零 |
| `programmer-workbench` | 微信小程序 | 同上 | 18 页 / 131 文件 / 388 KB | 零 |
| `trip-log` | 微信小程序 | 同上 + 腾讯云 SDK | 3 页 / 25 文件 / 199 KB | npm `@tencent-ai/workbuddy-cloud-sdk` |
| `study-hub` | 本机 Web | Node 24 + `node:sqlite` | 19 个 web 文件 + 14 个 server 文件 | 零 npm |
| `wiki-rag` | 本机 Web | Python FastAPI | 20 py + 3 web 文件 | pip: lancedb / torch / sentence-transformers |

**目标**：合并为一个本机 Web 应用 —— 单一入口、单一端口、单一导航，用户只面对一个应用。

**非目标**：
- 不修改 `engine/` 任何内容
- 不引入前端框架，不引入 npm 依赖（保持零构建）
- 不改动 `study-hub` 与 `wiki-rag` 的内部逻辑与测试语义

---

## 2. 现状核实结论

以下均为实测结论，不是推测。

### 2.1 `wx.*` 依赖面极薄

4 个小程序全部 `wx.*` 调用扫描结果：

| 应用 | wx API | 移植难度 |
|---|---|---|
| `games` | `getStorageSync` `setStorageSync` `removeStorageSync` `vibrateShort` | 易 |
| `knowledge-hub` | 上述 + `getStorageInfoSync` `getSystemInfoSync` `onThemeChange` `setNavigationBarColor` `setTabBarStyle` | 易 |
| `programmer-workbench` | 同 `knowledge-hub` | 易 |
| `trip-log` | `getStorageSync` `setStorageSync` `login` + 腾讯云 `database()` / `storage()` | **有阻碍** |

**核心结论**：`core/` 层 **零 `wx.` 依赖**（`knowledge-hub/core/` 17 文件、`programmer-workbench/core/` 13 文件、`games/utils/` 全扫描均为 0 处）。`wx` 只出现在两个地方：
1. `core/storage.js` —— 存储封装（**唯一的数据出入口**）
2. `core/theme.js` —— 主题/导航栏配色（Web 侧由 CSS 承担）

### 2.2 共享层已经分叉

`knowledge-hub/core/` 与 `programmer-workbench/core/` 按文件名 MD5 比对：

- **10 个文件逐字节相同**：`constants.js` `date.js` `format.js` `id.js` `md.js` `md2html.js` `storage.js` `store.js` `theme.js` `with-theme.js`
- **2 个文件已分叉**：`seed.js` `stores.js`
- **5 个 hub 独有**：`kh-constants.js` `graph-layout.js` `radar-layout.js` `review.js` `seed-data.js`

`components/` 比对：32 个文件中 **23 个已不同**，仅 9 个相同。

> `apps/README.md` 第 88 行声称"共享同一套 `core/` 与 `components/`（逐字节相同）"—— **该描述已过期**，实际早已分叉。合并的收益主要来自消除这份分叉。

### 2.3 视图层规模可控

35 个页面（17 + 18）的 `.js` 视图文件均为 2–12 KB；`setData` 调用共 129 处（knowledge-hub 11 / programmer-workbench 80 / games 38）。**不存在**超大视图文件。

### 2.4 现有测试基础设施可复用

小程序已有一套 Node 下运行的测试（16 个文件，约 120 KB），关键在 `tests/wx-stub.js`：它把 `wx` API 换成一个内存版 storage（`getStorageSync` / `setStorageSync` / `removeStorageSync` / `getStorageInfoSync`），并用 `Proxy` 让未知 API 自动 no-op。

**这意味着移植后现有测试几乎可以原样保留** —— 只需把 stub 的后端从内存 Map 换成 `localStorage` 形状的 stub。

### 2.5 路径假设（决定了目录布局）

| 位置 | 假设 | 影响 |
|---|---|---|
| `wiki-rag/server/config.py:8` | `PROJECTS_ROOT = APP_ROOT.parent.parent` → 仓库根 | 嵌套一层即断（语料 `docs/wiki/projects`、门禁 `scripts/wiki` 找不到） |
| `study-hub/tools/import-catalog.js:29` | `path.resolve(__dirname, "..", "..", "..", "scripts")` → 仓库根 | 嵌套一层即断 |
| `study-hub/server/app.js:14` | `ROOT = path.resolve(__dirname, "..")` | 自包含，任意深度均可 |
| `study-hub/server/main.js:24` | `.env` 读 `__dirname/../.env` | 自包含 |
| `wiki-rag/server/app.py:65` | 静态目录 `Path(__file__).parent.parent / "web"` | 自包含 |

**结论**：`study-hub` 与 `wiki-rag` 保持 `apps/` 直属，不下沉一层。

---

## 3. 目标架构

### 3.1 目录布局（`apps/` 直属）

```
apps/
├── README.md                 更新：描述合并后的单一 Web 应用
├── web/                      ★ 新增：组合根 + 共享层 + 各模块前端
│   ├── server/
│   │   ├── main.js           入口（监听 5173）
│   │   ├── app.js            组合根：挂载各模块
│   │   ├── proxy.js          → wiki-rag FastAPI 反代
│   │   └── modules/
│   │       └── study.js      复用 study-hub 的 createApp
│   ├── shared/               ★ 从 4 个小程序 core/ 抽出的共用层
│   │   ├── core/             10 个公共模块（单一副本）
│   │   │   ├── constants.js  date.js  format.js  id.js
│   │   │   ├── md.js  md2html.js  storage.js  store.js
│   │   │   └── theme.js  with-theme.js
│   │   ├── knowledge/        hub 独有：graph-layout radar-layout review kh-constants seed-data
│   │   ├── workbench/        workbench 独有：codetools
│   │   ├── games/            games/utils/*（snake sudoku game2048 match3 achievements share vibrate）
│   │   ├── ui.js             统一构件层（吸收 study-hub/web/ui.js）
│   │   ├── charts.js         手绘 SVG 图表层（吸收 study-hub/web/charts.js）
│   │   ├── platform.js       平台适配层：showToast/showModal/showActionSheet/导航等 9 个 API
│   │   └── storage-adapter.js  浏览器端 localStorage 适配
│   └── web/                  各模块前端
│       ├── index.html        统一外壳（导航 + 路由）
│       ├── style.css         全局样式（吸收小程序 WXSS）
│       ├── games/
│       ├── knowledge/
│       ├── workbench/
│       └── trip/
├── study-hub/                原地保留（路径零风险）
└── wiki-rag/                 原地保留（路径零风险）
```

**被删除的目录**（合并后无副本价值）：
`apps/games/` `apps/knowledge-hub/` `apps/programmer-workbench/` `apps/trip-log/`

### 3.2 路由约定（单一服务，单一端口）

| 路径 | 归属 | 实现 |
|---|---|---|
| `/` | 外壳 | 统一导航 + 模块入口 |
| `/study/*` | study-hub | 复用 `study-hub/server/app.js` 的 `createApp({routes, webRoot})` |
| `/wiki/*` | wiki-rag | `proxy.js` 转发到 `127.0.0.1:8000` |
| `/games/*` `/knowledge/*` `/workbench/*` `/trip/*` | 前端模块 | 静态资源 + 浏览器端逻辑 |
| `/shared/*` | 共用层 | 静态资源 |

---

## 4. 关键设计决策

### 决策 A：存储抽象 —— `core/storage.js` 只换底层

小程序的存储语义与 Web 的 `localStorage` **同构**：都是"字符串键 → 字符串值"，且都已用 `db:` 前缀隔离命名空间。

做法：`core/storage.js` 提为 `shared/core/storage.js`，底层从 `wx.*` 换成 `localStorage`，**上层 30+ 个调用方零改动**。

两个必须处理的语义差异：

| 行为 | `wx.getStorageSync` | `localStorage.getItem` | 处理 |
|---|---|---|---|
| 键不存在 | 返回 `''`（空串） | 返回 `null` | 适配层统一返回 `null` |
| 存对象 | 需手动 `JSON.stringify` | 只能存字符串 | 保持 `JSON.stringify` / `JSON.parse` 一致 |

`shared/core/storage.js` 的对外 API 不变（`get` / `set` / `remove` / `keys` / `clearPrefix`），因此 `stores.js` `store.js` `seed.js` 等**全部无需修改**。

### 决策 B：页面重写 —— 复用 `study-hub/web/ui.js` 构件模式

**不写 WXML→HTML 转译器**（会引入长期维护负担，且 WXSS 的 `rpx` 单位本就要转）。

目标形态已存在于 `study-hub/web/ui.js`：零依赖 UMD 模块，浏览器挂 `window.AlgoPlanUI`，Node 可 `require` 取纯构造器。构件集：`el` / `pageHead` / `hero` / `panel` / `block` / `statGrid` / `pill` / `kv` / `empty`。

渲染机制：
- 页面模块导出 `data`（初始状态）与 `render(state) → HTMLElement`（纯函数）
- `shared/ui.js` 提供 `mount(root, page)` 统一挂载 + 事件委托
- 状态更新 = 重新 `render` + 局部替换

`knowledge-hub` 的 `knowledge-graph`（Canvas）与 `radar-chart` 组件，以及 `study-hub/web/charts.js`（33.7 KB 手绘 SVG 纯函数层）—— 统一归入 `shared/charts.js` 与 `shared/knowledge/`，两个模块共用。

**样式转换规则**：WXSS 共 250 处 `rpx` 引用，各 `app.wxss` 头部已写明换算基准「**1px = 2rpx（按 375 设计稿）**」。因此 `Nrpx → N/2 px` 是机械换算，无歧义。`0.5px` 细边框在 Web 下保留（浏览器支持）。小程序的 `page` 全屏容器 → Web 的统一外壳容器（`.app-shell`），页面内容区自适应宽度。

### 决策 C：共享层抽取（消除已发生的分叉）

| 来源 | 目标位置 | 处置 |
|---|---|---|
| 10 个逐字节相同文件 | `shared/core/` | **单一副本**，两端共用 |
| `seed.js` `stores.js`（已分叉） | `shared/core/` | 逐字段 diff 后合并为一份；seed 数据取两边并集 |
| hub 独有 5 个 | `shared/knowledge/` | 迁入 |
| workbench 独有 `codetools.js` | `shared/workbench/` | 迁入 |
| `games/utils/*` | `shared/games/` | 迁入 8 个文件（`achievements` `game2048` `match3` `share` `snake` `sudoku` `storage` `vibrate`），其中 `storage.js` 替换为共享版 |

`shared/core/` 的公共文件同时提供两套入口：
- 浏览器：`window.AlgoPlanShared`（UMD，与 `ui.js` 同风格）
- Node：`require(...)`（供测试使用）

### 决策 D：trip-log 的图片 —— IndexedDB

`trip-log` 原有约束：每条记录最多 9 张图，单图上限 5 MB，即单条记录最多 45 MB —— **远超 localStorage 的 ~5 MB 配额**。

做法：
- **照片**以 `Blob` 存入 IndexedDB（store 名 `photos`，key = `<tripId>:<photoId>`）
- **元数据**（title / date / weather / location / companions / tags / notes / photos 索引）存 `localStorage`，`db:` 前缀
- Web 端选图：`<input type="file" accept="image/*" multiple>`
- 预览：`URL.createObjectURL(blob)`，页面卸载时 `revokeObjectURL`

**放弃的能力**：微信 openid 身份 + 腾讯云 database/storage 同步（`wx.login` 与云 SDK 无法在浏览器等价实现）。
**补偿**：提供 JSON 导出/导入（含照片 base64），作为数据出逃口。

> 备注：`trip-log` 当前云配置为 `PLACEHOLDER_ENDPOINT`，即云端功能本就未激活，迁移不会损失任何已可用的功能。

### 决策 E：wiki-rag 保持 Python，Node 反代

`wiki-rag` 依赖 lancedb + torch + sentence-transformers，Node 侧无等价实现。

做法：
- `apps/web/server/proxy.js` 把 `/wiki/api/*` 转发到 `127.0.0.1:8000`，`/wiki/*` 其余路径转发静态页
- Node 启动时探测 Python 服务存活；不存活时 `/wiki/*` 返回 **HTTP 503 + 中文提示**：「wiki-rag 未启动，请先运行 `python -m server.main`（需在 `apps/wiki-rag` 下）」
- **wiki-rag 不可用只影响该模块**，其余模块完全正常

### 决策 F：平台适配层 —— `shared/platform.js`

小程序的交互层共 193 处 wx UI 调用，但**只用了 9 个 API**：

| wx API | 用途 | Web 侧实现 |
|---|---|---|
| `showToast` | 轻提示（`icon: none` / `success`） | 自研 toast 组件 |
| `showModal` | 确认框；`editable: true` 时兼作输入框 | `<dialog>` + 内嵌 input |
| `showActionSheet` | 动作菜单 | `<dialog>` + 列表项 |
| `navigateTo` / `navigateBack` / `switchTab` | 页面跳转 | History API（`pushState` / `back` / 替换） |
| `showLoading` / `hideLoading` | 加载遮罩 | 自研 overlay |
| `chooseMedia` | 选图 | `<input type="file" accept="image/*" multiple>` |
| `previewImage` | 图片预览 | 自研 lightbox |
| `stopPullDownRefresh` | 下拉刷新结束 | 不需要（Web 无此手势） |

做法：在 `shared/platform.js` 提供**与 wx 同名同签名**的平台 API（`showToast({title, icon})` 等），页面代码的调用方式**一字不改**，只需把 `wx.` 前缀换成 `platform.`。

收益：
- 交互层只实现**一次**，而非 35 个页面各写一遍
- `wx.showModal({editable:true})` 的"快捷输入"语义（`capture-sheet` 组件依赖它）在 `<dialog>` 上有精确对应
- 页面层移植时，只需关心「数据 → 视图模型」这一件事

页面文件实际是两层结构，天然支持拆分交付（见 §7）：
- **数据层**：`refresh()` 中的 `stores.*` 调用、排序、`map` 成视图模型 —— 纯逻辑，零 wx 依赖
- **交互层**：`wx.*` 调用与 `setData` —— 由平台适配层吸收

### 决策 G：旧小程序目录直接删除

合并完成后删除 `apps/games/` `apps/knowledge-hub/` `apps/programmer-workbench/` `apps/trip-log/`。

前置条件（**必须全部满足才允许删除**）：
1. 该模块的 18 个（workbench）/ 17 个（knowledge-hub）/ 5 个（games）/ 3 个（trip）页面全部移植完成并可交互
2. 该模块现有测试全部迁入 `apps/web/tests/` 并通过
3. `apps/web/tests/run-all.js` 一条命令覆盖全部模块

`git` 历史保留原始代码，删除可回溯。

---

## 5. 数据流与错误处理

### 5.1 数据流

**前端模块（游戏 / 知识 / 工作台 / 足迹）**
```
用户操作 → 事件委托 → 更新页面 state → render(state) → 重绘
                                    ↘ storage.get/set('db:<key>') → localStorage
```

**study-hub**
```
浏览器 → /study/api/* → apps/web/server/modules/study.js
                      → study-hub/server/app.js 的 createApp（注入 routes 与 webRoot）
                      → SQLite（study-hub/data/app.db）
```

**wiki-rag**
```
浏览器 → /wiki/api/search → apps/web/server/proxy.js
                       → 127.0.0.1:8000（FastAPI 子进程）
                       → index/data/projects/<库>/
```

### 5.2 错误处理原则

沿用 `study-hub` 既有约定 **G4：面向用户的文案一律中文**。

| 场景 | 处理 |
|---|---|
| 后端接口异常 | JSON `{ok:false, error:"中文说明"}` + 语义化 HTTP 状态码 |
| 前端渲染异常 | 统一错误边界，显示中文提示而非白屏 |
| wiki-rag 未启动 | 该模块 503 + 中文提示，其余模块不受影响 |
| localStorage 写满 | `storage.set` 返回 `false` + 中文 warn（沿用现有行为） |
| IndexedDB 不可用（隐私模式） | 降级为仅元数据（无照片），并在 UI 明示 |

---

## 6. 测试策略

| 层次 | 手段 | 门禁 |
|---|---|---|
| `shared/core/` | 迁移小程序现有 `core.test.js` / `stores.test.js` / `review.test.js` / `graph.test.js` | 全部通过 |
| `shared/platform.js` | 新增单测，覆盖 9 个平台 API（toast / modal / actionSheet / 路由跳转 / loading / 选图 / 预览） | 全通过 |
| 数据层（每模块） | 迁移小程序现有 `tech.test.js` / `life.test.js` / `p5.test.js` / `tools.test.js` 等 | 全通过 |
| 页面层 | 各模块 `tests/*.test.js`（node:test），断言 `render()` 输出含关键元素 | 新增，全通过 |
| 前端存储 | `tests/wx-stub.js` 换 `localStorage` stub 后**原样复用** | 现有用例全通过 |
| 组合根 | `apps/web/tests/run-all.js` 一条命令跑全部模块 | CI 门禁 |
| 回归 | `study-hub` 原有 `tests/run-all.js` **继续独立跑**；`wiki-rag` 的 `pytest` 继续独立跑 | 不许破 |

> `study-hub` 与 `wiki-rag` 原地保留，其原有测试命令与结果不受合并影响。

---

## 7. 交付顺序

每一步结束都能独立运行，不存在半成品中间态。**主力模块优先**；主力模块内部按「数据层 → 视图层」拆成两步，各自可独立验收。

### 7.1 步骤总表

| 步骤 | 内容 | 验证方式 |
|---|---|---|
| 0 | 骨架：`apps/web/` 组合根 + 路由 + 静态托管 + 统一外壳 | `node apps/web/server/main.js` 起得来；4 个路由返回 200 |
| 1 | `shared/core/` 抽取（10 个公共文件）+ `shared/ui.js` + `shared/charts.js` | `shared/core` 单测全绿 |
| 2 | `shared/platform.js` 平台适配层（9 个 API） | 单测覆盖 toast / modal / actionSheet / 路由跳转 |
| 3 | **workbench 数据层**：`codetools.js` + `stores.js` 合并 + `seed.js` 归一 → `shared/workbench/` | 现有 `core.test.js` / `tech.test.js` 迁入并通过 |
| 4 | **workbench 视图层**：18 页 → `apps/web/web/workbench/` | 18 页可交互；工具箱 10 工具可用；`db:` 数据读写一致 |
| 5 | **knowledge-hub 数据层**：`graph-layout` `radar-layout` `review` `kh-constants` `seed-data` → `shared/knowledge/` | 现有 `review.test.js` / `graph.test.js` / `stores.test.js` 迁入并通过 |
| 6 | **knowledge-hub 视图层**：17 页 + 8 组件 → `apps/web/web/knowledge/` | 17 页渲染正确；图表复用 `charts.js` |
| 7 | **trip-log 模块**（3 页 + IndexedDB 图片 + 导出导入） | 增删改查 + 照片存取 + 导出导入往返一致 |
| 8 | **games 模块**（5 页） | 5 个游戏可玩；存档持久化；`engine.test.js` 迁入并通过 |
| 9 | **study-hub 挂载** + **wiki-rag 反代** | `/study` 原有测试全绿；`/wiki` 检索可用；wiki 停掉时其余模块正常 |
| 10 | 删除 4 个旧小程序目录 + 更新 `apps/README.md` | `apps/web/tests/run-all.js` 全绿；旧目录已消失 |

### 7.2 为什么要拆数据层 / 视图层

每个页面文件天然分两层，拆分后**数据层可先被测试覆盖**，视图层的重写就有稳定的回归护栏：

```
apps/programmer-workbench/pages/tech/notes/index.js
├── 数据层（零 wx 依赖）        → 搬进 shared/workbench/，node:test 直接断言
│   ├── stores.techNotes.getAll()
│   ├── 置顶优先 + 更新时间倒序的排序
│   └── map 成视图模型 {id, title, color, categoryLabel, time, preview, tags, pinned}
└── 交互层（需替换）            → 视图层重写时用 platform.* 承接
    ├── wx.navigateTo / wx.navigateBack
    ├── wx.showActionSheet / wx.showModal / wx.showToast
    └── this.setData({items, count, pinnedCount})
```

拆分后的收益：
1. **数据层先绿**：视图层还没写，逻辑已被 `node:test` 覆盖 —— 重写页面时改坏了数据逻辑会立刻被发现
2. **风险前置**：`stores.js` / `seed.js` 的分叉合并（决策 C）是最易出错的部分，拆开后它是独立可验收的一步
3. **每步都有产出**：步骤 3 结束时 `shared/workbench/` 已有可测的完整业务逻辑，步骤 4 只是把它显示出来

### 7.3 前置约束

步骤 10 的删除动作，必须在步骤 3–8 对应模块的测试全部迁入并通过之后执行。

---

## 8. 风险与缓解

| 风险 | 等级 | 缓解 |
|---|---|---|
| 35 个页面重写工作量大 | 高 | 分模块交付，主力模块优先；每步独立验收；`ui.js` 构件让每页代码量可控 |
| `stores.js` / `seed.js` 分叉合并丢数据 | 中 | 逐字段 diff，seed 数据取两边并集，迁入的 `stores.test.js` 作为回归门禁 |
| WXSS `rpx` 单位需转换 | 中 | WXSS 头部已写明换算基准「1px = 2rpx（375 设计稿）」。`rpx → px` 按 ÷2 机械换算；`page` 布局（小程序全屏）→ 普通块级布局 + 统一外壳容器 |
| wiki-rag 的 Python 依赖装不上 | 中 | 模块级降级（503 + 中文提示），其余模块不受影响；README 写清启动步骤 |
| 本地数据丢失（清浏览器缓存） | 中 | 各模块提供 JSON 导出/导入 |
| 删掉旧小程序后无法对照 | 中 | git 历史可回溯；步骤 10 严格前置 |
| `wx.login` 身份在 Web 端彻底缺失 | 低 | 当前云端本就未激活；导出/导入作为补偿 |

---

## 9. 不做清单（明确排除）

- 不做用户系统 / 登录（`trip-log` 的 openid 身份不恢复）
- 不做云同步 / 多端同步
- 不做 PWA / 离线缓存 / Service Worker
- 不引入 npm 依赖、前端框架、打包工具
- 不改 `engine/`
- 不改 `study-hub` 与 `wiki-rag` 的业务逻辑与内部路径假设
- 不恢复 `apps/.archive/`（旧小程序直接删除，git 历史可回溯）

---

## 10. 验收标准

合并完成需同时满足：

1. `node apps/web/server/main.js` 启动，浏览器打开 `http://127.0.0.1:5173` 可访问全部模块
2. `apps/web/tests/run-all.js` 一条命令，全部测试通过
3. `apps/study-hub` 的 `node tests/run-all.js` 仍全部通过
4. `apps/wiki-rag` 的 `python -m pytest tests -q` 仍全部通过
5. 4 个旧小程序目录已删除，`apps/README.md` 已更新为新架构说明
6. 每个模块均提供 JSON 导出/导入能力
