# FileSentinel — File Monitor/Blocker

Minifilter driver + usermode service for real-time file operation monitoring and blocking.

## Architecture

```
Usermode (sentinel_svc.exe)          Kernel (sentinel.sys)
┌───────────────────┐                ┌──────────────────────┐
│  Rule engine      │◄───Filter─────►│  Minifilter driver   │
│  - allow/deny     │    Port        │  - PreCreate         │
│  - logging        │  (named pipe)  │  - PreWrite          │
│  - config reload  │                │  - PreSetInfo         │
└───────────────────┘                └──────────────────────┘
                                            │
                                     FltMgr → NTFS → Disk
```

**Flow:**
1. App calls `CreateFile("C:\Blocked\test.txt", GENERIC_WRITE, ...)`
2. NTFS receives the IRP
3. FltMgr calls our `PreCreate` callback
4. Driver sends `{type=FileCreate, pid=1234, path="C:\Blocked\test.txt"}` to usermode
5. Usermode checks rules → replies `Deny`
6. Driver returns `STATUS_ACCESS_DENIED` to the calling app

## Files

```
driver/
  sentinel.h       — shared types (messages, verdicts, port name)
  driver.c         — DriverEntry, FltMgr registration, comm port
  callbacks.c      — PreCreate/PreWrite/PreSetInfo callbacks
  sentinel.inf     — driver installation INF
um/
  service.c        — usermode service (rule engine + logging)
```

## Build

### Driver (Visual Studio + WDK)

```
1. Open VS → Create "Kernel Mode Driver, Empty" project
2. Add driver/*.c and driver/*.h
3. Project Properties → Driver Type = "File System Minifilter Driver"
4. Add `fltMgr.lib` to Linker → Input → Additional Dependencies
5. Build (ARM64 or AMD64)
```

Or with MSBuild:
```cmd
msbuild sentinel.vcxproj /p:Configuration=Release /p:Platform=arm64
```

### Usermode service

```cmd
cl um\service.c /Fe:sentinel_svc.exe /I driver
```

Requires: `fltlib.lib` (linked automatically with `#pragma comment(lib, ...)`)

## Install

```cmd
:: Copy driver to system32\drivers
copy sentinel.sys C:\Windows\System32\drivers\

:: Install via rundll32 (or right-click INF → Install)
rundll32.exe setupapi.dll,InstallHinfSection DefaultInstall 132 .\sentinel.inf

:: Load driver
sc create sentinel type= filesys binPath= C:\Windows\System32\drivers\sentinel.sys
sc start sentinel
```

## Run

```cmd
:: Start the usermode service
sentinel_svc.exe
```

Expected output:
```
FileSentinel service starting...
Connected to FileSentinel driver.
Monitoring file operations. Blocked paths:
  \Blocked\
  \ReadOnly\

[ALLOW] PID=4820 CREATE C:\Users\test\Documents\readme.txt
[BLOCK] PID=4820 CREATE C:\Blocked\malware.exe
```

## Configuration

Edit `g_BlockedPaths[]` in `service.c` to add blocked path patterns:

```c
static const WCHAR *g_BlockedPaths[] = {
    L"\\Blocked\\",
    L"\\ReadOnly\\",
    L"\\Windows\\System32\\hackers\\",   // custom
};
```

For production — load rules from config file or registry.

## Uninstall

```cmd
sc stop sentinel
sc delete sentinel
rundll32.exe setupapi.dll,InstallHinfSection DefaultUninstall 132 .\sentinel.inf
```

## Testing with WinDbg

```
kd> !drvobj \Driver\sentinel
kd> bp sentinel!PreCreate
kd> ba w1 sentinel!g_ClientPort
```

## Notes

- Altitude `327000` — below Defender (328000), above generic anti-virus
- Only one usermode client at a time (connection refused if already connected)
- If usermode client disconnects — driver allows all operations (fail-open)
- `FltSendMessage` is synchronous — blocks until usermode replies (consider timeout)
- For production: add async queue + thread pool to avoid blocking I/O path
