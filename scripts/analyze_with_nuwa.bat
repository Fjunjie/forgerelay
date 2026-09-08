@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem =====================================================================
rem  analyze_with_nuwa.bat - ForgeRelay one-shot Nuwa (staticode) analysis
rem
rem  Flow (per nuw-auto-analyze-skill / SKILL.md):
rem    [1] environment check   : nuwa tools + cmake/ninja + compiler
rem    [2] nuw-config          : configure compiler (clang|gcc) + cc/c++ aliases
rem    [3] cmake configure     : plain cmake (NOT nuw-build), Ninja, Release
rem    [4] nuw-build           : capture IR  (nuw-build --dir <ir> ninja)
rem        -> verify build-log / nuwa.db / nuw-dbtool list, 1 auto retry
rem    [5] nuw-analyze         : static analysis (analysis side)
rem    [6] nuw-commit-error    : optional defect upload (opt-in)
rem
rem  Usage:
rem    scripts\analyze_with_nuwa.bat            & rem default compiler: clang
rem    scripts\analyze_with_nuwa.bat gcc        & rem use gcc + g++
rem    scripts\analyze_with_nuwa.bat clang commit   & rem also upload defects
rem
rem  Environment overrides:
rem    NUWA_PATH      dir containing nuw-*.exe (prepended to PATH)
rem    JOBS           parallel jobs for build/analyze   (default: 8)
rem    BUILD_DIR      cmake build dir                   (default: <proj>\build-nuwa)
rem    IR_DIR         nuwa IR dir                       (default: <proj>\nuwa-ir)
rem    FR_BUILD_TESTS build tests too                   (default: OFF; keeps GTest
rem                                                     code out of the IR)
rem    CMAKE_EXTRA    extra cmake configure flags
rem
rem  Exit codes: 0 ok | 1 env | 2 compiler config | 3 cmake | 4 build |
rem              5 IR verify | 6 analyze
rem
rem  NOTE: nuw-build takes --dir (NOT --ir); its log is build-log.txt
rem        (hyphen). Both are per SKILL.md field notes.
rem =====================================================================

rem ------------------------- arguments & defaults ----------------------
set "COMPILER=%~1"
if "%COMPILER%"=="" set "COMPILER=clang"
set "DO_COMMIT=%~2"

if /I not "%COMPILER%"=="clang" if /I not "%COMPILER%"=="gcc" (
    echo [ERROR] first argument must be "clang" or "gcc", got "%COMPILER%"
    exit /b 1
)

for %%I in ("%~dp0..") do set "PROJECT_DIR=%%~fI"
if not "%NUWA_PATH%"=="" set "PATH=%NUWA_PATH%;%PATH%"
if "%JOBS%"=="" set "JOBS=8"
if "%BUILD_DIR%"=="" set "BUILD_DIR=%PROJECT_DIR%\build-nuwa"
if "%IR_DIR%"=="" set "IR_DIR=%PROJECT_DIR%\nuwa-ir"
if "%FR_BUILD_TESTS%"=="" set "FR_BUILD_TESTS=OFF"

if /I "%COMPILER%"=="clang" (
    set "CC_BIN=clang"
    set "CXX_BIN=clang++"
    set "ALIAS_CC=cc"
    set "ALIAS_CXX=c++"
) else (
    set "CC_BIN=gcc"
    set "CXX_BIN=g++"
    set "ALIAS_CC=cc"
    set "ALIAS_CXX=c++"
)

echo ======================================================================
echo  ForgeRelay Nuwa analysis
echo    project   : %PROJECT_DIR%
echo    compiler  : %COMPILER% (%CC_BIN% / %CXX_BIN%)
echo    build dir : %BUILD_DIR%
echo    IR dir    : %IR_DIR%
echo    jobs      : %JOBS%   tests: %FR_BUILD_TESTS%   commit: %DO_COMMIT%
echo ======================================================================

