@echo off
:: ──────────────────────────────────────────────────────────────────────────────
:: build.cmd — 1-Click build for UAF PoC on Windows (ARM64 native)
:: Requires: Visual Studio 2022 with:
::   - VC++ ARM64 build tools
::   - LLVM/Clang tools for Windows (for ASan on ARM64)
:: ──────────────────────────────────────────────────────────────────────────────

setlocal enabledelayedexpansion

:: ── Detect VS via vswhere ─────────────────────────────────────────────────────
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Install Visual Studio 2022.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (
    `"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath`
) do set "VS_PATH=%%i"

if "%VS_PATH%"=="" (
    echo [ERROR] No VS2022 ARM64 toolchain found. Install "MSVC v143 ARM64 build tools".
    exit /b 1
)
echo [+] Visual Studio : %VS_PATH%

:: ── Always target native ARM64 ────────────────────────────────────────────────
:: Use arm64 host + arm64 target to avoid WOW64 / x86 emulation layer issues.
:: vcvarsall: arm64        = ARM64 host → ARM64 target (fully native)
::            amd64_arm64  = x64  host → ARM64 target (cross-compile)
set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat"
echo [+] Initializing ARM64 native toolchain...
call "%VCVARS%" arm64
if %ERRORLEVEL% neq 0 (
    echo [WARN] arm64 host not available, falling back to amd64_arm64 cross...
    call "%VCVARS%" amd64_arm64
)

:: ── Detect clang-cl for ASan (MSVC ASan broken on ARM64 in some VS versions) ──
set "CLANG_CL="
for /f "delims=" %%i in ('where clang-cl 2^>nul') do (
    if "!CLANG_CL!"=="" set "CLANG_CL=%%i"
)

if "!CLANG_CL!"=="" (
    :: Also check inside VS LLVM bundle
    set "VS_CLANG=%VS_PATH%\VC\Tools\Llvm\ARM64\bin\clang-cl.exe"
    if exist "!VS_CLANG!" set "CLANG_CL=!VS_CLANG!"
)

:: ── Build raw target (MSVC cl) ────────────────────────────────────────────────
set "BUILD_RAW=build_arm64_raw"
echo.
echo [+] Configuring uaf_poc_raw (MSVC cl, no ASan)...
cmake -B "%BUILD_RAW%" -S . ^
    -G "Ninja" ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_C_COMPILER=cl ^
    -DCMAKE_SYSTEM_NAME=Windows ^
    -DCMAKE_SYSTEM_PROCESSOR=ARM64

cmake --build "%BUILD_RAW%" --target uaf_poc_raw
if %ERRORLEVEL% neq 0 (echo [ERROR] uaf_poc_raw build failed & exit /b 1)
echo [OK] uaf_poc_raw.exe built.

:: ── Build ASan target ─────────────────────────────────────────────────────────
set "BUILD_ASAN=build_arm64_asan"
echo.
if not "!CLANG_CL!"=="" (
    echo [+] clang-cl found: !CLANG_CL!
    echo [+] Configuring uaf_poc_asan (clang-cl + ASan, native ARM64)...
    cmake -B "%BUILD_ASAN%" -S . ^
        -G "Ninja" ^
        -DCMAKE_BUILD_TYPE=Debug ^
        -DCMAKE_C_COMPILER="!CLANG_CL!" ^
        -DCMAKE_C_FLAGS="--target=arm64-pc-windows-msvc" ^
        -DCMAKE_SYSTEM_NAME=Windows ^
        -DCMAKE_SYSTEM_PROCESSOR=ARM64
) else (
    echo [WARN] clang-cl not found — using MSVC cl for ASan target.
    echo [WARN] If you see WOW64 / 0xC000026F errors, install LLVM for VS2022.
    echo [+] Configuring uaf_poc_asan (MSVC cl + /fsanitize=address)...
    cmake -B "%BUILD_ASAN%" -S . ^
        -G "Ninja" ^
        -DCMAKE_BUILD_TYPE=Debug ^
        -DCMAKE_C_COMPILER=cl ^
        -DCMAKE_SYSTEM_NAME=Windows ^
        -DCMAKE_SYSTEM_PROCESSOR=ARM64
)

cmake --build "%BUILD_ASAN%" --target uaf_poc_asan
if %ERRORLEVEL% neq 0 (echo [ERROR] uaf_poc_asan build failed & exit /b 1)
echo [OK] uaf_poc_asan.exe built.

:: ── Run ───────────────────────────────────────────────────────────────────────
echo.
echo ======================================================
echo  [1] uaf_poc_raw — no instrumentation (UAF succeeds)
echo ======================================================
"%BUILD_RAW%\uaf_poc_raw.exe"

echo.
echo ======================================================
echo  [2] uaf_poc_asan — AddressSanitizer (UAF detected)
echo ======================================================
"%BUILD_ASAN%\uaf_poc_asan.exe"

endlocal
