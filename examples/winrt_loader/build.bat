@echo off
setlocal enabledelayedexpansion

echo ====================================================================
echo   Building C++/WinRT Manual Loader Test Suite (ARM64)
echo ====================================================================

:: 1. Check if cl.exe is available in current environment
where cl.exe >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [*] Initializing MSVC build environment...
    if exist "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsarm64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsarm64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else (
        echo [ERROR] Could not find Visual Studio vcvars environment script.
        exit /b 1
    )
)

echo [*] Compiling C++/WinRT In-Process Component (WinRtComponent.dll)...
cl /nologo /LD /EHsc /std:c++20 WinRtComponent\WinRtComponent.cpp windowsapp.lib user32.lib /Fe:WinRtComponent.dll
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile WinRtComponent.dll
    exit /b %ERRORLEVEL%
)

echo [*] Compiling WinRtLoader.exe (Manual WinRT PE Loader)...
cl /nologo /EHsc /std:c++20 WinRtLoader.cpp windowsapp.lib user32.lib ole32.lib /Fe:WinRtLoader.exe
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile WinRtLoader.exe
    exit /b %ERRORLEVEL%
)

echo.
echo ====================================================================
echo   Running WinRtLoader.exe WinRtComponent.dll:
echo ====================================================================
WinRtLoader.exe WinRtComponent.dll
exit /b %ERRORLEVEL%
