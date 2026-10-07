@echo off
setlocal
cd /d "%~dp0"
echo WARNING: this removes all local build caches and downloaded archives for r6.3.
echo Older EB Recorder folders are not touched.
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build-windows.ps1" -FullClean
set "ERR=%ERRORLEVEL%"
echo.
if not "%ERR%"=="0" (
  echo Full clean build failed with exit code %ERR%.
) else (
  echo Full clean build finished successfully.
)
pause
exit /b %ERR%
