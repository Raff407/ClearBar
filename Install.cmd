@echo off
rem Double-click to install ClearBar for the current user (no admin rights needed).
set "PSModulePath="
if not exist "%~dp0install.ps1" (
  echo Please extract the whole zip first ^(right-click it, Extract All^),
  echo then run Install.cmd from the extracted folder.
  echo.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
if not "%errorlevel%"=="0" (
  echo.
  echo Installation did not finish - see the message above.
)
echo.
pause
