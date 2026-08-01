@echo off
:: compile_protected.cmd — Compiles with CFG and BTI / PAC enabled

cl /nologo ^
   /W3 /Zi /Od ^
   /guard:cf ^
   /guard:signret ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /Fe:check_mitigations_protected.exe ^
   /Fd:check_mitigations_protected.pdb ^
   check_mitigations.c ^
   /link /GUARD:CF

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Compilation with CFG and BTI/PAC failed.
    exit /b 1
)

echo.
echo [OK] Compiled protected binary: check_mitigations_protected.exe
