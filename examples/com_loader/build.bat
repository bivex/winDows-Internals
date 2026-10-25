@echo off
setlocal enabledelayedexpansion

echo ====================================================================
echo  Building Native COM and Reg-Free COM Loader Test Suite (ARM64)
echo ====================================================================

:: 1. Check if cl.exe is available in current environment
where cl.exe >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [*] Attempting to initialize MSVC build environment...
    if exist "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsarm64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsarm64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsarm64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsarm64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else (
        echo [ERROR] Could not find Visual Studio vcvars environment script.
        exit /b 1
    )
)

:: 2. Compile ComComponent.dll
echo [*] Compiling Native In-Process COM Component (ComComponent.dll)...
cl /nologo /LD /EHsc /std:c++20 ComComponent\ComComponent.cpp user32.lib /Fe:ComComponent.dll
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile ComComponent.dll
    exit /b %ERRORLEVEL%
)

echo [*] Compiling Native COM Loader and Reg-Free Host (ComLoader.exe)...
cl /nologo /EHsc /std:c++20 ComLoader.cpp user32.lib ole32.lib /Fe:ComLoader.exe
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile ComLoader.exe
    exit /b %ERRORLEVEL%
)

echo.
echo ====================================================================
echo  Echo Build Successful! Running ComLoader.exe...
echo ====================================================================
ComLoader.exe ComComponent.dll ComComponent.manifest
