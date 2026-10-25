@echo off
setlocal enabledelayedexpansion

echo ====================================================================
echo  Building CLR 4.x (.NET Framework 4.8) Native Host Suite (ARM64)
echo ====================================================================

:: 1. Check if cl.exe is available in current environment
where cl.exe >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [*] Attempting to initialize MSVC build environment...
    if exist "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsarm64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsarm64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsarm64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsarm64.bat"
    ) else (
        echo [ERROR] Could not find Visual Studio vcvars environment script.
        exit /b 1
    )
)

:: 2. Locate .NET Framework 4.8 C# Compiler (csc.exe)
set "CSC_EXE="
if exist "C:\Windows\Microsoft.NET\FrameworkArm64\v4.0.30319\csc.exe" (
    set "CSC_EXE=C:\Windows\Microsoft.NET\FrameworkArm64\v4.0.30319\csc.exe"
) else if exist "C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe" (
    set "CSC_EXE=C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
)

if "%CSC_EXE%"=="" (
    echo [ERROR] Could not locate .NET Framework 4.x csc.exe compiler.
    exit /b 2
)

:: 3. Compile ManagedNet4App.dll
echo [*] Compiling Managed .NET Framework 4.x Assembly (ManagedNet4App.dll)...
"%CSC_EXE%" /nologo /target:library /out:ManagedNet4App.dll ManagedNet4App\ManagedNet4App.cs
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile ManagedNet4App.dll
    exit /b %ERRORLEVEL%
)

:: 4. Compile Clr4Loader.exe
echo [*] Compiling Native C++ CLR 4 Host Loader (Clr4Loader.exe)...
cl /nologo /EHsc /std:c++20 Clr4Loader.cpp mscoree.lib user32.lib /Fe:Clr4Loader.exe
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile Clr4Loader.exe
    exit /b %ERRORLEVEL%
)

echo.
echo ====================================================================
echo  Build Successful! Running Clr4Loader.exe...
echo ====================================================================
Clr4Loader.exe ManagedNet4App.dll
