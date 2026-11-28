' stop.vbs —— 无黑框停止工作台服务

Dim fso, sh, base, node, js
Set fso = CreateObject("Scripting.FileSystemObject")
Set sh = CreateObject("WScript.Shell")

base = fso.GetParentFolderName(WScript.ScriptFullName)
node = fso.BuildPath(base, "node.exe")
js = fso.BuildPath(base, "launch.js")

sh.Run """" & node & """ """ & js & """ --stop", 0, False
