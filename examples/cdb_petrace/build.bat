@echo off
setlocal enabledelayedexpansion

echo ====================================================================
echo  Building PE Path Tracer (petrace) DbgEng Extension (ARM64)
echo ====================================================================

:: 1. Initialize MSVC Environment
where cl.exe >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [*] Initializing MSVC ARM64 developer environment...
    if exist "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsarm64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsarm64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsarm64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsarm64.bat"
    ) else (
        echo [ERROR] Could not find Visual Studio environment.
        exit /b 1
    )
)

:: 2. Compile petrace.dll
echo [*] Compiling petrace.dll (DbgEng Extension for ARM64)...
cl /nologo /LD /EHsc /std:c++20 src\petrace.cpp src\pe_analyzer.cpp src\disasm_engine.cpp src\correlator.cpp src\execution_tracer.cpp dbgeng.lib /Fe:petrace.dll
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile petrace.dll
    exit /b %ERRORLEVEL%
)
:: 3. Compile ComComponent.dll (with ComWorkerRoutine) and CorrelatorMatrixRunner.exe
echo [*] Compiling ComComponent.dll and CorrelatorMatrixRunner.exe...
if not exist "ComComponent" mkdir ComComponent
if exist "C:\test_com_loader\ComComponent\ComComponent.h" copy /Y "C:\test_com_loader\ComComponent\ComComponent.*" "ComComponent\" >nul
cl /nologo /LD /EHsc /std:c++20 ComComponent\ComComponent.cpp user32.lib /Fe:ComComponent.dll
cl /nologo /EHsc /std:c++20 CorrelatorMatrixRunner.cpp user32.lib ole32.lib /Fe:CorrelatorMatrixRunner.exe

echo.
echo ====================================================================
echo  Build Successful! Testing petrace.dll in cdb.exe...
echo ====================================================================
setlocal disabledelayedexpansion
set "CDB_EXE=C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe"
if not exist "%CDB_EXE%" (
    echo [WARN] cdb.exe not found at standard path. Searching...
    for /f "delims=" %%i in ('where cdb.exe 2^>nul') do set "CDB_EXE=%%i"
)

if not exist "%CDB_EXE%" (
    echo [ERROR] Could not locate cdb.exe for ARM64.
    exit /b 2
)

echo [*] Test 1: Extension Help Verification (!pehelp)...
"%CDB_EXE%" -c ".load C:\test_petrace\petrace.dll; !pehelp; .unload C:\test_petrace\petrace.dll; q" calc.exe

echo.
echo [*] Test 2: PE Invocation Vectors Analysis on calc.exe (!pepaths)...
"%CDB_EXE%" -c ".load C:\test_petrace\petrace.dll; !pepaths calc; .unload C:\test_petrace\petrace.dll; q" calc.exe

echo.
echo [*] Test 3: Control Flow Graph (CFG) Reconstruction on calc (!pecfg)...
"%CDB_EXE%" -c ".load C:\test_petrace\petrace.dll; !pecfg calc!wWinMainCRTStartup; .unload C:\test_petrace\petrace.dll; q" calc.exe

echo.
echo [*] Test 4: Dynamic Level A -> Level C Execution Correlation on ComComponent.dll...
"%CDB_EXE%" -c "sxe ld:ComComponent; g; .load C:\test_petrace\petrace.dll; !petrace ComComponent; g; !pechains; !pesummary; !peexport C:\test_petrace\com_trace.jsonl; .unload C:\test_petrace\petrace.dll; q" regsvr32.exe /s C:\test_com_loader\ComComponent.dll

echo.
echo [*] Test 5: Full 5-Scenario Matrix Execution on CorrelatorMatrixRunner.exe (!petrace / !pechains)...
"%CDB_EXE%" -c "sxe ld:ComComponent; g; .load C:\test_petrace\petrace.dll; !petrace ComComponent; g; !pechains; !pesummary; !peexport C:\test_petrace\matrix_trace.jsonl; .unload C:\test_petrace\petrace.dll; q" CorrelatorMatrixRunner.exe ComComponent.dll
