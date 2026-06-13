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
│  │       │          │           │  + windbg_agent  │       │
│  │  ARM PL011 UART  │           │  :44444 (MCP)    │       │
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
bash /Volumes/External/Code/winDows-Internals/KERNEL/START_DEBUG_SESSION.sh
```

Run as normal user (NOT sudo) — `prlctl` is per-user. The script asks for sudo only for socat.

Script sequence:
1. Configures serial ports (idempotent)
2. Kills stale socat, removes old sockets
3. Starts both VMs
4. Waits for both Unix sockets
5. Starts socat relay
6. Launches WinDbg on Debugger VM via `prlctl exec`
7. Restarts socat (Parallels recreates the socket on reboot)
8. Reboots Target VM — WinDbg catches boot break automatically

For a detailed from-scratch checklist, see [ColdStart.md](ColdStart.md).

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

## WinDbg Agent MCP

`windbg_agent.dll` exposes a Streamable HTTP MCP server inside WinDbg, allowing commands from the macOS host.

See [windbg_agent.md](windbg_agent.md) for full setup.

### Quick start

```
kd> !load C:\Tools\windbg-agent\windbg_agent.dll
kd> !agent mcp 0.0.0.0 44444
```

If port 44444 is already bound (stale process), kill it first in an elevated cmd on Debugger VM:

```cmd
for /f "tokens=5" %a in ('netstat -ano ^| findstr :44444') do taskkill /F /PID %a
```

Then retry `!agent mcp 0.0.0.0 44444`.

### Verify from macOS host

```bash
curl -s -X POST http://10.211.55.5:44444/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}'
```

---

## Reconnecting After VM Reboot

Parallels **deletes and recreates** `/tmp/kd.sock` on every Target reboot.
The running socat holds a dead fd to the old socket and silently stops relaying.

**Rule:** always restart socat before rebooting Target.

```bash
sudo pkill -f "socat.*kd.sock"
sudo /opt/homebrew/bin/socat UNIX-CONNECT:/tmp/kd.sock UNIX-CONNECT:/tmp/debugger.sock &
prlctl exec "Windows 11 Pro (Target)" --current-user cmd /c "shutdown /r /t 0"
```

WinDbg will reconnect automatically — no restart needed.

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

# Force break into target
Ctrl+Break
```

---

## Network Info

| | Shared Network (NIC0) | Bridged (NIC1) |
|---|---|---|
| **Target** | `10.211.55.6` | `192.168.146.128` |
| **Debugger** | `10.211.55.5` | `192.168.146.94` |
| **macOS host** | `10.211.55.2` | `192.168.146.243` |

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| `/tmp/kd.sock` not created | Target serial config missing — run step 0 of the script |
| `socat: Permission denied` | Run socat with `sudo` |
| WinDbg stuck at `Waiting to reconnect...` | Start WinDbg BEFORE rebooting Target |
| socat dies immediately | Both sockets must exist before running socat |
| `Win32 error 0n87` / `The parameter is incorrect` | Use `com:port=com1,baud=115200,reconnect`, not `\\.\COM1` |
| `Access is denied` opening COM1 | Launch WinDbg as Administrator |
| WinDbg connected but no symbols | Run `.symfix` then `.reload` |
| `!agent mcp` returns `start() returned false` | Port already bound — kill the PID with `taskkill /F /PID <pid>` |
| MCP curl times out from host | WinDbg was bound to `127.0.0.1` — use `!agent mcp 0.0.0.0 44444` |
