@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0XrGate-Wsl.ps1" -Action Stop %*
set "XR_EXIT=%errorlevel%"
echo.
echo XR Gate finished with exit code %XR_EXIT%. See START-HERE.html.
pause
exit /b %XR_EXIT%
