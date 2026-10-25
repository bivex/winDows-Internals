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

:: 2. Build capstone static library if missing
if not exist "C:\test_petrace\capstone\build\capstone.lib" (
    echo [*] Building capstone static library...
    cd /d C:\test_petrace\capstone
    cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCAPSTONE_BUILD_CSTOOL=OFF -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_ARM64_SUPPORT=ON -DCAPSTONE_X86_SUPPORT=ON
    cmake --build build --config Release
    cd /d C:\test_petrace
)

:: 3. Compile petrace.dll (linked against capstone)
echo [*] Compiling petrace.dll (DbgEng Extension for ARM64, capstone decoder)...
cl /nologo /LD /EHsc /std:c++20 /I C:\test_petrace\capstone\include src\petrace.cpp src\pe_analyzer.cpp src\disasm_engine.cpp src\correlator.cpp src\event_callbacks.cpp src\execution_tracer.cpp dbgeng.lib C:\test_petrace\capstone\build\capstone.lib /Fe:petrace.dll
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile petrace.dll
    exit /b %ERRORLEVEL%
)

:: 4. Compile ComComponent.dll (with ComWorkerRoutine) and CorrelatorMatrixRunner.exe
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

:: NOTE: petrace registers an engine event sink (IDebugEventCallbacks), so the
:: extension is intentionally NOT unloaded mid-session (cdb exits with 'q').

echo [*] Test 1: Extension Help Verification (!pehelp)...
"%CDB_EXE%" -c ".load C:\test_petrace\petrace.dll; !pehelp; q" calc.exe

echo.
echo [*] Test 2: Capstone CFG Reconstruction (!pecfg)...
"%CDB_EXE%" -c ".load C:\test_petrace\petrace.dll; !pecfg calc!wWinMainCRTStartup; q" calc.exe

echo.
echo [*] Test 3: Full 6-Scenario Matrix with Level A/B/C correlation (incl. LOAD_LIBRARY_AS_IMAGE_RESOURCE)...
"%CDB_EXE%" -c "sxe ld:ComComponent; g; .load C:\test_petrace\petrace.dll; !petrace ComComponent; g; !pechains; !pesummary; !peexport C:\test_petrace\matrix_trace.jsonl; q" CorrelatorMatrixRunner.exe ComComponent.dll
