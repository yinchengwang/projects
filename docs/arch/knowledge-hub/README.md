# Knowledge Hub 架构设计

> ⚠️ **历史文档 · 原型已迁移**
>
> 本文描述 Knowledge Hub 的 **Taro 3.6 + React 18 原型**架构。该原型（原 `engine/apps/web/knowledge_hub/`）已于 2026-10-22 删除。
>
> **当前实现**：`apps/web/` 的 `/knowledge/` 模块 —— 原生 Web 页面（合并应用，5173 端口）。
> 原型的 19 页 / 12 个 Zustand store 已重写为 17 页 / 14 个数据域（`core/stores.js`），原 SVG 图表改为 Canvas 2d。
> 本文档保留作为原型设计的**历史参考**，其中的架构决策（数据域划分、SM-2 间隔重复、雷达/图谱布局）已迁移至新实现。
> 详见 [`apps/README.md`](../../../apps/README.md)。
> 其所在目录 `engine/apps/` 已于 2026-10-23 整目录删除。

> 知识中心 — 基于 Taro 3.6 + React 18 的 H5 + 微信小程序双端学习追踪平台（原型）

## 1. 架构概览

```mermaid
flowchart TB
    subgraph H5["H5 端"]
        H5_BROWSER["浏览器"]
        H5_VITE["Vite 5 构建"]
        H5_ROUTER["React Router DOM"]
    end

    subgraph WEAPP["微信小程序端"]
        WEAPP_RUNTIME["小程序运行时"]
        WEAPP_CLI["Taro CLI 编译"]
        WEAPP_ROUTER["Taro Router"]
    end

    subgraph SHARED["共享代码层 (src/)"]
        PAGES["19 个页面"]
        STORES["12 个 Zustand Store"]
        COMPONENTS["共享组件"]
        SERVICES["服务层"]
        HOOKS["自定义 Hooks"]
        DATA["静态数据"]
        UTILS["工具函数"]
        TYPES["类型定义"]
    end

    subgraph ADAPTERS["双端适配层"]
        H5_ADAPTER["web/adapters/"]
        H5_LAYOUT["web/components/Layout/"]
        H5_STYLES["web/styles/"]
    end

    H5_BROWSER --> H5_VITE --> H5_ROUTER
    WEAPP_RUNTIME --> WEAPP_CLI --> WEAPP_ROUTER

    H5_ROUTER --> ADAPTERS
    WEAPP_ROUTER --> ADAPTERS
    ADAPTERS --> SHARED

    STORES --> SERVICES
    PAGES --> STORES
    PAGES --> COMPONENTS
    PAGES --> HOOKS
```

## 2. 技术栈

| 层级 | 技术 | 版本 | 用途 |
|------|------|------|------|
| 框架 | Taro | 3.6 | 跨端统一框架 |
| 前端框架 | React | 18 | UI 组件库 |
| 状态管理 | Zustand | 最新 | 全局状态 + persist 持久化 |
| 路由 (H5) | React Router DOM | 6.x | 浏览器端路由 |
| 路由 (小程序) | Taro Router | 内置 | 小程序页面栈 |
| 样式 | SCSS | Dart Sass | 样式预处理 |
| 样式变量 | CSS Variables | CSS 标准 | 主题切换 |
| 构建 (H5) | Vite | 5 | 快速开发/生产构建 |
| 构建 (小程序) | Taro CLI | 内置 | 小程序编译打包 |
| 测试 | Playwright | 最新 | E2E 测试 |
| 小程序平台 | @tarojs/plugin-platform-weapp | 内置 | 微信小程序编译 |

## 3. 状态管理层

12 个 Zustand Store 按职责分为 4 大类：

```mermaid
classDiagram
    direction TB

    class 学习类 {
        +knowledgeStore : 知识图谱
        +learningPathStore : 学习路径
        +reviewStore : 复习调度 SM-2
        +quizStore : 题库练习
    }

    class 面试类 {
        +interviewStore : 面试追踪
        +interviewBankStore : 面试题库
        +mockInterviewStore : 模拟面试
    }

    class 用户类 {
        +userStore : 用户信息
        +settingsStore : 应用设置
    }

    class 项目类 {
        +fiveYearPlanStore : 五年计划
        +projectStore : 项目路线
        +excerptStore : 摘抄收藏
    }

    class ZustandPersist {
        <<middleware>>
        +persist() 持久化中间件
        +localStorage H5 存储
        +TaroStorage 小程序存储
    }

    学习类 ..|> ZustandPersist : 使用
    面试类 ..|> ZustandPersist : 使用
    用户类 ..|> ZustandPersist : 使用
    项目类 ..|> ZustandPersist : 使用
```

### Store 详情

