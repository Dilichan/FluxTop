@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Start-FluxTop.ps1" %*
exit /b %errorlevel%
