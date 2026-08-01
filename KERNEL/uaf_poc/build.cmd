@echo off
:: ──────────────────────────────────────────────────────────────────────────────
:: build.cmd — Build uaf_poc_raw.exe for WinDbg debugging on Target VM
:: No ASan. Full debug symbols (/Zi /Od).
:: ──────────────────────────────────────────────────────────────────────────────

setlocal enabledelayedexpansion

:: ── Detect VS ─────────────────────────────────────────────────────────────────
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (
    `"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath`
) do set "VS_PATH=%%i"

if "%VS_PATH%"=="" (
    echo [ERROR] VS2022 ARM64 toolchain not found.
    exit /b 1
)

echo [+] VS : %VS_PATH%

:: ── Init native ARM64 toolchain ───────────────────────────────────────────────
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" arm64
if %ERRORLEVEL% neq 0 (
    echo [WARN] arm64 host unavailable, trying amd64_arm64...
    call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" amd64_arm64
)

:: ── Build ─────────────────────────────────────────────────────────────────────
set "BUILD_DIR=build_arm64_debug"

cmake -B "%BUILD_DIR%" -S . ^
    -G "Ninja" ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_C_COMPILER=cl ^
    -DCMAKE_SYSTEM_NAME=Windows ^
    -DCMAKE_SYSTEM_PROCESSOR=ARM64

cmake --build "%BUILD_DIR%" --target uaf_poc_raw

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo.
echo [OK] Done. Copy these to Target VM:
echo      %BUILD_DIR%\uaf_poc_raw.exe
echo      %BUILD_DIR%\uaf_poc_raw.pdb
echo.
echo WinDbg on debugger VM:
echo   .open -debuginfo <path>\uaf_poc_raw.pdb
echo   g   -- run until crash / bp
