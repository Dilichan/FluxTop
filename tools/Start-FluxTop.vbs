Option Explicit
Dim files, shell, repository, script, command
Set files = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")
repository = files.GetParentFolderName(files.GetParentFolderName(WScript.ScriptFullName))
script = files.BuildPath(repository, "tools\Start-FluxTop.ps1")
command = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & script & """ -Autostart"
shell.Run command, 0, False
