# apps-web-installer 设计说明

## 目标形态

一个 Windows `.exe` 安装包，双击即装，装完有开始菜单和桌面入口，
自带 Node 运行时，不写注册表启动项，卸载干净。

## 为什么是 NSIS

候选与结论：

| 方案 | 结论 |
|---|---|
| NSIS | ✅ 选定。脚本小、控制全、能产出真正的卸载器。预编译二进制可从 electron-builder 的 GitHub releases 直接拿 |
| Inno Setup | 官网只托管 `.issig` 签名文件，安装包在表单后面，自动化拿不到 |
| IExpress | 系统自带，但只能自解压 + 跑脚本，界面粗糙、卸载器要自己写 |
| electron-builder | 要拉 Electron（~100MB），且会把应用架构改成 Electron —— 与「零依赖零构建」冲突 |
| WiX / MSI | 需要 .NET 与管理员权限，装出来是 MSI 不是 exe |

## 关键决策

### 逐用户安装，不要管理员权限

```nsis
RequestExecutionLevel user
SetShellVarContext current
```

装到 `%LOCALAPPDATA%\Programs\WorkbenchApp`，写 `HKCU`。
用户不需要管理员密码，也不会触发 UAC。

`SetShellVarContext` 只能放在 `.onInit` 里 —— 指令级会报
`command SetShellVarContext not valid outside Section or Function`。

### 启动器用 VBS 包一层

直接让快捷方式指向 `node.exe launch.js` 会弹一个控制台窗口。
所以快捷方式指向 `start.vbs`：

```vbs
sh.Run """" & node & """ """ & js & """", 0, False
```

`0` 是隐藏窗口。`start.vbs` / `stop.vbs` 从脚本自身位置推安装目录
（`GetParentFolderName(WScript.ScriptFullName)`），所以整个文件夹挪走了照样能用。

启动器是无窗口的，出错时没人看得见 —— 所以它自己弹提示框（`mshta`），
并把错误写进 `launcher.log`。

### 端口选择：跳过 5174 与 8000

本应用默认 5173，但 5173 被占时会往后找。**不能落在 5174** ——
那是 study-hub 的端口，而 `/api/health` 会去探测 study-hub。
本应用占 5174 就是自己探自己 → 无限递归。

实测症状：`/api/health` 卡 17 秒，socket 被自己的递归请求打爆，
客户端拿到 `EADDRINUSE`。

两条都堵上了：

1. `launch.js` 的 `reserved()` 跳过 5174 / 8000
2. `server/health.js` 的 `probe()` 多收一个 `selfPort`，
   目标端口等于本服务就直接跳过

第 2 条要靠 `req.socket.localPort` 拿自己的端口 —— 请求进来时
`localPort` 就是它自己监听的端口。

### 就绪探测探首页，不探 /api/health

`/api/health` 要去探测 study-hub 与 wiki-rag，这两个没起的时候
每次要等 1.5 秒超时，不适合当就绪信号。探 `/`（静态首页） instant。

### NSIS 脚本必须是 UTF-16LE

NSIS 按 ANSI 读脚本，中文注释直接报 `Bad text encoding`。
仓库里源文件保持 UTF-8，`build.js` 编译前统一转 UTF-16LE。

`license.txt` / `README.txt` 同理 —— 安装向导的文本框按 ANSI/UTF-16 渲染，
UTF-8 会乱码。

### 图标必须是 BMP 格式的 ICO

NSIS 的 `Icon` / `UninstallIcon` 只认 BMP 条目，PNG 压缩的读不了。
所以 `make-icon.js` 用 Chrome 渲染 SVG 出 PNG，再用纯 Node（zlib + 反滤波）
解回 RGBA，转成 32 位 DIB 打进 ICO。

### 服务进程要 detached

启动器退出后服务得继续跑：

```js
spawn(nodeExe, [entry], { detached: true, stdio: "ignore", windowsHide: true })
child.unref()
```

端口与 pid 写进 `runtime.json`，`--stop` 靠它停。

## 目录布局

```
apps/web/installer/
├── build.js        构建编排
├── installer.nsi   NSIS 脚本
├── launch.js       启动器（装完用）
├── start.vbs       无黑框启动
├── stop.vbs        无黑框停止
├── fetch-nsis.js   拉工具链
├── make-icon.js    生成图标
├── icon.svg/.ico   图标
├── license.txt     向导许可说明
└── README.txt      装完的说明
```

安装后：

```
WorkbenchApp/
├── node.exe  launch.js  start.vbs  stop.vbs  icon.ico  README.txt
├── app/       ← apps/web/{server,shared,web}
├── ai-timer/  ← 仓库根 ai-timer/（AI 简报的数据源）
└── uninstall.exe
```

启动器把 `AI_TIMER_ROOT` 指到安装目录下的 `ai-timer/`，
装完 AI 简报模块照常能用。

## 验证方式

- `node apps/web/installer/build.js` 出包，检查退出码与体积
- 静默安装 `Setup.exe /S` → 查文件、快捷方式、注册项
- 跑 `launch.js` → 查端口、页面 200、AI 简报数据条数
- 静默卸载 → 查目录、快捷方式、注册项、服务进程全清
- `node apps/web/tests/run-all.js` 全过
