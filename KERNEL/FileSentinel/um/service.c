//
// FileSentinel usermode service — standalone (no fltuser.h required)
// Connects to minifilter via CreateFile on the filter port device
//
// Build: cl service.c /Fe:sentinel_svc.exe
//

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../driver/sentinel.h"

// ---------------------------------------------------------------------------
// Blocked path rules
// ---------------------------------------------------------------------------
static const WCHAR *g_BlockedPaths[] = {
    L"\\Blocked\\",
    L"\\ReadOnly\\",
};
#define BLOCKED_COUNT (sizeof(g_BlockedPaths) / sizeof(g_BlockedPaths[0]))

// ---------------------------------------------------------------------------
// Rule engine
// ---------------------------------------------------------------------------
static SENTINEL_VERDICT EvaluateRule(PSENTINEL_MESSAGE msg)
{
    for (DWORD i = 0; i < BLOCKED_COUNT; i++) {
        if (wcsstr(msg->FilePath, g_BlockedPaths[i])) {
            wprintf(L"[BLOCK] PID=%lu %hs %ls\n",
                    msg->ProcessId,
                    msg->Type == SentinelMsg_FileCreate  ? "CREATE" :
                    msg->Type == SentinelMsg_FileWrite   ? "WRITE"  :
                    msg->Type == SentinelMsg_FileDelete  ? "DELETE" :
                    msg->Type == SentinelMsg_FileRename  ? "RENAME" : "?",
                    msg->FilePath);
            return SentinelVerdict_Deny;
        }
    }

    if (msg->Type == SentinelMsg_FileCreate || msg->Type == SentinelMsg_FileDelete) {
        wprintf(L"[ALLOW] PID=%lu %hs %ls\n",
                msg->ProcessId,
                msg->Type == SentinelMsg_FileCreate  ? "CREATE" :
                msg->Type == SentinelMsg_FileDelete  ? "DELETE" :
                msg->Type == SentinelMsg_FileRename  ? "RENAME" : "WRITE",
                msg->FilePath);
    }

    return SentinelVerdict_Allow;
}

// ===========================================================================
// Main — connect to driver and process events
// ===========================================================================
int __cdecl wmain(int argc, wchar_t *argv[])
{
    HANDLE hPort;
    HRESULT hr;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    wprintf(L"FileSentinel service starting...\n");

    //
    // Connect to the minifilter communication port.
    // Filter Manager creates a device for each port at \Device\FileSentinelPort
    // Usermode opens it via \\.\FileSentinelPort
    //
    hPort = CreateFileW(
        L"\\\\.\\FileSentinelPort",
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (hPort == INVALID_HANDLE_VALUE) {
        wprintf(L"ERROR: cannot connect to driver (err=%lu). Is driver loaded?\n",
                GetLastError());
        return 1;
    }

    wprintf(L"Connected to FileSentinel driver.\n");
    wprintf(L"Monitoring file operations. Blocked paths:\n");
    for (DWORD i = 0; i < BLOCKED_COUNT; i++) {
        wprintf(L"  %ls\n", g_BlockedPaths[i]);
    }
    wprintf(L"\n");

    // Main loop — read messages from driver, send verdicts back
    for (;;) {
        SENTINEL_MESSAGE msg = { 0 };
        DWORD bytesReturned;

        // Read operation info from driver
        BOOL ok = ReadFile(hPort, &msg, sizeof(msg), &bytesReturned, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_INVALID_HANDLE || err == ERROR_BROKEN_PIPE) {
                wprintf(L"Driver disconnected.\n");
                break;
            }
            wprintf(L"ReadFile error: %lu\n", err);
            continue;
        }

        // Evaluate rules
        SENTINEL_VERDICT verdict = EvaluateRule(&msg);

        // Send verdict back
        SENTINEL_REPLY reply = { 0 };
        reply.Verdict = verdict;

        ok = WriteFile(hPort, &reply, sizeof(reply), &bytesReturned, NULL);
        if (!ok) {
            wprintf(L"WriteFile error: %lu\n", GetLastError());
        }
    }

    CloseHandle(hPort);
    wprintf(L"FileSentinel service stopped.\n");
    return 0;
}
