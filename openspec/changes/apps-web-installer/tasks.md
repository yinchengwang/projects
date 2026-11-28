# apps-web-installer 任务清单

## 任务列表

### Task #1: 拿到 NSIS 工具链
- **状态**: completed
- **描述**: Chocolatey 要管理员、SourceForge 反爬、Inno Setup 只托管签名文件；
  最后用 electron-builder 托管的 GitHub releases 里的 NSIS 预编译包 +
  独立版 `7zr.exe` 解开
- **验收**: `makensis.exe /VERSION` 有输出；`fetch-nsis.js` 能复现

### Task #2: 生成应用图标
- **状态**: completed
- **描述**: `make-icon.js` 用 headless Chrome 渲染 `icon.svg` 出 7 个尺寸 PNG，
  再用纯 Node（zlib + 反滤波）解回 RGBA 转成 32 位 DIB 打进 ICO
- **验收**: ICO 目录项 7 条，每条的 DIB 宽高与尺寸对得上；NSIS 的 `Icon` 命令能读

### Task #3: 写启动器
- **状态**: completed
- **描述**: `launch.js` 探测端口、detached 拉起服务、开浏览器；
  `start.vbs` / `stop.vbs` 无黑框；出错弹提示框 + 写日志
- **验收**: 幂等启动；端口占用时自动避让；`--stop` 能停掉服务

### Task #4: 写 NSIS 安装脚本
- **状态**: completed
- **描述**: 逐用户安装、三个组件（应用本体 / ai-timer 数据 / 桌面快捷方式）、
  开始菜单三个入口、注册到「应用和功能」、卸载时停服务
- **验收**: 装完有 `HKCU\...\Uninstall\WorkbenchApp`；卸载后目录与注册项清空

### Task #5: 写构建脚本
- **状态**: completed
- **描述**: `build.js` 摆 staging → 转 UTF-16LE → 调 makensis → 产物落 `build/apps-web/`
- **验收**: `node apps/web/installer/build.js` 出包，退出码 0

### Task #6: 修 /api/health 自己探自己
- **状态**: completed
- **描述**: 本应用占 5174（study-hub 的端口）时，`/api/health` 探测 study-hub
  就是自己探自己 → 无限递归。实测卡 17 秒、socket 打爆
- **验收**: `server/health.js` 的 `probe()` 收 `selfPort`，命中就跳过；
  `apps/web/server/app.js` 传 `req.socket.localPort`；health.test.js 两个回归用例；
  **已验证**：去掉保护后两个用例都失败，恢复后全过

### Task #7: 端到端验证
- **状态**: completed
- **描述**: 装 → 启 → 查 → 卸，各查一遍
- **验收**:
  - 安装退出码 0，文件/快捷方式/注册项齐全
  - 启动后端口避开 5174；页面全 200；AI 简报 5 简报 / 6 巡检 / 18 系列
  - 卸载后目录、快捷方式、注册项、服务进程全清
  - `node apps/web/tests/run-all.js` 35 组全过

### Task #8: 文档与提交
- **状态**: completed
- **描述**: `apps/web/installer/README.md`、`apps/README.md` 补说明；
  `.gitignore` 排除 `.toolchain/`
- **验收**: 文档说得清怎么构建、怎么装、两个坑是什么
