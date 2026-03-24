kd> !load Y:\Code\windbg-agent\build-arm64-vs2026\Release\windbg_agent.dll
kd> !agent mcp
MCP SERVER ACTIVE
Target: ntkrnlmp.exe (PID 0)
...

MCP CLIENT CONFIGURATION:
Add to your MCP client (e.g., Claude Desktop):
```
{
  "mcpServers": {
    "windbg-agent": {
      "url": "http://127.0.0.1:37780/mcp"
    }
  }
}
```

....................


add Global: ~/.codex/config.toml lines:

```
[mcp_servers.windbg-agent]
url = "http://127.0.0.1:37780/mcp"
enabled = true
```

run codex and See 

```
/mcp

🔌  MCP Tools

  • windbg-agent
    • Status: enabled
    • Auth: Unsupported
    • URL: http://127.0.0.1:37780/mcp
    • Tools: (none)
    • Resources: (none)
    • Resource templates: (none)
```

Success criteria

```
• Connected. The WinDbg MCP is active and responding.

  Current target is a live remote kernel debug session:

  - Windows 10 kernel build 26100.1 on ARM64
  - Transport: Remote KD over COM1
  - Debugger: WinDbg 10.0.29507.1001

  The dbg_ask helper failed to start its side session, but direct debugger commands work, so I can still drive WinDbg
  through MCP. If you want, I can run something specific next, for example !analyze -v, kb, lm, or inspect a process/
  thread.
````
