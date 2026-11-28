# 应用集合

本目录是仓库内**所有本机应用**的 canonical 位置。

合并后的形态：一个入口 + 两个独立服务。

```
apps/
├── web/           合并应用（Node 服务，5173）
│   ├── server/    组合根：路由、静态托管、健康检查
│   ├── shared/    共用层：core / ui / charts / platform / photos + 各模块数据层
│   └── web/       各模块前端（workbench / knowledge / trip / games）
├── study-hub/     学习中枢（Node，5174，独立进程）
└── wiki-rag/      Wiki 检索（Python FastAPI，8000，独立进程）
```

> 历史说明：原先的 4 个微信小程序（workbench / knowledge-hub / trip-log / games）
> 已于 2026-11-26 整体移植为原生 Web 页面并合并进 `apps/web/`。那些小程序更早前
> 从 `engine/apps/web/`（Taro 工程）原生化重写而来。

---

## 合并应用 · `web/`（5173）

**一个入口，四个模块。** 43 个页面，全部是原生 Web 页面。

### 启动

```bash
node apps/web/server/main.js
# 打开 http://127.0.0.1:5173
```

需要 Node ≥ 24。零 npm 依赖、零构建。

### 模块

| 模块 | 路径 | 页面数 | 说明 |
|---|---|---|---|
| 程序员工作台 | `/workbench/` | 18 | 待办、技术笔记、代码片段、面试题库、资源、读书、健身、心情、日程、项目集、闪念 + 10 个离线工具 |
| 知识中心 | `/knowledge/` | 17 | SM-2 间隔复习、题库练习与错题本、面试追踪与模拟面试、知识图谱（Canvas）、技术雷达（Canvas）、五年计划 |
| AI 简报 | `/aitimer/` | 3 | 浏览 AI 定时任务产物：每日简报、代码仓巡检、每日一特性深度系列 |
| 足迹 | `/trip/` | 3 | 记录走过的地方与照片（照片存 IndexedDB） |
| 游戏 | `/games/` | 5 | 贪吃蛇、2048、数独、消消乐 |

### 数据存哪

| 数据 | 存储 | 说明 |
|---|---|---|
| workbench / knowledge / trip 的业务数据 | `localStorage`，`db:` 前缀 | 按数据域分键 |
| games 的存档 | `localStorage`，`games_` 前缀 | 与上面不冲突 |
| trip 的照片 | **IndexedDB** | 单条记录最多 9 张 × 5MB，超出 localStorage 配额 |

全部数据只在本机，不上传任何服务器。

### AI 简报模块（`/aitimer/`）

**唯一一个「数据在磁盘上、不在浏览器里」的模块。** 它读取仓库根的 `ai-timer/` 目录 ——
那是 3 个 AI 定时任务的产物目录（详见 [`../ai-timer/README.md`](../ai-timer/README.md)）。

`ai-timer/` 被 `.gitignore` 忽略，所以**新克隆的仓可能没有这个目录**。
模块对此做了兜底：目录不存在时接口返回 200 + `exists: false`，前端显示空态而不是报错。

| 集合 | 目录 | 内容 |
|---|---|---|
| AI 简报 | `ai-timer/AI简报/` | 每日数据库+AI 简报，按日期倒序 |
| 代码仓巡检 | `ai-timer/代码仓巡检/` | 每日只读巡检报告，按日期倒序 |
| 每日一特性 | `ai-timer/每日一特性/learn_deep/<领域>/` | 各领域系列，系列内含 README 索引页与专题文章 |

服务端接口（`apps/web/server/aitimer.js`）：

| 接口 | 返回 |
|---|---|
| `GET /api/aitimer/index` | 完整索引：三个集合 + 每个文档的元信息 |
| `GET /api/aitimer/doc?id=<docId>` | 单篇文档（正文已去掉 frontmatter） |
| `GET /api/aitimer/raw?id=<docId>` | 原始 Markdown 全文（含 frontmatter） |

文档 id 形如 `briefing/2026-11-28`、`inspection/2026-11-28`、`learn/db/pg-hot-update`。
id 含斜杠，所以走 query 参数而非路由参数。

可用 `AI_TIMER_ROOT` 环境变量覆盖 `ai-timer` 根目录（测试用）。

### 目录

```
web/
├── server/          组合根：main / app / router / static / health / aitimer
├── shared/
│   ├── core/        通用层：storage / store / id / date / format / md / constants / theme
│   ├── ui.js        构件层（页头 / 主卡 / 数字块 / 药丸 / 进度条 / 空态）
│   ├── charts.js    手绘 SVG 图表层
│   ├── platform.js  平台适配层（承接 wx 的交互 API，Promise 风格）
│   ├── photos.js    IndexedDB 图片存储层
│   ├── workbench/   workbench 数据层（12 个数据域 + 10 个工具）
│   ├── knowledge/   knowledge-hub 数据层（14 个数据域 + 图谱/雷达布局）
│   ├── trip/        trip 数据层 + 导出导入
│   └── games/       4 个游戏引擎 + 存档层
├── web/             各模块前端 + cjs-loader.js（浏览器侧 CommonJS 加载器）
└── tests/           全部测试，一条命令跑完
```

### 测试

```bash
cd apps/web && node tests/run-all.js
```

---

## 学习中枢 · `study-hub/`（5174）

算法 / 数据库 / AI 三轨合一的学习助手。本机运行，判题走真实编译执行。

**独立进程**，不与合并应用耦合。

```bash
PORT=5174 node apps/study-hub/server/main.js
# 打开 http://127.0.0.1:5174
```

需要 Node ≥ 24。设 `STUDY_HUB_LEARNING_ROOT` 指向 learning 仓可开启同步；不设则纯本地模式。

```bash
cd apps/study-hub && node tests/run-all.js
```

---

## Wiki 检索 · `wiki-rag/`（8000）

本地 wiki 语料的三栏混合检索工作台。答案为抽取式，只拼接检索到的原文块。

**独立进程**（Python）。

```powershell
cd apps\wiki-rag
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
python tools/ingest.py --project postgres   # 建索引
python -m server.main
# 打开 http://127.0.0.1:8000
```

```powershell
python -m pytest tests -q
```

---

## 三个服务的关系

合并应用的外壳是**导航入口**；study-hub 与 wiki-rag 各自独立启动、独立端口。

原因是这两个应用的前端硬编码了绝对路径（study-hub 有 35 处 `fetch("/api/...")`），
挂到子路径下会全部失效。保持独立端口，它们一行都不用改。

外壳首页会显示两个后端的在线状态（运行中 / 未启动），未启动时鼠标悬停给出启动命令。
