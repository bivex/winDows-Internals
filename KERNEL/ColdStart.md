# Cold Start

Quick runbook for bringing up a fresh kernel debugging session between the two Parallels ARM64 VMs on this Mac.

## Verified Working Setup

- Host: macOS on Apple Silicon
- Parallels: 20.2.2
- Target VM: `Windows 11 Pro (Target)`
- Debugger VM: `Windows 11 Pro (Debugger)`
- Transport: serial over Unix sockets via `socat`
- WinDbg path on Debugger:
  `C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\windbg.exe`

Important:
- Launch `WinDbg` as Administrator.
- Use `com1`, not `\\.\COM1`.
- Start `WinDbg` before rebooting the Target VM.

## Preconditions

Target VM must already have kernel debugging enabled:

```cmd
bcdedit /debug on
bcdedit /set testsigning on
bcdedit /dbgsettings serial debugport:1 baudrate:115200
```

Both Parallels VMs must have serial sockets configured:

- Target: `/tmp/kd.sock`
- Debugger: `/tmp/debugger.sock`

## Cold Start Steps

### 1. Start the Target VM

```bash
prlctl start "Windows 11 Pro (Target)"
```

### 2. Start the Debugger VM

```bash
prlctl start "Windows 11 Pro (Debugger)"
```

### 3. Wait for both sockets to appear on the host

```bash
ls -l /tmp/kd.sock /tmp/debugger.sock
```

Expected:
- both files exist
- both are Unix sockets

### 4. Start the relay on the macOS host

```bash
sudo /opt/homebrew/bin/socat UNIX-CLIENT:/tmp/kd.sock UNIX-CLIENT:/tmp/debugger.sock
```

Notes:
- keep this terminal open while debugging
- if you want it in the background:

```bash
sudo /opt/homebrew/bin/socat UNIX-CLIENT:/tmp/kd.sock UNIX-CLIENT:/tmp/debugger.sock &
```

### 5. On the Debugger VM, open WinDbg as Administrator

PowerShell:

```powershell
Start-Process -Verb RunAs `
  -FilePath 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\windbg.exe' `
  -ArgumentList @('-k','com:port=com1,baud=115200,reconnect')
```

If you prefer a plain command line inside an elevated shell:

```cmd
"C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\windbg.exe" -k com:port=com1,baud=115200,reconnect
```

Expected first state:

```text
Opened \\.\com1
Waiting to reconnect...
```

### 6. Reboot the Target VM

From the macOS host:

```bash
prlctl exec "Windows 11 Pro (Target)" --current-user cmd /c "shutdown /r /t 0"
```

### 7. Wait for WinDbg to attach

Expected output:

```text
Connected to Windows 10 26100 ARM 64-bit (AArch64) target at (...)
Kernel Debugger connection established.
```

Once connected:

```text
Ctrl+Break
```

forces a break into the target.

## Fast Validation

If GUI WinDbg is acting up, validate the link first with `kd.exe` on the Debugger VM:

```cmd
"C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe" -k com:port=com1,baud=115200
```

This was verified to connect successfully on this setup.

## Known Gotchas

- `windbg -k com:port=\\.\COM1,baud=115200,reconnect` is wrong on this setup and throws parameter errors.
- Running `WinDbg` without elevation can fail to open `COM1`.
- `socat` needs `sudo` because the Parallels-created sockets are owned by `root`.
- If `WinDbg` never reconnects, make sure the relay was started before rebooting the Target VM.
- If `socat` exits immediately, one of the socket files is missing.

## Useful Host Commands

Check VM state:

```bash
prlctl list -a
```

Check socket presence:

```bash
ls -l /tmp/kd.sock /tmp/debugger.sock
```

Check relay:

```bash
pgrep -af socat
```

Kill stale relay:

```bash
pkill -f "socat.*kd.sock.*debugger.sock"
```
