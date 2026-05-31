$cl = "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\MSVC\14.51.36231\bin\HostARM64\arm64\cl.exe"
$wdk = "C:\Program Files (x86)\Windows Kits\10"
$msvc = "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\MSVC\14.51.36231"
$ver = "10.0.26100.0"

# Sync files
Copy-Item "\\mac\Home\Documents\FileSentinel\driver\*" "C:\FileSentinel\driver\" -Force -Recurse
Copy-Item "\\mac\Home\Documents\FileSentinel\um\*" "C:\FileSentinel\um\" -Force -Recurse

# ============================================================
# 1. Build USERMODE service
# ============================================================
Write-Host "=== Building sentinel_svc.exe (usermode) ==="
$um_inc = "/I`"C:\FileSentinel\driver`" /I`"$wdk\Include\$ver\um`" /I`"$wdk\Include\$ver\shared`" /I`"$wdk\Include\$ver\ucrt`" /I`"$msvc\include`""
$um_lib = "/LIBPATH:`"$wdk\Lib\$ver\um\arm64`" /LIBPATH:`"$msvc\lib\arm64`" /LIBPATH:`"$wdk\Lib\$ver\ucrt\arm64`""
cmd /c "`"$cl`" /nologo /W3 /DUNICODE /D_UNICODE $um_inc C:\FileSentinel\um\service.c /Fe:C:\FileSentinel\sentinel_svc.exe /link $um_lib"
if ($LASTEXITCODE -eq 0) { Write-Host "OK: sentinel_svc.exe`n" } else { Write-Host "FAILED`n"; exit 1 }

# ============================================================
# 2. Build KERNEL driver
#    NO ucrt — wdm.h conflicts with it.
#    MSVC include has its own <string.h> etc for kernel.
# ============================================================
Write-Host "=== Building sentinel.sys (kernel minifilter) ==="
$km_inc = "/I`"C:\FileSentinel\driver`" /I`"$wdk\Include\$ver\ucrt`" /I`"$wdk\Include\$ver\shared`" /I`"$wdk\Include\$ver\km`" /I`"$msvc\include`""
$km_lib = "/LIBPATH:`"$wdk\Lib\$ver\km\arm64`" /LIBPATH:`"$msvc\lib\arm64`" fltMgr.lib ntoskrnl.lib hal.lib bufferoverflowfastfailk.lib"

cmd /c "`"$cl`" /nologo /W3 /kernel /D_ARM64_ $km_inc C:\FileSentinel\driver\driver.c C:\FileSentinel\driver\callbacks.c /Fe:C:\FileSentinel\sentinel.sys /link /SUBSYSTEM:NATIVE /DRIVER:WDM /ENTRY:DriverEntry $km_lib"
if ($LASTEXITCODE -eq 0) { Write-Host "OK: sentinel.sys`n" } else { Write-Host "FAILED`n"; exit 1 }

Write-Host "=== Build complete ==="
Get-ChildItem C:\FileSentinel\*.exe, C:\FileSentinel\*.sys | Select Name, Length
