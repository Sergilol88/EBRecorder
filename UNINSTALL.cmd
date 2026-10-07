@echo off
setlocal
cd /d "%~dp0"

if exist "%~dp0uninstall.ps1" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
) else if exist "%~dp0dist\uninstall.ps1" (
  echo Source-tree uninstall detected. Using the built package helper in dist...
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0dist\uninstall.ps1"
) else if exist "%~dp0scripts\uninstall.ps1" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\uninstall.ps1"
) else (
  echo ERROR: uninstall.ps1 was not found.
  echo.
  pause
  exit /b 2
)

set "ERR=%ERRORLEVEL%"
echo.
pause
exit /b %ERR%
