@echo off
setlocal enabledelayedexpansion

echo ====================================================================
echo   Building C++/CLI Mixed-Mode (IJW) Loader Test Suite
echo ====================================================================

:: Check if cl.exe is available in current environment
where cl.exe >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [!] cl.exe not found in PATH.
    echo [*] Attempting to initialize MSVC build environment...
    
    if exist "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsarm64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsarm64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else (
        echo [ERROR] Could not find Visual Studio vcvars environment script.
        echo Please run this batch script from a Visual Studio Developer Command Prompt.
        exit /b 1
    )
)
echo [*] Compiling MixedLib.dll with cl /clr /LD...
cl /clr /LD MixedLib.cpp /Fe:MixedLib.dll
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile MixedLib.dll
    exit /b %ERRORLEVEL%
)

echo [*] Compiling MixedExe.exe with native cl.exe...
cl /EHsc MixedExe.cpp MixedLib.lib /Fe:MixedExe.exe
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile MixedExe.exe
    exit /b %ERRORLEVEL%
)

echo [*] Compiling CppCliLoader.exe (manual PE loader, no MixedLib.lib link)...
cl /EHsc CppCliLoader.cpp /Fe:CppCliLoader.exe
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile CppCliLoader.exe
    exit /b %ERRORLEVEL%
)

echo.
echo ====================================================================
echo   Build Successful! Running MixedExe.exe:
echo ====================================================================
MixedExe.exe

echo.
echo ====================================================================
echo   Running CppCliLoader.exe (manual mapping, no LoadLibrary):
echo ====================================================================
CppCliLoader.exe MixedLib.dll
exit /b %ERRORLEVEL%