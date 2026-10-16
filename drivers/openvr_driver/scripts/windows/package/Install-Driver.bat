@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Manage-Driver.ps1" -Action Install %*
set "XR_EXIT=%errorlevel%"
pause
exit /b %XR_EXIT%
