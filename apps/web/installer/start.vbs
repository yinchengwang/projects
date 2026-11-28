' start.vbs —— 无黑框启动工作台
'
' 用 VBS 而不是直接跑 node.exe，是为了不弹控制台窗口。
' 安装目录从脚本自身位置推出来，所以整个文件夹挪走了也能用。

Dim fso, sh, base, node, js
Set fso = CreateObject("Scripting.FileSystemObject")
Set sh = CreateObject("WScript.Shell")

base = fso.GetParentFolderName(WScript.ScriptFullName)
node = fso.BuildPath(base, "node.exe")
js = fso.BuildPath(base, "launch.js")

sh.Run """" & node & """ """ & js & """", 0, False
