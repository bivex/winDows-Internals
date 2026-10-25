@echo off
setlocal enabledelayedexpansion

echo ====================================================================
echo   Building C# / CoreCLR Manual Host and Loader Test Suite (ARM64)
echo ====================================================================

:: 1. Check if dotnet is available
where dotnet >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] dotnet CLI not found in PATH.
    exit /b 1
)

:: 2. Check if cl.exe is available in current environment
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

echo [*] Building Pure C# Assembly (SharpApp.dll)...
dotnet build SharpApp\SharpApp.csproj -c Release
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to build SharpApp.dll
    exit /b %ERRORLEVEL%
)

echo [*] Compiling CSharpLoader.exe (Native C++ CoreCLR Host)...
cl /nologo /EHsc CSharpLoader.cpp user32.lib /Fe:CSharpLoader.exe
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile CSharpLoader.exe
    exit /b %ERRORLEVEL%
)
echo ====================================================================
echo   Running CSharpLoader.exe SharpApp.dll:
echo ====================================================================
if exist "SharpApp\bin\arm64\Release\net10.0\SharpApp.dll" (
    set "TARGET_DLL=SharpApp\bin\arm64\Release\net10.0\SharpApp.dll"
) else (
    set "TARGET_DLL=SharpApp\bin\Release\net10.0\SharpApp.dll"
)
CSharpLoader.exe !TARGET_DLL!
exit /b %ERRORLEVEL%
