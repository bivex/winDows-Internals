//
// FileSentinel usermode service — connects via CDO device (no fltuser.h needed)
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
// Main
// ===========================================================================
int __cdecl wmain(int argc, wchar_t *argv[])
{
    HANDLE hDevice;
    DWORD bytesReturned;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    wprintf(L"FileSentinel service starting...\n");

    hDevice = CreateFileW(
        SENTINEL_USER_PATH,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (hDevice == INVALID_HANDLE_VALUE) {
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

    for (;;) {
        SENTINEL_MESSAGE msg = { 0 };

        // Poll for next event
        BOOL ok = DeviceIoControl(
            hDevice,
            IOCTL_SENTINEL_GET_EVENT,
            NULL, 0,
            &msg, sizeof(msg),
            &bytesReturned,
            NULL);

        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_MORE_ITEMS || err == ERROR_NOT_FOUND) {
                Sleep(50);  // No event pending, wait and retry
                continue;
            }
            if (err == ERROR_INVALID_HANDLE || err == ERROR_BROKEN_PIPE) {
                wprintf(L"Driver disconnected.\n");
                break;
            }
            wprintf(L"GET_EVENT error: %lu\n", err);
            Sleep(100);
            continue;
        }

        // Evaluate and send verdict
        SENTINEL_VERDICT verdict = EvaluateRule(&msg);
        SENTINEL_REPLY reply = { 0 };
        reply.Verdict = verdict;

        ok = DeviceIoControl(
            hDevice,
            IOCTL_SENTINEL_REPLY_EVENT,
            &reply, sizeof(reply),
            NULL, 0,
            &bytesReturned,
            NULL);

        if (!ok) {
            wprintf(L"REPLY_EVENT error: %lu\n", GetLastError());
        }
    }

    CloseHandle(hDevice);
    wprintf(L"FileSentinel service stopped.\n");
    return 0;
}
