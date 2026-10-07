@echo off
setlocal
cd /d "%~dp0"

if exist "%~dp0install.ps1" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
) else if exist "%~dp0dist\install.ps1" (
  echo Source-tree install detected. Using the built package in dist...
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0dist\install.ps1"
) else (
  echo ERROR: install.ps1 was not found.
  echo.
  echo If this is a source checkout, run BUILD.cmd first and then run this file again.
  echo Expected built installer: "%~dp0dist\install.ps1"
  echo.
  pause
  exit /b 2
)

set "ERR=%ERRORLEVEL%"
echo.
pause
exit /b %ERR%
