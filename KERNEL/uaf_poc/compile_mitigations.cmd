@echo off
:: compile_mitigations.cmd — Compiles check_mitigations.c using MSVC cl.exe

cl /nologo ^
   /W3 /Zi /Od ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /Fe:check_mitigations.exe ^
   /Fd:check_mitigations.pdb ^
   check_mitigations.c

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Compilation failed.
    exit /b 1
)

echo.
echo [OK] Compiled: check_mitigations.exe
