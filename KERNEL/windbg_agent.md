# WinDbg Agent MCP — Remote Debugging from Host

Управление ядром Windows через MCP-сервер, встроенный в WinDbg на Debugger VM.
Позволяет отправлять команды WinDbg с хоста (macOS) через HTTP.

## Архитектура

```
macOS (хост)                    Debugger VM                    Target VM
┌─────────────┐    HTTP/MCP    ┌──────────────┐   serial    ┌──────────────┐
│ Claude Code  │──────────────>│  WinDbg      │────────────>│ Windows 11   │
│ curl, etc    │  10.211.55.5 │  + windbg    │   COM1      │ kernel debug │
│              │   :44444      │    _agent.dll│   115200    │              │
└─────────────┘               └──────────────┘             └──────────────┘
```

## Preconditions

- Две VM запущены, socat relay работает (см. `ColdStart.md`)
- WinDbg подключён к Target (Kernel Debugger connection established)
- Файл `windbg_agent.dll` доступен на Debugger VM

## Шаг 1. Запустить MCP сервер в WinDbg

Внутри WinDbg на Debugger VM:

```
kd> !load C:\Tools\windbg-agent\windbg_agent.dll
kd> !agent mcp 0.0.0.0 44444
```

Флаг `0.0.0.0` — бинд на все интерфейсы, чтобы хост мог достучаться по сети.
Без него (по умолчанию `127.0.0.1`) — доступ только внутри VM.

Ожидаемый вывод:

```
MCP SERVER ACTIVE
Target: ntkrnlmp.exe (PID 0)
...
MCP server is running in background. Use '!agent mcp stop' to stop it.
```

## Шаг 2. Проверить доступность с хоста

```bash
# Пинг MCP сервера
curl -s -X POST http://10.211.55.5:44444/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}'
```

Успешный ответ:

```json
{"id":1,"jsonrpc":"2.0","result":{"capabilities":{"tools":{}},"protocolVersion":"2024-11-05","serverInfo":{"name":"windbg-agent","version":"1.0.0"}}}
```

## Шаг 3. Выполнить команду WinDbg с хоста

```bash
# Получить Session ID
SESSION_ID=$(curl -s -i -X POST http://10.211.55.5:44444/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}' \
  | grep -i 'mcp-session-id' | awk '{print $2}' | tr -d '\r')

# Выполнить команду ядра
curl -s -X POST http://10.211.55.5:44444/mcp \
  -H "Content-Type: application/json" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"dbg_exec","arguments":{"command":"version"}}}'
```

## Доступные инструменты

| Tool | Описание | Пример команды |
|---|---|---|
| `dbg_exec` | Выполнить команду WinDbg | `kb`, `lm`, `!process 0 0`, `dt nt!_EPROCESS` |
| `dbg_ask` | Задать вопрос AI-ассистенту | `"почему BSOD на nt!KeBugCheckEx?"` |

## Примеры полезных команд

```bash
# Стек текущего потока
'{"name":"dbg_exec","arguments":{"command":"kb"}}'

# Список загруженных модулей
'{"name":"dbg_exec","arguments":{"command":"lm"}}'

# Все процессы
'{"name":"dbg_exec","arguments":{"command":"!process 0 0"}}'

# Структура EPROCESS
'{"name":"dbg_exec","arguments":{"command":"dt nt!_EPROCESS"}}'

# Анализ последнего BSOD
'{"name":"dbg_exec","arguments":{"command":"!analyze -v"}}'
```

## Остановка MCP сервера

В WinDbg на Debugger VM:

```
kd> !agent mcp stop
```

## Сетевые адреса

| Компонент | IP | Порт |
|---|---|---|
| macOS хост | `10.211.55.2` | — |
| Debugger VM (shared) | `10.211.55.5` | `44444` |
| Target VM | через serial | — |

## Gotchas

- `0.0.0.0` обязателен — без него MCP слушает только `127.0.0.1` и с хоста не достучаться
- WinDbg должен быть запущен от имени администратора на Debugger VM
- Если Target перезагружается — перезапустить socat relay, затем WinDbg reconnect автоматически
- Порт `44444` можно изменить — указать в команде `!agent mcp 0.0.0.0 <порт>`
- `dbg_exec` возвращает ошибку если Target не в break state (нужно Ctrl+Break в WinDbg)