rem ------------------------- [1] environment check ---------------------
echo [STAGE 1] environment check
set "MISSING="
for %%T in (nuw-config nuw-build nuw-dbtool nuw-analyze) do (
    where %%T >nul 2>nul || set "MISSING=!MISSING! %%T"
)
if /I "%DO_COMMIT%"=="commit" where nuw-commit-error >nul 2>nul || set "MISSING=!MISSING! nuw-commit-error"
for %%T in (cmake ninja) do (
    where %%T >nul 2>nul || set "MISSING=!MISSING! %%T"
)
for %%T in (%CC_BIN% %CXX_BIN%) do (
    where %%T >nul 2>nul || set "MISSING=!MISSING! %%T"
)
if not "%MISSING%"=="" (
    echo [ERROR] required tools not found on PATH:%MISSING%
    echo         put the nuwa toolchain dir in PATH or set NUWA_PATH,
    echo         and make sure cmake/ninja/%CC_BIN%/%CXX_BIN% are available.
    exit /b 1
)
echo   tools OK: nuwa toolchain + cmake + ninja + %CC_BIN%/%CXX_BIN%
where cmake | findstr /I "nuwa" >nul 2>nul && echo   [warn] cmake resolves under a NUWA dir, verify it is the real cmake.

rem ------------------------- [2] nuw-config ----------------------------
echo [STAGE 2] configure compiler with nuw-config
rem --clang/--gcc configure the pair (cc + cxx) at once; "already exists"
rem warnings are expected and ignorable (per SKILL.md).
nuw-config --%COMPILER%
if !errorlevel! neq 0 (
    echo [ERROR] nuw-config --%COMPILER% failed
    exit /b 2
)
rem Preempt the #1 capture failure: cmake using cc/c++ aliases.
nuw-config -co %ALIAS_CC% -p %CC_BIN% -tm
nuw-config -co %ALIAS_CXX% -p %CXX_BIN% -tm
echo   configured compilers:
nuw-config -lc

rem ------------------------- [3]+[4] configure & build -----------------
set "ATTEMPT=0"

:prepare_and_build
set /a ATTEMPT+=1
echo [STAGE 3] cmake configure (attempt !ATTEMPT!)
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if exist "%IR_DIR%" rmdir /s /q "%IR_DIR%"
cmake -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    -DFR_BUILD_TESTS=%FR_BUILD_TESTS% ^
    -DCMAKE_C_COMPILER=%CC_BIN% -DCMAKE_CXX_COMPILER=%CXX_BIN% ^
    %CMAKE_EXTRA%
if !errorlevel! neq 0 (
    echo [ERROR] cmake configure failed, see output above (exit 3)
    exit /b 3
)

echo [STAGE 4] nuw-build (capture IR, attempt !ATTEMPT!)
pushd "%BUILD_DIR%"
nuw-build --dir "%IR_DIR%" ninja -j%JOBS%
set "BUILD_RC=!errorlevel!"
popd
if not "%BUILD_RC%"=="0" (
    echo [ERROR] nuw-build returned %BUILD_RC% ^(exit 4^)
    exit /b 4
)

rem verify: success marker + interception check (log name per SKILL.md)
set "BUILD_LOG="
if exist "%IR_DIR%\build-log.txt"  set "BUILD_LOG=%IR_DIR%\build-log.txt"
if "%BUILD_LOG%"=="" if exist "%IR_DIR%\build_log.txt" set "BUILD_LOG=%IR_DIR%\build_log.txt"
if "%BUILD_LOG%"=="" (
    echo [ERROR] nuw-build log not found under %IR_DIR% ^(exit 4^)
    exit /b 4
)
findstr /C:"The nuw-build completed successfully" "%BUILD_LOG%" >nul
set "SUCCESS_MARK=!errorlevel!"
findstr /C:"No source has been compiled" "%BUILD_LOG%" >nul
set "NO_CAPTURE=!errorlevel!"
if "%SUCCESS_MARK%"=="0" if not "%NO_CAPTURE%"=="0" goto build_ok

echo   [warn] capture suspicious (success marker: %SUCCESS_MARK%, no-capture: %NO_CAPTURE%)
if %ATTEMPT% lss 2 (
    echo   retrying once after clean: configuring cc/c++ aliases, wiping build+IR
    nuw-config -co %ALIAS_CC%  -p %CC_BIN%  -tm
    nuw-config -co %ALIAS_CXX% -p %CXX_BIN% -tm
    goto prepare_and_build
)
echo [ERROR] nuw-build still not capturing after retry ^(exit 4^)
echo   inspect "%BUILD_LOG%" and %IR_DIR%\capture.log;
echo   per skill, next fallback is nuw-buildless after 5 diagnosed attempts.
exit /b 4

