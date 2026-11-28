# apps-web-installer：把合并应用打成 Windows 安装包

**变更名称：** 合并应用 Windows 安装包
**创建日期：** 2026-11-28
**状态：** 进行中
**关联模块：** `apps/web/installer/`

---

## 背景

`apps/web` 是本机 Web 应用：零 npm 依赖、零构建，`node apps/web/server/main.js` 就能跑。
但「跑起来」和「装上」是两回事：

- 没有安装包 → 每次都要开终端敲命令、记端口
- 没有开始菜单/桌面入口 → 不像一个已安装的应用
- 没有卸载器 → 删文件靠手动
- 没有自带 Node 运行时 → 换台机器就起不来

## What Changes

### 必做项

| 文件 | 类型 | 说明 |
|---|---|---|
| `apps/web/installer/build.js` | 新建 | 构建流程：摆 staging → 调 makensis → 产物落 `build/apps-web/` |
| `apps/web/installer/installer.nsi` | 新建 | NSIS 脚本：组件、快捷方式、卸载项、卸载时停服务 |
| `apps/web/installer/launch.js` | 新建 | 启动器：探测端口、拉起服务、开浏览器 |
| `apps/web/installer/start.vbs` | 新建 | 无黑框启动（快捷方式指向它） |
| `apps/web/installer/stop.vbs` | 新建 | 无黑框停止 |
| `apps/web/installer/fetch-nsis.js` | 新建 | 拉 NSIS 工具链到 `.toolchain/`（不入库） |
| `apps/web/installer/make-icon.js` | 新建 | SVG → 多尺寸 PNG → 32 位 DIB 的 .ico |
| `apps/web/installer/icon.svg` | 新建 | 图标源文件 |
| `apps/web/installer/icon.ico` | 新建 | 生成物，入库 |
| `apps/web/installer/license.txt` | 新建 | 安装向导的许可说明 |
| `apps/web/installer/README.txt` | 新建 | 装完给用户看的说明 |
| `apps/web/installer/README.md` | 新建 | 构建与使用说明 |
| `apps/web/server/health.js` | 修改 | 探测目标端口等于本服务端口时跳过（防自己探自己） |
| `apps/web/server/app.js` | 修改 | `/api/health` 把本服务端口传进探测 |
| `apps/web/tests/health.test.js` | 修改 | 两个回归用例 |
| `.gitignore` | 修改 | 排除 `apps/web/installer/.toolchain/` |
| `apps/README.md` | 修改 | 补安装包说明 |

### 不做

- 不做 macOS / Linux 安装包 —— 需求只提 Windows
- 不把 NSIS 二进制入库 —— 太大，`fetch-nsis.js` 现拉现用
- 不改应用本体的功能 —— 只加打包这一层

## 影响

- `apps/web` 多了一个 `installer/` 目录，仍是零 npm 依赖
- 应用本体有两处小改动（health 自探测保护），是为了装完能正常跑
- 产物在 `build/apps-web/`，不入库
