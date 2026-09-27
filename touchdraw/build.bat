@echo off
rem TouchDraw build script. Usage: build.bat [console|windows] [OutName.exe]
rem Bypasses VsDevCmd (flaky in this environment) with explicit toolchain env.
setlocal
set "MODE=%1"
set "OUTNAME=%2"
if "%MODE%"=="" set "MODE=console"
if "%OUTNAME%"=="" set "OUTNAME=touchdraw-dev.exe"

set "MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "PATH=%MSVC%\bin\Hostx86\x86;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\ucrt"
set "LIB=%MSVC%\lib\x86;%SDK%\Lib\%SDKVER%\um\x86;%SDK%\Lib\%SDKVER%\ucrt\x86"

cd /d D:\Anshul_Projects\Touchdraw\touchpad
if "%MODE%"=="windows" (
  cl /nologo /W3 /D_CONSOLE /DUNICODE /D_UNICODE /Fo"D:\Anshul_Projects\Touchdraw\touchdraw\build-app\\" main.c point2d.c stroke.c touchevents.c touchpad.c utils.c history.c fileio.c hid.lib comdlg32.lib user32.lib gdi32.lib kernel32.lib libcmt.lib libvcruntime.lib libucrt.lib /link /SUBSYSTEM:WINDOWS /OUT:"D:\Anshul_Projects\Touchdraw\touchdraw\build-app\%OUTNAME%"
) else (
  cl /nologo /W3 /D_DEBUG /D_CONSOLE /DUNICODE /D_UNICODE /Fo"D:\Anshul_Projects\Touchdraw\touchdraw\build-app\\" main.c point2d.c stroke.c touchevents.c touchpad.c utils.c history.c fileio.c hid.lib comdlg32.lib user32.lib gdi32.lib kernel32.lib libcmt.lib libvcruntime.lib libucrt.lib /link /SUBSYSTEM:CONSOLE /OUT:"D:\Anshul_Projects\Touchdraw\touchdraw\build-app\%OUTNAME%"
)
