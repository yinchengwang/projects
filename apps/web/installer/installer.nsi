; 工作台 —— Windows 安装包脚本
;
; 由 apps/web/installer/build.js 调用（makensis /DSTAGE=... /DOUTFILE=...）。
; 单独跑这个脚本没意义 —— 目录结构靠 build.js 摆好。

Unicode true

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!ifndef STAGE
  !error "缺少 /DSTAGE=..."
!endif
!ifndef OUTFILE
  !error "缺少 /DOUTFILE=..."
!endif
!ifndef APP_VERSION
  !define APP_VERSION "0.0.0"
!endif

!define APP_NAME      "工作台"
!define APP_ID        "WorkbenchApp"
!define APP_REGKEY    "Software\${APP_ID}"
!define UNINST_KEY    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}"
!define MUI_PAGE_WELCOME_TEXT "左边是选择，右边是安装。"

Name "${APP_NAME} ${APP_VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\${APP_ID}"
InstallDirRegKey HKCU "${APP_REGKEY}" "InstallDir"

; 逐用户安装 —— 不需要管理员权限
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show

Function .onInit
  ; 只能在这里设：指令级不允许
  SetShellVarContext current
FunctionEnd

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "FileDescription" "${APP_NAME} 安装程序"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "CompanyName" "本地应用"
VIAddVersionKey "LegalCopyright" "仅供本机使用"

!define MUI_ABORTWARNING
!define MUI_ICON "${STAGE}\icon.ico"
!define MUI_UNICON "${STAGE}\icon.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\start.vbs"
!define MUI_FINISHPAGE_RUN_TEXT "立即启动 ${APP_NAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\license.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

; ---------------------------------------------------------------- 组件

Section "应用本体" SecCore
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${STAGE}\node.exe"
  File "${STAGE}\launch.js"
  File "${STAGE}\icon.ico"
  File "${STAGE}\start.vbs"
  File "${STAGE}\stop.vbs"
  File "${STAGE}\README.txt"

  SetOutPath "$INSTDIR\app"
  File /r "${STAGE}\app\*.*"

  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; 注册到「应用和功能」
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "本地应用"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\icon.ico"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegStr HKCU "${UNINST_KEY}" "NoModify" "1"
  WriteRegStr HKCU "${UNINST_KEY}" "NoRepair" "1"

  ; 记下安装目录，重装时沿用
  WriteRegStr HKCU "${APP_REGKEY}" "InstallDir" "$INSTDIR"

  ; 开始菜单
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\start.vbs"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\停止${APP_NAME}.lnk" "$INSTDIR\stop.vbs"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\卸载${APP_NAME}.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

Section "AI 简报数据" SecAiTimer
  SetOutPath "$INSTDIR\ai-timer"
  File /r "${STAGE}\ai-timer\*.*"
SectionEnd

Section "桌面快捷方式" SecDesktop
  CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\start.vbs"
SectionEnd

; ---------------------------------------------------------------- 卸载

Section "Uninstall"
  ; 先停服务，不然文件被占用
  nsExec::ExecToLog '"$INSTDIR\node.exe" "$INSTDIR\launch.js" --stop'
  Pop $0

  Delete "$INSTDIR\node.exe"
  Delete "$INSTDIR\launch.js"
  Delete "$INSTDIR\icon.ico"
  Delete "$INSTDIR\start.vbs"
  Delete "$INSTDIR\stop.vbs"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\port.txt"
  Delete "$INSTDIR\runtime.json"
  Delete "$INSTDIR\launcher.log"
  Delete "$INSTDIR\uninstall.exe"
  RMDir /r "$INSTDIR\app"
  RMDir /r "$INSTDIR\ai-timer"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\停止${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\卸载${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"
  Delete "$DESKTOP\${APP_NAME}.lnk"

  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "${APP_REGKEY}"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "应用本体、快捷方式与卸载入口（必需）"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAiTimer} "AI 定时任务的产物目录（AI 简报模块的数据源）"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "在桌面上放一个${APP_NAME}快捷方式"
!insertmacro MUI_FUNCTION_DESCRIPTION_END