:build_ok
echo   build log OK: "%BUILD_LOG%"

rem ------------------------- IR verification ---------------------------
echo [STAGE 4b] verify IR database
if not exist "%IR_DIR%\compile\nuwa.db" (
    echo [ERROR] %IR_DIR%\compile\nuwa.db not found ^(exit 5^)
    exit /b 5
)
nuw-dbtool --dir "%IR_DIR%" list > "%IR_DIR%\dbtool-list.txt" 2>&1
if !errorlevel! neq 0 (
    rem some versions take --ir instead of --dir (nuw-analyze-guide.md)
    nuw-dbtool --ir "%IR_DIR%" list > "%IR_DIR%\dbtool-list.txt" 2>&1
    if !errorlevel! neq 0 (
        echo [ERROR] nuw-dbtool list failed with both --dir and --ir ^(exit 5^)
        exit /b 5
    )
)
set "DB_ROWS=0"
for /f %%C in ('type "%IR_DIR%\dbtool-list.txt" ^| find /c /v ""') do set "DB_ROWS=%%C"
echo   nuw-dbtool list: %DB_ROWS% line(s)
findstr /I /C:"fr_" "%IR_DIR%\dbtool-list.txt" >nul && echo   project sources (fr_*.c) present in IR.
findstr /I /R "\.c$ \.cc$ \.cpp$" "%IR_DIR%\dbtool-list.txt" >nul
if !errorlevel! neq 0 (
    echo [ERROR] IR database contains no C/C++ source ^(exit 5^)
    type "%IR_DIR%\dbtool-list.txt"
    exit /b 5
)

rem ------------------------- [5] nuw-analyze ---------------------------
echo [STAGE 5] nuw-analyze (static analysis, this may take a while)
if exist "%IR_DIR%\analyze-console.log" del /q "%IR_DIR%\analyze-console.log"
nuw-analyze --dir "%IR_DIR%" -j%JOBS% > "%IR_DIR%\analyze-console.log" 2>&1
set "ANA_RC=!errorlevel!"
findstr /I /C:"Analysis summary" "%IR_DIR%\analyze-console.log" >nul
set "SUMMARY_MARK=!errorlevel!"
if not "%ANA_RC%"=="0" goto analyze_fail
if not "%SUMMARY_MARK%"=="0" goto analyze_fail

echo   analysis finished, summary lines:
findstr /I /R /C:"Total" /C:"Checker" /C:"Defects" /C:"Violations" /C:"Time Taken" /C:"Files Analyzed" /C:"Functions Analyzed" "%IR_DIR%\analyze-console.log"
echo   result files:
dir /b "%IR_DIR%\output\*.errors.xml" 2>nul
if !errorlevel! neq 0 echo   (no .errors.xml files found - check analyze-console.log)
echo   full log: %IR_DIR%\analyze-console.log
echo   results : %IR_DIR%\output\

rem ------------------------- [6] optional commit -----------------------
if /I not "%DO_COMMIT%"=="commit" (
    echo [STAGE 6] skipped ^(pass "commit" as 2nd argument to upload defects^)
    goto done
)
echo [STAGE 6] nuw-commit-error (upload defects)
nuw-commit-error --dir "%IR_DIR%"
if !errorlevel! neq 0 (
    echo [ERROR] nuw-commit-error failed; check network/auth ^(exit 6^)
    exit /b 6
)

:done
echo ======================================================================
echo  Nuwa analysis pipeline finished successfully.
echo  IR: %IR_DIR%   Results: %IR_DIR%\output\
echo ======================================================================
exit /b 0

:analyze_fail
echo [ERROR] nuw-analyze failed ^(rc=%ANA_RC%, summary marker=%SUMMARY_MARK%, exit 6^)
echo   log tail hints:
findstr /I /C:"error" /C:"failed" /C:"fatal" "%IR_DIR%\analyze-console.log" 2>nul | more +0
echo   full log: %IR_DIR%\analyze-console.log
echo   check IR integrity: nuw-dbtool --dir "%IR_DIR%" list
exit /b 6
