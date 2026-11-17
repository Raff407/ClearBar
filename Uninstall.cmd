@echo off
rem Double-click to remove ClearBar.
set "PSModulePath="
if not exist "%~dp0uninstall.ps1" (
  echo uninstall.ps1 is missing next to this file.
  echo Use Settings, Apps, Installed apps, ClearBar, Uninstall instead.
  echo.
  pause
  exit /b 1
)
echo On Windows 11, Explorer restarts briefly at the end so every file can be removed.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" -Yes
echo.
pause
