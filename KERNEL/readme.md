# WinDbg Kernel Debugging — Two Parallels ARM64 VMs on Apple Silicon

Kernel debugging between two Windows 11 ARM64 VMs in Parallels Desktop on an Apple Silicon Mac.

---

## Environment

| | Value |
|---|---|
| **Host** | macOS (Apple Silicon) |
| **Hypervisor** | Parallels Desktop 20.2.2 |
| **Guest OS** | Windows 11 Pro ARM64 (26100) |
| **Debugger VM** | `Windows 11 Pro (Debugger)` |
| **Target VM** | `Windows 11 Pro (Target)` |
| **Transport** | Serial over Unix socket relay (socat) |

---

## Why not KDNET?

Parallels on Apple Silicon always emulates VirtIO legacy NICs (`VEN_1AF4 DEV_1000`).
Windows KDNET provider `kd_02_1af4.dll` does **not** support `DEV_1000` — zero packets are ever transmitted.
No workaround exists without changing the hypervisor.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  macOS Host                                                 │
│                                                             │
│  ┌──────────────────┐   socat   ┌──────────────────┐       │
│  │  Target VM       │  relay    │  Debugger VM     │       │
│  │                  │           │                  │       │
│  │  kdcom (kdcom.dll│           │  WinDbg          │       │
│  │  grabs COM1)     │           │  -k com:COM1     │       │
│  │       │          │           │       │          │       │
│  │  ARM PL011 UART  │           │  ARM PL011 UART  │       │
│  │       │          │           │       │          │       │
│  └───────┼──────────┘           └───────┼──────────┘       │
│          │                              │                   │
│   /tmp/kd.sock ◄──── socat ────► /tmp/debugger.sock        │
│   (Unix socket)                  (Unix socket)              │
└─────────────────────────────────────────────────────────────┘
```

**Data flow:**
- Target `kdcom` → ARM PL011 (COM1) → Parallels → `/tmp/kd.sock`
- `/tmp/kd.sock` ↔ `socat` ↔ `/tmp/debugger.sock`
- `/tmp/debugger.sock` → Parallels → ARM PL011 (COM1) → WinDbg on Debugger

---

## One-time Setup (already done)

### Target VM — bcdedit (run once as Administrator)

```cmd
bcdedit /debug on
bcdedit /set testsigning on
bcdedit /dbgsettings serial debugport:1 baudrate:115200
```

### Parallels serial port config

Both VMs have `EmulatedType=3` (Unix socket mode) configured in `config.pvs`:

**Target** → `/tmp/kd.sock` (server, `Remote=0 SocketMode=0`)
**Debugger** → `/tmp/debugger.sock` (server, `Remote=0 SocketMode=0`)

> Set via: `prlctl set "<VM>" --device-set serial0 --socket /tmp/<name>.sock`

---

## Starting a Debug Session

### Option A — script (recommended)

```bash
/Volumes/External/Code/double_parallels_windbg/START_DEBUG_SESSION.sh
```

For a detailed from-scratch checklist, see [ColdStart.md](/Volumes/External/Code/double_parallels_windbg/ColdStart.md).

### Option B — manual steps

```bash
# 1. Start Target VM (creates /tmp/kd.sock)
prlctl start "Windows 11 Pro (Target)"

# 2. Start Debugger VM (creates /tmp/debugger.sock)
prlctl start "Windows 11 Pro (Debugger)"

# 3. Wait for both sockets to appear
ls -la /tmp/kd.sock /tmp/debugger.sock

# 4. Start socat relay on macOS host
sudo socat UNIX-CLIENT:/tmp/kd.sock UNIX-CLIENT:/tmp/debugger.sock &

# 5. On Debugger VM — open WinDbg as Administrator
"C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\windbg.exe" -k com:port=com1,baud=115200,reconnect

# 6. Reboot Target VM
prlctl exec "Windows 11 Pro (Target)" cmd /c "shutdown /r /t 0"
```

### Expected WinDbg output on success

```
Opened \\.\COM1
Waiting to reconnect...
Connected to Windows 10 26100 ARM 64-bit (AArch64) target at (...)
Kernel Debugger connection established.
Kernel base = 0xfffff800`6bc00000
nt!DebugService2+0x8:
fffff800`6be013c8 d43e0000 brk   #0xF000
```

---

## Reconnecting After VM Reboot

If Target reboots during a session, WinDbg automatically reconnects — no need to restart socat or WinDbg. The relay stays alive.

To manually break into the debugger:

```
Ctrl+Break  (in WinDbg)
```

---

## Useful WinDbg Commands

```
# Show loaded modules
lm

# Stack trace
k

# Process list
!process 0 0

# Break on process creation
bp nt!PspInsertProcess

# Reload symbols
.reload /f

# Continue execution
g
```

---

## Network Info (for reference)

| | Shared Network (NIC0) | Bridged (NIC1) |
|---|---|---|
| **Target** | `10.211.55.6` | `192.168.146.128` |
| **Debugger** | `10.211.55.5` | `192.168.146.94` |
| **macOS host** | `10.211.55.2` | `192.168.146.243` |

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| `/tmp/kd.sock` not created | Target serial config missing — check `config.pvs` |
| `socat: Permission denied` | Run socat with `sudo` |
| WinDbg stuck at `Waiting to reconnect...` | Start WinDbg BEFORE rebooting Target |
| socat dies immediately | Both sockets must exist before running socat |
| `Win32 error 0n87` / `The parameter is incorrect` | Use `com:port=com1,baud=115200,reconnect`, not `\\.\COM1` |
| `Access is denied` opening COM1 | Launch WinDbg as Administrator |
| WinDbg connected but no symbols | Run `.symfix` then `.reload` |