| Store | 职责 | 核心状态 | 持久化 |
|-------|------|----------|--------|
| userStore | 用户信息与认证 | 用户名、头像、登录状态 | 是 |
| settingsStore | 应用配置 | 主题、语言、通知偏好 | 是 |
| quizStore | 题库练习 | 题目列表、答题记录、正确率 | 是 |
| reviewStore | 复习调度 | 复习计划、SM-2 参数、到期队列 | 是 |
| knowledgeStore | 知识图谱 | 节点、边、标签、搜索 | 是 |
| learningPathStore | 学习路径 | 路径节点、进度、依赖关系 | 是 |
| excerptStore | 摘抄收藏 | 摘抄内容、分类、标签 | 是 |
| interviewStore | 面试追踪 | 面试记录、状态、反馈 | 是 |
| interviewBankStore | 面试题库 | 题目分类、难度、标签 | 是 |
| mockInterviewStore | 模拟面试 | 模拟场景、评分、报告 | 是 |
| fiveYearPlanStore | 五年计划 | 年度目标、里程碑、进度 | 是 |
| projectStore | 项目路线 | 项目列表、技术栈、状态 | 是 |

## 4. 页面路由

19 个页面的导航结构：

```mermaid
flowchart TB
    DASHBOARD["仪表盘 Dashboard"]
    QUIZ["题库 Quiz"]
    REVIEW["复习 Review"]
    INTERVIEW_TRACK["面试追踪 Interview Tracker"]
    INTERVIEW_BANK["面试题库 Interview Bank"]
    MOCK_INTERVIEW["模拟面试 Mock Interview"]
    KNOWLEDGE_GRAPH["知识图谱 Knowledge Graph"]
    LEARNING_PATH["学习路径 Learning Path"]
    ILLUSTRATION["图解系列 Illustration"]
    EXCERPT["摘抄 Excerpt"]
    GAP_ANALYSIS["差距分析 Gap Analysis"]
    TECH_RADAR["技术雷达 Tech Radar"]
    FIVE_YEAR["五年计划 Five Year Plan"]
    PROJECT_ROADMAP["项目路线 Project Roadmap"]
    SETTINGS["设置 Settings"]

    DASHBOARD --> QUIZ
    DASHBOARD --> REVIEW
    DASHBOARD --> INTERVIEW_TRACK
    DASHBOARD --> KNOWLEDGE_GRAPH
    DASHBOARD --> LEARNING_PATH
    DASHBOARD --> EXCERPT
    DASHBOARD --> TECH_RADAR
    DASHBOARD --> FIVE_YEAR

    INTERVIEW_TRACK --> INTERVIEW_BANK
    INTERVIEW_TRACK --> MOCK_INTERVIEW

    KNOWLEDGE_GRAPH --> GAP_ANALYSIS
    LEARNING_PATH --> ILLUSTRATION
    LEARNING_PATH --> PROJECT_ROADMAP

    SETTINGS --> SETTINGS
```

### 页面清单

| 页面 | 路径 | 功能 |
|------|------|------|
| 仪表盘 | / | 学习概览、今日任务、进度统计 |
| 题库 | /quiz | 分类题库、答题、错题本 |
| 复习 | /review | SM-2 间隔复习、到期提醒 |
| 面试追踪 | /interview-tracker | 面试记录、状态跟踪 |
| 面试题库 | /interview-bank | 题目分类、难度筛选 |
| 模拟面试 | /mock-interview | 模拟场景、评分反馈 |
| 知识图谱 | /knowledge-graph | 节点关系、标签搜索 |
| 学习路径 | /learning-path | 路径规划、进度追踪 |
| 图解系列 | /illustration | 可视化图解 |
| 摘抄 | /excerpt | 收藏摘抄、分类管理 |
| 差距分析 | /gap-analysis | 技能差距、补强建议 |
| 技术雷达 | /tech-radar | 技术趋势、掌握度 |
| 五年计划 | /five-year-plan | 年度目标、里程碑 |
| 项目路线 | /project-roadmap | 项目规划、技术栈 |
| 设置 | /settings | 主题、语言、数据管理 |

## 5. 组件体系

```mermaid
classDiagram
    direction TB

    class 共享组件 {
        <<src/components/>>
    }

    class Charts {
        +HeatmapCalendar 热力日历
        +TrendChart 趋势图
    }

    class LazyLoad {
        +LazyLoadComponent 懒加载
        +LazyLoadImage 图片懒加载
    }

    class NavBar {
        +NavBar 导航栏
        +NavBarBack 返回按钮
    }

    class Responsive {
        +ResponsiveLayout 响应式布局
        +useBreakpoint 断点 Hook
    }

    class SafeArea {
        +SafeArea 安全区
        +SafeAreaTop 顶部安全区
        +SafeAreaBottom 底部安全区
    }

    class TabBar {
        +TabBar 底部标签栏
        +TabBarItem 标签项
    }

    共享组件 --> Charts
    共享组件 --> LazyLoad
    共享组件 --> NavBar
    共享组件 --> Responsive
    共享组件 --> SafeArea
    共享组件 --> TabBar
```

### 组件职责

