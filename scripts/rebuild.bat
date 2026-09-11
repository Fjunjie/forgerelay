@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem =====================================================================
rem  rebuild.bat - clean & rebuild a ForgeRelay CMake preset (Windows)
rem
rem  Usage:
rem    scripts\rebuild.bat                     rem clang-debug, build only
rem    scripts\rebuild.bat clang-debug test    rem build + ctest
rem    scripts\rebuild.bat gcc-debug test      rem need real gcc on PATH
rem    scripts\rebuild.bat clang-asan test     rem ASan/UBSan build
rem
rem  "Clean" removes build\<preset> entirely, so configure runs fresh
rem  (first run downloads GoogleTest/SQLite via FetchContent - needs net).
rem
rem  cmake resolution order (same as analyze_with_nuwa.bat):
rem    PATH -> CMAKE_DIR env var -> <project>\..\.tools\cmake-*\bin
rem =====================================================================

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=clang-debug"
set "RUN_TEST=%~2"

for %%I in ("%~dp0..") do set "PROJECT_DIR=%%~fI"
set "BUILD_DIR=%PROJECT_DIR%\build\%PRESET%"

rem ---- locate cmake ----------------------------------------------------
where cmake >nul 2>nul
if not !errorlevel! equ 0 (
    if defined CMAKE_DIR set "PATH=%CMAKE_DIR%;%PATH%"
)
where cmake >nul 2>nul
if not !errorlevel! equ 0 (
    for /d %%D in ("%PROJECT_DIR%\..\.tools\cmake-*") do (
        if exist "%%D\bin\cmake.exe" set "PATH=%%D\bin;%PATH%"
    )
)
where cmake >nul 2>nul
if not !errorlevel! equ 0 (
    echo [ERROR] cmake not found.
    echo   Install it, e.g. "winget install Kitware.CMake", or set CMAKE_DIR
    echo   to the directory containing cmake.exe, then rerun.
    exit /b 1
)

echo ======================================================================
echo  clean + rebuild: %PRESET%
echo  build dir: %BUILD_DIR%
echo ======================================================================

echo [1/3] clean
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"

echo [2/3] configure + build
cmake --preset "%PRESET%"
if not !errorlevel! equ 0 (
    echo [ERROR] cmake configure failed
    exit /b 1
)
cmake --build --preset "%PRESET%" -j
if not !errorlevel! equ 0 (
    echo [ERROR] build failed
    exit /b 1
)

if /I not "%RUN_TEST%"=="test" goto done

echo [3/3] ctest
ctest --preset "%PRESET%" --output-on-failure
if not !errorlevel! equ 0 (
    echo [ERROR] tests failed
    exit /b 1
)

:done
echo ======================================================================
echo  Rebuild finished: %PRESET%
echo ======================================================================
exit /b 0
