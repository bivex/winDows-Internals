# WinDbg Kernel Debugging Environment — Cold Start Guide

> Parallels Desktop ARM64 | Windows 11 | Kernel debugging via serial + MCP

---

## 1. Architecture

```
Host (macOS)
├─ Debugger VM (10.211.55.5) — WinDbg + MCP Agent (port 44444)
└─ Target VM (10.211.55.6)   — Debuggee (serial: \\.\com1)
    │
    └─ Connected via named pipe → socat bridge on Host
```

**Serial connection flow:**
```
Target VM (\\.\com1) → Parallels serial socket → socat UNIX bridge → Debugger VM (\\.\com1)
```

---

## 2. VM Startup Order

### 2.1 Start both VMs

```bash
prlctl start "Windows 11 Pro (Target)"
prlctl start "Windows 11 Pro (Debugger)"
```

### 2.2 Verify both are running

```bash
prlctl list --all
```

Both should show `running`.

---

## 3. Serial Debugging Bridge (socat)

### 3.1 Kill stale socat processes

```bash
pkill -f socat 2>/dev/null
```

### 3.2 Start socat bridge

```bash
socat -d -d UNIX-LISTEN:/tmp/com1.sock,fork TCP:10.211.55.5:4445 &
socat -d -d TCP-LISTEN:4445,reuseaddr UNIX-CONNECT:/tmp/com1.sock &
```

### 3.3 Verify

```bash
ps aux | grep socat
```

Should show 2 socat processes.

---

## 4. MCP Agent (Port 44444)

### 4.1 Open firewall on Debugger VM

```bash
prlctl exec "Windows 11 Pro (Debugger)" \
  netsh advfirewall firewall add rule name="MCP Agent" dir=in action=allow protocol=TCP localport=44444

prlctl exec "Windows 11 Pro (Debugger)" \
  netsh advfirewall firewall add rule name="MCP Agent Out" dir=out action=allow protocol=TCP localport=44444
```

### 4.2 Ensure iphlpsvc is running (required for portproxy)

```bash
prlctl exec "Windows 11 Pro (Debugger)" net start iphlpsvc
```

> **CRITICAL:** `iphlpsvc` (IP Helper) is required for `netsh interface portproxy` to work.
> If MCP was working before and suddenly stopped — check this service FIRST.

### 4.3 Verify MCP agent responds

From host:
```bash
curl -s --connect-timeout 5 http://10.211.55.5:44444/mcp
```

Should return some response (not connection refused / not empty).

---

## 5. Common Failure Scenarios

### 5.1 Target frozen at breakpoint → MCP hangs

**Symptom:** MCP returns nothing or "socket connection was closed unexpectedly"

**Fix:**
1. Send `g` (go) command via WinDbg to unfreeze target
2. If MCP is unreachable, use `prlctl exec` to interact with debugger VM
3. Alternative: restart target VM

```bash
prlctl restart "Windows 11 Pro (Target)"
```

### 5.2 MCP port unreachable from host

**Checklist (in order):**
1. Is Debugger VM running? → `prlctl list --all`
2. Is port 44444 open in firewall? → Section 4.1
3. Is iphlpsvc running? → `prlctl exec "Windows 11 Pro (Debugger)" sc query iphlpsvc`
4. Is MCP agent process running inside Debugger VM?

```bash
prlctl exec "Windows 11 Pro (Debugger)" "tasklist /fi \"imagename eq node.exe\""
```

5. Can we reach it locally from Debugger VM?

```bash
prlctl exec "Windows 11 Pro (Debugger)" "curl -s http://localhost:44444/mcp"
```

### 5.3 SSH not available on Debugger VM

SSH is NOT used for debugging. Use `prlctl exec` instead:

```bash
prlctl exec "Windows 11 Pro (Debugger)" <command>
prlctl exec "Windows 11 Pro (Target)" <command>
```

### 5.4 socat bridge broken

**Symptom:** WinDbg shows "not connected" or "waiting for reconnect"

**Fix:**
```bash
pkill -f socat
# Restart with Section 3.2 commands
```

---

## 6. Quick Health Check

Run this to verify everything is working:

```bash
# 1. VMs
prlctl list --all | grep -E "running|stopped"

# 2. socat
ps aux | grep socat | grep -v grep | wc -l  # should be >= 2

# 3. MCP
curl -s --connect-timeout 5 http://10.211.55.5:44444/mcp | head -3

# 4. Firewall rule exists
prlctl exec "Windows 11 Pro (Debugger)" \
  "netsh advfirewall firewall show rule name=\"MCP Agent\"" 2>&1 | head -5
```

---

## 7. Lessons Learned

| # | Issue | Root Cause | Fix |
|---|-------|-----------|-----|
| 1 | MCP unreachable after working before | `iphlpsvc` stopped | `net start iphlpsvc` |
| 2 | Target frozen, MCP hangs | Breakpoint hit in kernel | Send `g` via WinDbg or restart target |
| 3 | Firewall blocks 44444 | Rule missing after reboot | Re-add firewall rule (Section 4.1) |
| 4 | socat stale processes | Leftover from previous session | `pkill -f socat` then restart |
| 5 | Tool results missing ("internal error") | Target frozen at breakpoint during MCP call | Unfreeze target first, retry |

---

## 8. Session Start Checklist

Before starting WinDbg research:

- [ ] Both VMs running (`prlctl list --all`)
- [ ] socat bridge active (`ps aux | grep socat`)
- [ ] Firewall rule for 44444 exists
- [ ] iphlpsvc running on Debugger VM
- [ ] MCP responds to curl test
- [ ] Target NOT frozen at breakpoint
