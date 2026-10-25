@echo off
setlocal enabledelayedexpansion

echo ====================================================================
echo   Building NativeAOT Manual Loader Test Suite (ARM64)
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
echo [*] Publishing NativeAOT Shared Library (AotLib.dll)...
dotnet publish AotLib\AotLib.csproj -c Release -r win-arm64 -p:PublishAot=true
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to publish AotLib.dll
    exit /b %ERRORLEVEL%
)
if exist "AotLib\bin\arm64\Release\net10.0\win-arm64\publish\AotLib.dll" (
    copy /y "AotLib\bin\arm64\Release\net10.0\win-arm64\publish\AotLib.dll" . >nul
    copy /y "AotLib\bin\arm64\Release\net10.0\win-arm64\publish\AotLib.pdb" . >nul
) else (
    copy /y "AotLib\bin\Release\net10.0\win-arm64\publish\AotLib.dll" . >nul
    copy /y "AotLib\bin\Release\net10.0\win-arm64\publish\AotLib.pdb" . >nul
)

echo [*] Compiling NativeAotLoader.exe (Manual PE Loader)...
cl /nologo /EHsc NativeAotLoader.cpp user32.lib /Fe:NativeAotLoader.exe
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile NativeAotLoader.exe
    exit /b %ERRORLEVEL%
)

echo.
echo ====================================================================
echo   Running NativeAotLoader.exe AotLib.dll:
echo ====================================================================
NativeAotLoader.exe AotLib.dll
exit /b %ERRORLEVEL%
