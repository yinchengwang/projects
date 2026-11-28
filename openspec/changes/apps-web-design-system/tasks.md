# apps-web-design-system 任务清单

## 任务列表

### Task #1: 量化体检
- **状态**: completed
- **描述**: 写 `build/design-review/audit.js` 算 WCAG 对比度、色彩分布、字号分布；
  写 `probe.html` / `probe2.html` 用 headless Chrome 吐 `getComputedStyle`
- **验收**: 定位到 5 个具体问题（饱和度 1.5%、卡片亮度差 2%、muted 对比 2.31、
  11-13px 字号、`:focus` 0 处），外加 2 个功能缺陷（暗色只染黑正文、
  `tone-*` 是死类）

### Task #2: 重写共用层样式
- **状态**: completed
- **描述**: `web/style.css` 全量重写 —— 令牌体系、页面底/卡面拉开、
  边框 1px、正文 15px、对比度修正、品牌色深一档、焦点环、表单基线、
  交互过渡、模块彩条、`tone-*` 样式、`prefers-reduced-motion`
- **验收**: 对比度双主题全过 AA；`design-system.test.js` 28 条通过

### Task #3: 修主题类挂载点
- **状态**: completed
- **描述**: `with-theme.js` 的主题类从 `#app-main` 改挂 `document.body`
  —— 原来页头在 `#app-main` 外面，暗色只染黑正文区
- **验收**: 暗色主题下页头与正文同色；`wb-core.test.js` 主题用例全过

### Task #4: 统一色板
- **状态**: completed
- **描述**: `constants.js` 主色/分类色/优先级色/心情色、`charts.js` 默认色、
  `theme.js` 的 `navFront/navBg` 全部对齐新色板
- **验收**: 浅色主题 `--brand` 与白底 5.93:1；暗色用亮档 `#38BDF8`

### Task #5: 模块层边框与过渡
- **状态**: completed
- **描述**: 5 个模块 CSS 的 `0.5px` 全改 `1px`（30 处）；
  各模块补上自己可点元素的过渡
- **验收**: 全仓 CSS 无 `0.5px`；`design-system.test.js` 相应断言通过

### Task #6: 语义色落地
- **状态**: completed
- **描述**: 优先级药丸按 `data-priority` 上色；心情按钮按 `data-level` 上色
  并渲染 `MOOD_LEVELS` 里一直没人用的 emoji
- **验收**: `getComputedStyle` 确认颜色生效；DOM 里 5 个 `data-level`、
  2 个 `data-priority` 齐全

### Task #7: 预览壳对齐
- **状态**: completed
- **描述**: `web/preview.html` 的配色改成与应用共用层同一套语言
- **验收**: 预览页饱和度 8.7% → 16.1%

### Task #8: 测试与提交
- **状态**: completed
- **描述**: 新增 `tests/design-system.test.js`；同步 `wb-core.test.js` /
  `wb-pages.test.js` 的色值断言；HEX 解析用例改用与调色板无关的颜色
- **验收**:
  - `node tests/run-all.js` → 36 组全过，exit 0
  - **验证过用例能抓到问题**：退回弱对比度 + `0.5px` 后 4 条变红，恢复后全绿
