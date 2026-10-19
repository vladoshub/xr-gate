@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0XrGate-Wsl.ps1" -Action Run -StartSteamVR %*
set "XR_GATE_EXIT=%ERRORLEVEL%"
echo.
echo XR Gate finished with exit code %XR_GATE_EXIT%. See START-HERE.html.
pause
exit /b %XR_GATE_EXIT%

