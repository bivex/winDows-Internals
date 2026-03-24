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
```
EXAMPLE CURL COMMANDS:
  # Initialize session (inspect Mcp-Session-Id response header)
  curl -i -X POST http://127.0.0.1:37780/mcp \
    -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}'

  # List tools using the returned Mcp-Session-Id
  curl -X POST http://1

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
