# WinDbg Agent MCP — Remote Debugging from Host

Управление ядром Windows через MCP-сервер, встроенный в WinDbg на Debugger VM.
Позволяет отправлять команды WinDbg с хоста (macOS) через HTTP.

## Архитектура

```
macOS (хост)                    Debugger VM                    Target VM
┌─────────────┐    HTTP/MCP    ┌──────────────┐   serial    ┌──────────────┐
│ curl, etc   │──────────────>│  WinDbg      │────────────>│ Windows 11   │
│             │  10.211.55.5  │  + windbg    │   COM1      │ kernel debug │
│             │   :44444      │    _agent.dll│   115200    │              │
└─────────────┘               │  portproxy   │             └──────────────┘
                               │  0.0.0.0:44444             
                               │  → 127.0.0.1:44444         
                               └──────────────┘             
```

## Preconditions

- Две VM запущены, socat relay работает (см. `ColdStart.md`)
- WinDbg подключён к Target (Kernel Debugger connection established)
- Файл `windbg_agent.dll` доступен на Debugger VM

## Шаг 1. One-time portproxy setup (Debugger VM, elevated cmd)

`windbg_agent` биндит только loopback. portproxy форвардит внешний трафик внутрь.
Выполнить один раз — переживает ребуты:

```cmd
net start iphlpsvc

netsh advfirewall firewall add rule name=MCP dir=in action=allow protocol=TCP localport=44444

netsh interface portproxy add v4tov4 listenport=44444 listenaddress=0.0.0.0 connectport=44445 connectaddress=127.0.0.1

netsh interface portproxy add v6tov4 listenport=44444 listenaddress=:: connectport=44445 connectaddress=127.0.0.1
```

Примеч.: portproxy должен слушать на порту 44444 и перенаправлять на внутренний порт `44445`, так как иначе `iphlpsvc` блокирует локальный порт.

Второй proxy нужен потому что Parallels Shared Network блокирует IPv4 между хостом и VM — IPv6 работает, IPv4 нет.

Проверить:

```cmd
netsh interface portproxy show all
```

Ожидаемый вывод:

```
Listen on ipv4:             Connect to ipv4:
Address         Port        Address         Port
--------------- ----------  --------------- ----------
0.0.0.0         44444       127.0.0.1       44445

Listen on ipv6:             Connect to ipv4:
Address         Port        Address         Port
--------------- ----------  --------------- ----------
*               44444       127.0.0.1       44445
```

## Шаг 2. Запустить MCP сервер в WinDbg

Внутри WinDbg на Debugger VM:

```
kd> !load C:\Tools\windbg-agent\windbg_agent.dll
kd> !agent mcp 127.0.0.1 44445
```

Если порт занят стale-процессом:

```cmd
for /f "tokens=5" %a in ('netstat -ano ^| findstr :44444') do taskkill /F /PID %a
```

Ожидаемый вывод:

```
MCP SERVER ACTIVE
Target: ntkrnlmp.exe (PID 0)
...
MCP server is running in background. Use '!agent mcp stop' to stop it.
```

## Шаг 3. Проверить доступность с хоста

Parallels Shared Network блокирует IPv4 между хостом и VM — использовать IPv6:

```bash
curl -s -X POST "http://[fdb2:2c26:f4e4:0:14eb:9504:d0a3:9cc0]:44444/mcp" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}'
```

Узнать актуальный IPv6 Debugger VM:

```bash
prlctl exec "Windows 11 Pro (Debugger)" cmd /c "ipconfig | findstr IPv6"
```

Успешный ответ:

```json
{"id":1,"jsonrpc":"2.0","result":{"capabilities":{"tools":{}},"protocolVersion":"2024-11-05","serverInfo":{"name":"windbg-agent","version":"1.0.0"}}}
```

## Шаг 4. Выполнить команду WinDbg с хоста

```bash
# Получить Session ID
SESSION_ID=$(curl -s -i -X POST "http://[fdb2:2c26:f4e4:0:14eb:9504:d0a3:9cc0]:44444/mcp" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}' \
  | grep -i 'mcp-session-id' | awk '{print $2}' | tr -d '\r')

# Выполнить команду ядра
curl -s -X POST "http://[fdb2:2c26:f4e4:0:14eb:9504:d0a3:9cc0]:44444/mcp" \
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

| Компонент | IPv4 | IPv6 | Порт |
|---|---|---|---|
| macOS хост | `10.211.55.2` | — | — |
| Debugger VM | `10.211.55.5` | `fdb2:2c26:f4e4:0:14eb:9504:d0a3:9cc0` | `44444` |
| Target VM | через serial | — | — |

> Parallels Shared Network блокирует IPv4 трафик хост↔VM. Использовать IPv6 для MCP.

## Gotchas

- `windbg_agent` биндит только `127.0.0.1` — portproxy обязателен для доступа с хоста
- portproxy требует сервис IP Helper (`iphlpsvc`) — запустить `net start iphlpsvc`
- WinDbg должен быть запущен от имени администратора на Debugger VM
- Если Target перезагружается — перезапустить socat relay, WinDbg reconnect автоматически
- Порт `44444` можно изменить — указать в `!agent mcp 127.0.0.1 <порт>` и в portproxy
- `dbg_exec` возвращает ошибку если Target не в break state (нужно Ctrl+Break в WinDbg)
