# apps-web-walkthrough 任务清单

## 任务列表

### Task #1: 摸清真实 DOM
- **状态**: completed
- **描述**: 用 headless Chrome 的 `--dump-dom` 把首页、五个模块落地页、
  仪表盘的 class 与 href 抓出来，作为步骤表里选择器的依据
- **验收**: 15 个步骤用到的页面全部 200；每个 `pick` 选择器都能在真实 DOM 里命中

### Task #2: 写导览页
- **状态**: completed
- **描述**: `apps/web/web/walkthrough.html` —— 左栏步骤条、右栏真应用 iframe、
  高亮框、真点击、peek/点击两种模式、file:// 自动跳转
- **验收**: 19 步都能走通

### Task #3: 修 rAF 的 this 丢失
- **状态**: completed
- **描述**: `frame.contentWindow.requestAnimationFrame` 摘下来调用会丢 `this`，
  Chrome 抛 "parameter 1 is not of type 'Function'"。
  异常在 `highlight()` 里抛出，把后面的 `clickForReal()` 一起带没 ——
  表现是高亮出来了但点了没反应。改成 `.bind(win)`，并给每步加 try/catch
- **验收**: 控制台 0 异常；「点程序员工作台」真的跳到了 `/workbench/pages/dashboard/index`

### Task #4: 修 children.indexOf 两个真 bug
- **状态**: completed
- **描述**: `shared/platform.js` 的 `showToast` 清理与 `closeMask` 用了
  `children.indexOf`/`children.splice` —— 浏览器里 `children` 是 HTMLCollection，
  没这两个方法。后果：提示条永远消不掉；对话框关掉后遮罩留着，整个应用点不动
- **验收**:
  - 两处改成 `parentNode.removeChild(node)`
  - **在真实浏览器里验过**：点「添加待办」→ 取消 → 遮罩数 0，应用仍可点
  - 退回旧写法后遮罩数 1、控制台报 `children.indexOf is not a function`

### Task #5: 补测试
- **状态**: completed
- **描述**: 新增 `apps/web/tests/platform-dom.test.js` —— 用一个 `children` 只读
  （无 `indexOf`/`splice`）的 DOM 桩，把浏览器的真实约束搬进测试
- **验收**:
  - 3 条用例通过
  - **验证过能抓到问题**：退回旧写法后 3 条全红，恢复后全绿
  - `node tests/run-all.js` → 37 组全过，exit 0

### Task #6: 接入首页
- **状态**: completed
- **描述**: 首页加「先看看它怎么用」区块（导览 + 页面预览两个入口）；
  导航加「操作导览」；两张卡片各配一个彩条色
- **验收**: 首页两个入口可点，导航项可点

### Task #7: 重建安装包
- **状态**: completed
- **描述**: 应用文件变了，安装包要重出
- **验收**: 装出来的 `app/web/` 里含 `walkthrough.html` 且 CSS 是新的