| 组件 | 职责 | 双端差异 |
|------|------|----------|
| HeatmapCalendar | 学习热力图日历 | H5 用 SVG，小程序用 Canvas |
| TrendChart | 学习趋势折线图 | H5 用 SVG，小程序用 Canvas |
| LazyLoad | 组件/图片懒加载 | H5 用 IntersectionObserver，小程序用 Taro API |
| NavBar | 顶部导航栏 | H5 自定义，小程序用 Taro NavBar |
| Responsive | 响应式布局 | H5 用 CSS Media Query，小程序用 Taro.getSystemInfo |
| SafeArea | 安全区适配 | H5 用 env()，小程序用 Taro SafeArea |
| TabBar | 底部标签栏 | H5 自定义，小程序用 Taro TabBar |

## 6. 数据流

```mermaid
sequenceDiagram
    participant P as 页面
    participant H as Hook
    participant S as Store
    participant SV as Service
    participant R as 渲染

    P->>H: useStore / 自定义 Hook
    H->>S: 读取状态
    S-->>H: 返回状态
    H-->>P: 提供状态 + 操作

    P->>S: 触发 Action
    S->>SV: 调用 Service
    SV->>SV: 数据处理/计算
    SV-->>S: 返回结果
    S->>S: 更新状态
    S->>R: 触发重渲染
    R-->>P: UI 更新

    Note over S: Zustand persist 自动持久化
```

### 数据流原则

1. **单向数据流**：页面 → Hook → Store → Service → Store → 渲染
2. **Store 为单一数据源**：所有共享状态集中在 Zustand Store
3. **Service 无状态**：Service 只做数据处理和计算，不持有状态
4. **Hook 封装逻辑**：页面通过 Hook 访问 Store，不直接 import Store
5. **持久化透明**：persist 中间件自动同步 localStorage / Taro Storage

## 7. 双端适配

```mermaid
flowchart LR
    subgraph SOURCE["源码"]
        SRC["src/ 共享代码"]
        WEB["web/ H5 适配"]
    end

    subgraph BUILD["构建"]
        VITE["Vite 5\nH5 构建"]
        TARO_CLI["Taro CLI\n@tarojs/plugin-platform-weapp"]
    end

    subgraph OUTPUT["输出"]
        H5_BUNDLE["dist/h5/\nHTML + JS + CSS"]
        WEAPP_BUNDLE["dist/weapp/\n小程序代码包"]
    end

    subgraph RUNTIME["运行时"]
        BROWSER["浏览器\nReact Router DOM"]
        WEAPP["微信小程序\nTaro Router"]
    end

    SRC --> VITE
    WEB --> VITE
    VITE --> H5_BUNDLE
    H5_BUNDLE --> BROWSER

    SRC --> TARO_CLI
    TARO_CLI --> WEAPP_BUNDLE
    WEAPP_BUNDLE --> WEAPP
```

### 适配策略

| 差异点 | H5 方案 | 小程序方案 | 适配文件 |
|--------|---------|------------|----------|
| 路由 | React Router DOM | Taro Router | web/adapters/router.ts |
| 存储 | localStorage | Taro Storage | web/adapters/storage.ts |
| 导航栏 | 自定义 NavBar | Taro 原生 NavBar | web/adapters/nav.ts |
| 底部栏 | 自定义 TabBar | Taro 原生 TabBar | web/adapters/tab.ts |
| 安全区 | CSS env() | Taro SafeArea | web/adapters/safearea.ts |
| 图片懒加载 | IntersectionObserver | Taro.createIntersectionObserver | web/adapters/lazyload.ts |

## 8. 关键代码位置

| 模块 | 路径 | 说明 |
|------|------|------|
| H5 入口 | web/main.tsx | React 启动入口 |
| H5 App | web/App.tsx | 路由配置 + 错误边界 |
| 小程序入口 | src/app.config.ts | 小程序配置 |
| 路由配置 | web/config/routes.ts | H5 路由表 |
| 状态管理 | src/stores/*.ts | 12 个 Zustand Store |
| 页面组件 | src/pages/*/index.tsx | 19 个页面 |
| 共享组件 | src/components/*/*.tsx | 7 大类组件 |
| H5 适配 | web/adapters/*.ts | Taro → H5 适配层 |
| 布局组件 | web/components/Layout/*.tsx | H5 布局 |
| 全局样式 | src/styles/*.scss | 全局 SCSS |
| H5 样式 | web/styles/*.scss | H5 专用样式 |
| 静态数据 | src/data/*.ts | 题库、面试题等静态数据 |
| 自定义 Hook | src/hooks/*.ts | 业务逻辑封装 |
| 服务层 | src/services/*.ts | 数据处理和计算 |
| 类型定义 | src/types/*.ts | TypeScript 类型 |
| 工具函数 | src/utils/*.ts | 通用工具 |
| E2E 测试 | e2e/*.spec.ts | Playwright 测试 |
| 文档 | docs/*.md | 项目文档 |

---

> **项目位置**：已删除（2026-10-23）；现实现见 `apps/web/` 的 `/knowledge/` 模块