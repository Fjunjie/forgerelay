@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem =====================================================================
rem  analyze_with_nuwa.bat - ForgeRelay StatiCode^(Nuwa^) static analysis
rem
rem  Uses only three Nuwa commands:
rem    1) nuw-config   - configure the compiler for capture
rem    2) nuw-build    - run the native build and capture IR
rem    3) nuw-analyze  - run the static analysis on the IR
rem
rem  Usage:
rem    scripts\analyze_with_nuwa.bat              rem clang, 8 jobs
rem    scripts\analyze_with_nuwa.bat gcc 8        rem gcc, 8 jobs
rem
rem  Requirements on PATH: nuw-config/nuw-build/nuw-analyze, cmake, ninja,
rem  and clang/clang++ (default) or gcc/g++.
rem  Outputs: build dir %PROJECT_DIR%\build-nuwa, IR %PROJECT_DIR%\nuwa-ir,
rem  analysis results in <IR>\output\.
rem =====================================================================

set "COMPILER=%~1"
if "%COMPILER%"=="" set "COMPILER=clang"
set "JOBS=%~2"
if "%JOBS%"=="" set "JOBS=8"

for %%I in ("%~dp0..") do set "PROJECT_DIR=%%~fI"
set "BUILD_DIR=%PROJECT_DIR%\build-nuwa"
set "IR_DIR=%PROJECT_DIR%\nuwa-ir"

if /I "%COMPILER%"=="clang" (
    set "CC=clang"
    set "CXX=clang++"
) else if /I "%COMPILER%"=="gcc" (
    set "CC=gcc"
    set "CXX=g++"
) else (
    echo [ERROR] compiler must be "clang" or "gcc", got "%COMPILER%"
    exit /b 1
)

echo ======================================================================
echo  ForgeRelay Nuwa analysis: compiler=%COMPILER% jobs=%JOBS%
echo  project: %PROJECT_DIR%
echo ======================================================================

echo [1/4] nuw-config --%COMPILER%
nuw-config --%COMPILER%
if not !errorlevel! equ 0 (
    echo [ERROR] nuw-config failed
    exit /b 2
)
rem Preempt the most common capture failure: cmake calling cc/c++ aliases.
nuw-config -co cc -p %CC% -tm >nul 2>&1
nuw-config -co c++ -p %CXX% -tm >nul 2>&1
nuw-config -lc

echo [2/4] cmake configure
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if exist "%IR_DIR%" rmdir /s /q "%IR_DIR%"
cmake -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    -DFR_BUILD_TESTS=OFF ^
    -DCMAKE_C_COMPILER=%CC% -DCMAKE_CXX_COMPILER=%CXX%
if not !errorlevel! equ 0 (
    echo [ERROR] cmake configure failed
    exit /b 3
)

echo [3/4] nuw-build: compile and capture IR
pushd "%BUILD_DIR%"
nuw-build --dir "%IR_DIR%" cmake --build .
set "RC=!errorlevel!"
popd
if not "%RC%"=="0" (
    echo [ERROR] nuw-build failed rc=%RC%
    exit /b 4
)
if not exist "%IR_DIR%\compile\nuwa.db" (
    echo [ERROR] %IR_DIR%\compile\nuwa.db missing - nothing was captured
    exit /b 4
)
findstr /C:"The nuw-build completed successfully" "%IR_DIR%\build-log.txt" >nul 2>&1
if not !errorlevel! equ 0 (
    echo [WARN] success marker not found in %IR_DIR%\build-log.txt
)
findstr /C:"No source has been compiled" "%IR_DIR%\build-log.txt" >nul 2>&1
if !errorlevel! equ 0 (
    echo [ERROR] compiler interception failed: "No source has been compiled"
    echo   Hint: configure the compiler names cmake actually used, e.g.
    echo   nuw-config -co cc -p %CC% -tm   and/or full paths, then rerun.
    exit /b 4
)

echo [4/4] nuw-analyze
nuw-analyze --dir "%IR_DIR%" -j %JOBS%
set "RC=!errorlevel!"
if not "%RC%"=="0" (
    echo [ERROR] nuw-analyze failed rc=%RC%
    exit /b 5
)

echo ======================================================================
echo  Analysis finished. Results: %IR_DIR%\output\
echo ======================================================================
exit /b 0
