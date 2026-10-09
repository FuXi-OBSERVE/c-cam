@echo off
setlocal
chcp 65001 >nul

rem ---- locate w64devkit ----
set "TOOLCHAIN=D:\toolchain\w64devkit\bin"
if exist "%TOOLCHAIN%\g++.exe" goto :got_toolchain
set "TOOLCHAIN=C:\w64devkit\bin"
if exist "%TOOLCHAIN%\g++.exe" goto :got_toolchain
set "TOOLCHAIN=D:\w64devkit\bin"
if exist "%TOOLCHAIN%\g++.exe" goto :got_toolchain

rem fallback: g++ already in PATH
where g++ >nul 2>nul
if not errorlevel 1 goto :no_path_set
echo [ERROR] w64devkit not found, edit build.bat to set TOOLCHAIN
exit /b 1

:got_toolchain
set "PATH=%TOOLCHAIN%;%PATH%"

:no_path_set
cd /d "%~dp0"

rem ---- kill leftover processes holding the exe ----
taskkill /f /im trayclock.exe >nul 2>nul
taskkill /f /im trayclock-watchdog.exe >nul 2>nul
ping -n 2 127.0.0.1 >nul

echo === building trayclock.exe ===
g++ -std=c++17 -O2 -Wall -municode -mwindows -static ^
  src\main.cpp ^
  src\settings.cpp ^
  src\util.cpp ^
  src\capture.cpp ^
  src\clock.cpp ^
  src\timeline.cpp ^
  src\watchdog.cpp ^
  src\service.cpp ^
  -o trayclock.exe ^
  -lgdiplus -lgdi32 -luser32 -lshell32 -lshlwapi -lcomdlg32 ^
  -lole32 -loleaut32 -luuid -lstrmiids -ldwmapi -ladvapi32 ^
  -lwtsapi32 -luserenv -lcomctl32

if errorlevel 1 (
  echo.
  echo === BUILD FAILED ===
  exit /b 1
)
echo.
echo === BUILD OK: %~dp0trayclock.exe ===