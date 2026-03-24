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
