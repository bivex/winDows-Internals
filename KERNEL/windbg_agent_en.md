# WinDbg Agent MCP — Remote Debugging from Host

Control a Windows kernel through the MCP server built into WinDbg on the Debugger VM.
Send WinDbg commands from the host (macOS) over HTTP.

## Architecture

```
macOS (host)                    Debugger VM                    Target VM
┌─────────────┐    HTTP/MCP    ┌──────────────┐   serial    ┌──────────────┐
│ Claude Code  │──────────────>│  WinDbg      │────────────>│ Windows 11   │
│ curl, etc    │  10.66.57.105 │  + windbg    │   COM1      │ kernel debug │
│              │   :44444      │    _agent.dll│   115200    │              │
└─────────────┘               └──────────────┘             └──────────────┘
```

## Preconditions

- Both VMs are running, socat relay is active (see `ColdStart.md`)
- WinDbg is connected to the target (Kernel Debugger connection established)
- `windbg_agent.dll` is available on the Debugger VM

## Step 1. Start MCP server in WinDbg

Inside WinDbg on the Debugger VM:

```
kd> !load C:\Tools\windbg-agent\windbg_agent.dll
kd> !agent mcp 0.0.0.0 44444
```

The `0.0.0.0` flag binds to all interfaces so the host can reach it over the network.
Without it (defaults to `127.0.0.1`) — only accessible inside the VM.

Expected output:

```
MCP SERVER ACTIVE
Target: ntkrnlmp.exe (PID 0)
...
MCP server is running in background. Use '!agent mcp stop' to stop it.
```

## Step 2. Verify connectivity from host

```bash
# Ping the MCP server
curl -s -X POST http://10.66.57.105:44444/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}'
```

Successful response:

```json
{"id":1,"jsonrpc":"2.0","result":{"capabilities":{"tools":{}},"protocolVersion":"2024-11-05","serverInfo":{"name":"windbg-agent","version":"1.0.0"}}}
```

## Step 3. Execute a WinDbg command from host

```bash
# Get Session ID
SESSION_ID=$(curl -s -i -X POST http://10.66.57.105:44444/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}' \
  | grep -i 'mcp-session-id' | awk '{print $2}' | tr -d '\r')

# Execute a kernel command
curl -s -X POST http://10.66.57.105:44444/mcp \
  -H "Content-Type: application/json" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"dbg_exec","arguments":{"command":"version"}}}'
```

## Available Tools

| Tool | Description | Example command |
|---|---|---|
| `dbg_exec` | Execute a WinDbg command | `kb`, `lm`, `!process 0 0`, `dt nt!_EPROCESS` |
| `dbg_ask` | Ask the AI debugging assistant | `"why did BSOD happen at nt!KeBugCheckEx?"` |

## Example Commands

```bash
# Current thread stack
'{"name":"dbg_exec","arguments":{"command":"kb"}}'

# Loaded modules
'{"name":"dbg_exec","arguments":{"command":"lm"}}'

# All processes
'{"name":"dbg_exec","arguments":{"command":"!process 0 0"}}'

# EPROCESS structure
'{"name":"dbg_exec","arguments":{"command":"dt nt!_EPROCESS"}}'

# Analyze last BSOD
'{"name":"dbg_exec","arguments":{"command":"!analyze -v"}}'
```

## Stopping the MCP Server

In WinDbg on the Debugger VM:

```
kd> !agent mcp stop
```

## Network Addresses

| Component | IP | Port |
|---|---|---|
| macOS host | `10.66.57.104` | — |
| Debugger VM (bridge) | `10.66.57.105` | `44444` |
| Target VM | via serial | — |

## Gotchas

- `0.0.0.0` is required — without it MCP listens on `127.0.0.1` only and the host cannot reach it
- WinDbg must run as Administrator on the Debugger VM
- If the Target reboots — restart the socat relay first, then WinDbg reconnects automatically
- Port `44444` can be changed — specify in the command `!agent mcp 0.0.0.0 <port>`
- `dbg_exec` returns an error if the Target is not in a break state (need Ctrl+Break in WinDbg)
