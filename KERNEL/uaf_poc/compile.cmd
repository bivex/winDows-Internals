@echo off
:: compile.cmd — Direct cl.exe build, no CMake, no VS project
:: Run from Developer Command Prompt for VS 2022 (ARM64 or x64)

cl /nologo ^
   /Zi /Od /GS- /W3 ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /Fe:uaf_poc_raw.exe ^
   /Fd:uaf_poc_raw.pdb ^
   uaf_poc.c

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo.
echo [OK] uaf_poc_raw.exe + uaf_poc_raw.pdb — ready for WinDbg.
