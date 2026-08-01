@echo off
:: ──────────────────────────────────────────────────────────────────────────────
:: build.cmd — 1-Click build for UAF PoC on Windows (ARM64 or x64)
:: Requires: Visual Studio 2022 with C/C++ and ASan components installed
:: ──────────────────────────────────────────────────────────────────────────────

setlocal enabledelayedexpansion

:: Detect VS installation via vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Install Visual Studio 2022.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)

if "%VS_PATH%"=="" (
    echo [ERROR] Visual Studio not found.
    exit /b 1
)

echo [+] Visual Studio: %VS_PATH%

:: ── Select architecture ───────────────────────────────────────────────────────
:: Change to x64 if not on ARM64 Windows
set "ARCH=ARM64"
if /I "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "ARCH=x64"
echo [+] Target arch: %ARCH%

:: ── Init VS environment ───────────────────────────────────────────────────────
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH%

:: ── Configure & Build ─────────────────────────────────────────────────────────
set "BUILD_DIR=build_windows_%ARCH%"

cmake -B "%BUILD_DIR%" -S . ^
    -G "Ninja" ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_C_COMPILER=cl

if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configure failed.
    exit /b %ERRORLEVEL%
)

cmake --build "%BUILD_DIR%" --parallel

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed.
    exit /b %ERRORLEVEL%
)

echo.
echo [OK] Build complete. Binaries in: %BUILD_DIR%\
echo.

:: ── Run raw (expect UAF to succeed silently) ──────────────────────────────────
echo ======================================================
echo  Running: uaf_poc_raw (no instrumentation)
echo ======================================================
"%BUILD_DIR%\uaf_poc_raw.exe"

echo.
:: ── Run ASan (expect UAF to be detected) ─────────────────────────────────────
echo ======================================================
echo  Running: uaf_poc_asan (AddressSanitizer)
echo ======================================================
"%BUILD_DIR%\uaf_poc_asan.exe"

endlocal
