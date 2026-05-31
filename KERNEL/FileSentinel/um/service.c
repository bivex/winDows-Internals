//
// FileSentinel usermode service
// Connects to minifilter driver, receives file operation events, applies rules
//
// Build:  cl service.c /Fe:sentinel_svc.exe
// Run:    sentinel_svc.exe
//

#include <windows.h>
#include <fltuser.h>    // Filter Manager usermode API
#include <stdio.h>

#pragma comment(lib, "fltlib.lib")

#include "../driver/sentinel.h"

// ---------------------------------------------------------------------------
// Simple rule: block writes to paths containing these substrings
// ---------------------------------------------------------------------------
static const WCHAR *g_BlockedPaths[] = {
    L"\\Blocked\\",
    L"\\ReadOnly\\",
    // Add more rules here
};
static ULONG g_BlockedPathCount = sizeof(g_BlockedPaths) / sizeof(g_BlockedPaths[0]);

// ---------------------------------------------------------------------------
// Rule engine: check if file path should be blocked
// ---------------------------------------------------------------------------
static SENTINEL_VERDICT EvaluateRule(PSENTINEL_MESSAGE msg)
{
    for (ULONG i = 0; i < g_BlockedPathCount; i++) {
        if (wcsstr(msg->FilePath, g_BlockedPaths[i])) {
            wprintf(L"[BLOCK] PID=%lu %s %ls\n",
                    msg->ProcessId,
                    msg->Type == SentinelMsg_FileCreate  ? "CREATE" :
                    msg->Type == SentinelMsg_FileWrite   ? "WRITE"  :
                    msg->Type == SentinelMsg_FileDelete  ? "DELETE" :
                    msg->Type == SentinelMsg_FileRename  ? "RENAME" : "?",
                    msg->FilePath);
            return SentinelVerdict_Deny;
        }
    }

    // Log allowed operations (optional, noisy)
    if (msg->Type == SentinelMsg_FileCreate || msg->Type == SentinelMsg_FileDelete) {
        wprintf(L"[ALLOW] PID=%lu %s %ls\n",
                msg->ProcessId,
                msg->Type == SentinelMsg_FileCreate  ? "CREATE" :
                msg->Type == SentinelMsg_FileDelete  ? "DELETE" :
                msg->Type == SentinelMsg_FileRename  ? "RENAME" : "WRITE",
                msg->FilePath);
    }

    return SentinelVerdict_Allow;
}

// ===========================================================================
// Main loop
// ===========================================================================
int __cdecl wmain(int argc, wchar_t *argv[])
{
    HANDLE hPort = NULL;
    HRESULT hr;
    DWORD bytesReturned;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    wprintf(L"FileSentinel service starting...\n");

    // Connect to the minifilter driver
    hr = FilterConnectCommunicationPort(
            SENTINEL_PORT_USER,
            0,            // options
            NULL,         // context
            0,            // contextSize
            NULL,         // security
            &hPort);

    if (FAILED(hr)) {
        wprintf(L"ERROR: cannot connect to driver (hr=0x%08X). Is driver loaded?\n", hr);
        return 1;
    }

    wprintf(L"Connected to FileSentinel driver.\n");
    wprintf(L"Monitoring file operations. Blocked paths:\n");
    for (ULONG i = 0; i < g_BlockedPathCount; i++) {
        wprintf(L"  %ls\n", g_BlockedPaths[i]);
    }
    wprintf(L"\n");

    // Main message loop
    for (;;) {
        SENTINEL_MESSAGE msg = { 0 };
        SENTINEL_REPLY reply = { 0 };

        // Blocking receive — driver sends operation info
        hr = FilterGetMessage(hPort, &msg.Header, sizeof(msg), &bytesReturned);
        if (FAILED(hr)) {
            if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE)) {
                wprintf(L"Driver disconnected.\n");
                break;
            }
            wprintf(L"FilterGetMessage error: 0x%08X\n", hr);
            continue;
        }

        // Evaluate rules
        reply.Verdict = EvaluateRule(&msg);

        // Send verdict back to driver
        hr = FilterReplyMessage(hPort, &reply.Header, sizeof(reply));
        if (FAILED(hr)) {
            wprintf(L"FilterReplyMessage error: 0x%08X\n", hr);
        }
    }

    if (hPort)
        CloseHandle(hPort);

    wprintf(L"FileSentinel service stopped.\n");
    return 0;
}
