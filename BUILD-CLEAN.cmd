@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build-windows.ps1" -Clean
set "ERR=%ERRORLEVEL%"
echo.
if not "%ERR%"=="0" (
  echo Clean build failed with exit code %ERR%.
) else (
  echo Clean build finished successfully.
)
pause
exit /b %ERR%
