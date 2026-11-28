# Windows 安装包

把合并应用（`apps/web`）打成一个能双击安装的 Windows 安装程序。

```bash
node apps/web/installer/build.js
# 产物：build/apps-web/工作台-1.0.0-安装包.exe
```

## 装完是什么样

| 项目 | 内容 |
|---|---|
| 安装位置 | `%LOCALAPPDATA%\Programs\WorkbenchApp`（逐用户安装，**不需要管理员权限**） |
| 开始菜单 | 「工作台」「停止工作台」「卸载工作台」三个入口 |
| 桌面 | 一个「工作台」快捷方式（可在安装时取消勾选） |
| 卸载 | Windows「应用和功能」里能找到，也能从开始菜单卸载 |

装完点「工作台」→ 自动起服务并打开浏览器。点「停止工作台」→ 关掉服务。

**不依赖系统里装没装 Node** —— 安装包自带一份 node.exe。
**不写注册表启动项**，不后台常驻，关了就是关了。

## 装了什么

```
WorkbenchApp/
├── node.exe          Node 运行时（自带，不依赖系统）
├── launch.js         启动器：拉起服务 + 开浏览器
├── start.vbs         无黑框启动（快捷方式指向它）
├── stop.vbs          无黑框停止
├── icon.ico          应用图标
├── app/              应用本体（server / shared / web）
├── ai-timer/         AI 定时任务的产物（AI 简报模块的数据源）
└── uninstall.exe
```

`AI_TIMER_ROOT` 由启动器指向安装目录下的 `ai-timer/`，
所以装完 AI 简报模块照常能用。

## 构建

### 工具链

打包要 `makensis.exe`（NSIS）。仓库里不放二进制，`fetch-nsis.js` 负责拉：

```bash
node apps/web/installer/fetch-nsis.js
```

它从 electron-builder 托管的 GitHub releases 下 NSIS 的预编译包，
用独立版 `7zr.exe` 解开，落到 `apps/web/installer/.toolchain/nsis/`（不入库）。

### 图标

```bash
node apps/web/installer/make-icon.js
```

借 headless Chrome 把 `icon.svg` 渲染成 7 个尺寸的 PNG，
再转成 32 位 DIB 打进 `icon.ico`。

**为什么不用 PNG 压缩的 ICO**：NSIS 的 `Icon` / `UninstallIcon` 命令只认 BMP 格式的
ICO 条目，PNG 那种它读不了。所以 `make-icon.js` 里带了纯 Node 的 PNG 解码
（zlib + 反滤波）再转 DIB。

### 主流程

```bash
node apps/web/installer/build.js              # 带 ai-timer 数据
node apps/web/installer/build.js --skip-ai-timer   # 不带
```

依次做三件事：摆 staging → 调 makensis → 产物落到 `build/apps-web/`。

## 两个坑（都踩过了）

**一、NSIS 脚本必须是 UTF-16LE。**
NSIS 按 ANSI 读脚本，中文注释会直接报 `Bad text encoding`。
仓库里源文件保持 UTF-8，`build.js` 在编译前转成 UTF-16LE。
同理 `license.txt` / `README.txt` 也得转 —— 安装向导的文本框按 ANSI/UTF-16 渲染，
UTF-8 会乱码。

**二、本应用不能跑在 5174。**
5174 是 study-hub 的端口，而 `/api/health` 会去探测 study-hub。
本应用要是也占 5174，探测就变成自己探自己 → `/api/health` 无限递归
（实测卡 17 秒，socket 也会被打爆）。

启动器因此跳过了 5174 和 8000；`server/health.js` 里也加了保护：
探测目标端口等于本服务端口时直接跳过。两条都做了。

## 目录

```
apps/web/installer/
├── build.js        构建：摆 staging → 调 makensis
├── installer.nsi   NSIS 脚本
├── launch.js       启动器（装完用）
├── start.vbs       无黑框启动（快捷方式指向它）
├── stop.vbs        无黑框停止
├── fetch-nsis.js   拉 NSIS 工具链
├── make-icon.js    生成 icon.ico
├── icon.svg        图标源
├── icon.ico        生成物（入库）
├── license.txt     安装向导的许可说明
└── README.txt      装完给用户看的说明
```

零 npm 依赖。
