@echo off
rem Builds ClearBar.exe, ClearBarTap.dll and tests.exe into .\out
rem (x64, release, static CRT). Needs Visual Studio 2022+ (or Build Tools) with
rem the "Desktop development with C++" workload and a Windows 11 SDK.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found - install Visual Studio or Build Tools & exit /b 1)
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio with C++ tools found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || (echo vcvars64 failed & exit /b 1)

if not exist out mkdir out
if not exist out\obj mkdir out\obj
set "CF=/nologo /std:c++20 /EHsc /O2 /Gy /GS /guard:cf /W4 /WX /MT /DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /permissive- /utf-8 /Zc:__cplusplus /Brepro /Fo:out\obj\ /Fd:out\obj\"
set "LF=/link /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /guard:cf /INCREMENTAL:NO /Brepro"

rc /nologo /fo out\obj\ClearBar.res src\exe\ClearBar.rc || exit /b 1
rc /nologo /fo out\obj\ClearBarTap.res src\tap\ClearBarTap.rc || exit /b 1
cl %CF% src\exe\main.cpp out\obj\ClearBar.res /Fe:out\ClearBar.exe %LF% /SUBSYSTEM:WINDOWS user32.lib kernel32.lib advapi32.lib || exit /b 1
cl %CF% /LD src\tap\tap.cpp out\obj\ClearBarTap.res /Fe:out\ClearBarTap.dll %LF% /DEF:src\tap\tap.def || exit /b 1
cl %CF% tests\tests.cpp /Fe:out\tests.exe %LF% /SUBSYSTEM:CONSOLE || exit /b 1
del /q out\*.exp out\*.lib 2>nul
echo BUILD OK
