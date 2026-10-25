// ============================================================================
// SpecialApcRunner.cpp — Exotic Execution Vector: Special User APC Harness
//
// Demonstrates control transfer into a private unexported function inside a PE
// via modern Windows 11 Special User APC (QueueUserAPC2 -> KiUserApcDispatcher):
//   1. Worker thread runs in a strictly NON-ALERTABLE loop (Sleep(10)).
//   2. Normal User APCs CANNOT fire because the thread never enters alertable wait.
//   3. QueueUserAPC2 with QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC (0x1) immediately
//      interrupts the thread mid-execution.
//   4. The CPU context is hijacked by ntdll!KiUserApcDispatcher, which invokes
//      the private unexported function ComComponent!PrivateSpecialApcRoutine.
//   5. ntdll!NtContinue restores the original thread context, resuming the loop.
//
// Build: cl /nologo /EHsc /std:c++20 SpecialApcRunner.cpp user32.lib /Fe:SpecialApcRunner.exe
#include "ComComponent/ComComponent.h"
// ============================================================================

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <atomic>

#ifndef QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC
#define QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC 0x00000001
#endif

using FnGetPrivateApcRoutinePointer = PAPCFUNC(WINAPI*)(void);
using FnQueueUserAPC2 = BOOL(WINAPI*)(PAPCFUNC, HANDLE, ULONG_PTR, DWORD);

static std::atomic<bool> g_workerReady(false);
static std::atomic<bool> g_stopWorker(false);
static std::atomic<bool> g_apcDelivered(false);

DWORD WINAPI NonAlertableWorkerThread(LPVOID lpParam) {
    DWORD tid = GetCurrentThreadId();
    printf("[worker] Non-alertable worker thread started (TID=%lu)...\n", tid);
    printf("[worker] Loop condition: strictly non-alertable Sleep(10). Normal APCs CANNOT enter!\n");
    g_workerReady.store(true);

    int count = 0;
    while (!g_stopWorker.load()) {
        // Sleep(10) is NON-ALERTABLE in Win32!
        Sleep(10);
        count++;
    }

    printf("[worker] Non-alertable loop completed after %d iterations.\n", count);
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    LPCWSTR dllPath = (argc > 1) ? L"ComComponent.dll" : L"ComComponent.dll";

    printf("====================================================================\n");
    printf("  EXOTIC VECTOR TEST: SPECIAL USER APC (QueueUserAPC2)\n");
    printf("  Target DLL : %ls\n", dllPath);
    printf("====================================================================\n");

    // 1. Load ComComponent.dll
    printf("\n>>> STEP 1: Loading Target Component <<<\n");
    HMODULE hMod = LoadLibraryExW(dllPath, nullptr, 0);
    printf("[host] LoadLibraryExW(\"%ls\") -> hMod = %p\n", dllPath, (void*)hMod);
    if (!hMod) {
        printf("[fatal] Failed to load %ls, GLE=%lu\n", dllPath, GetLastError());
        return 1;
    }

    // 2. Query the raw function pointer to the private unexported callback
    auto pfnApcExport = (PAPCFUNC)GetProcAddress(hMod, "ComSpecialApcRoutine");
    printf("[host] GetProcAddress(\"ComSpecialApcRoutine\") -> %p\n", (void*)pfnApcExport);

    auto pfnGetPtr = (FnGetPrivateApcRoutinePointer)GetProcAddress(hMod, "GetPrivateApcRoutinePointer");
    printf("[host] GetProcAddress(\"GetPrivateApcRoutinePointer\") -> %p\n", (void*)pfnGetPtr);

    PAPCFUNC pfnPrivateApc = pfnApcExport ? pfnApcExport : (pfnGetPtr ? pfnGetPtr() : nullptr);
    printf("[host] Target APC Callback VA : %p\n", (void*)pfnPrivateApc);
    if (!pfnPrivateApc) {
        printf("[fatal] Failed to resolve APC target\n");
        return 2;
    }

    // 3. Resolve QueueUserAPC2 from KERNELBASE / KERNEL32
    printf("\n>>> STEP 3: Resolving QueueUserAPC2 <<<\n");
    auto pQueueUserAPC2 = (FnQueueUserAPC2)GetProcAddress(GetModuleHandleA("kernel32.dll"), "QueueUserAPC2");
    if (!pQueueUserAPC2) {
        pQueueUserAPC2 = (FnQueueUserAPC2)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "QueueUserAPC2");
    }
    printf("[host] QueueUserAPC2 resolved : %p\n", (void*)pQueueUserAPC2);
    if (!pQueueUserAPC2) {
        printf("[fatal] QueueUserAPC2 not supported on this OS build\n");
        return 3;
    }

    // 4. Start the Non-Alertable Worker Thread
    printf("\n>>> STEP 4: Spawning Non-Alertable Worker Thread <<<\n");
    DWORD workerTid = 0;
    HANDLE hThread = CreateThread(nullptr, 0, NonAlertableWorkerThread, nullptr, 0, &workerTid);
    printf("[host] CreateThread -> hThread = %p, TID = %lu\n", (void*)hThread, workerTid);

    // Wait until worker loop is actively running
    while (!g_workerReady.load()) {
        Sleep(5);
    }
    Sleep(20); // Ensure worker is inside Sleep(10)

    // 5. Dispatch Special User APC!
    printf("\n>>> STEP 5: Dispatching Special User APC (Interrupts Non-Alertable Loop!) <<<\n");
    ULONG_PTR apcData = 0xDEADBEEF1337ULL;
    printf("[host] Invoking QueueUserAPC2 with QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC (0x1):\n");
    printf("  Target APC Routine : %p\n", (void*)pfnPrivateApc);
    printf("  Target Thread TID  : %lu\n", workerTid);
    printf("  Argument dwData    : 0x%llX\n", (unsigned long long)apcData);

    BOOL bQueued = pQueueUserAPC2(pfnPrivateApc, hThread, apcData, QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC);
    printf("[host] QueueUserAPC2 result : %s (GLE=%lu)\n", bQueued ? "SUCCESS" : "FAILED", GetLastError());

    // Allow APC delivery and execution
    Sleep(100);

    // 6. Signal worker to stop and wait
    printf("\n>>> STEP 6: Worker Shutdown & Cleanup <<<\n");
    g_stopWorker.store(true);
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    FreeLibrary(hMod);
    printf("[host] FreeLibrary completed\n");

    printf("\n====================================================================\n");
    printf("  SPECIAL USER APC EXECUTION COMPLETED\n");
    printf("====================================================================\n");

    return 0;
}
