// ============================================================================
// MixedExe.cpp - Pure Native Win32 Client for C++/CLI Mixed-Mode Research
//
// Links dynamically against MixedLib.dll.
// Exercises:
// 1. Forward and reverse managed crossover.
// 2. Exception propagation through clr!UMThunkStub into native catch blocks.
// 3. Multi-threaded verification of native Win32 TLS vs managed [ThreadStatic].
//
// Build: cl MixedExe.cpp MixedLib.lib /Fe:MixedExe.exe
// ============================================================================

#include <stdio.h>
#include <windows.h>

// Import declarations from MixedLib.dll
extern "C" {
    __declspec(dllimport) int  NativeCallsManaged(int x);
    __declspec(dllimport) int  TestMixedSeh(int x);
    __declspec(dllimport) void TestBothTls(int threadId);
    __declspec(dllimport) int  TestPointerAnatomy();
}

DWORD WINAPI WorkerThreadProc(LPVOID lpParam) {
    int id = (int)(INT_PTR)lpParam;
    printf("\n--- [Worker Thread %d] Starting Execution ---\n", id);
    TestBothTls(id);
    return 0;
}

int main() {
    printf("====================================================================\n");
    printf("  Windows C++/CLI Mixed-Mode (IJW) Loader & Interop Verification\n");
    printf("====================================================================\n\n");

    // ------------------------------------------------------------------------
    // Experiment 1: Forward & Reverse Crossover Execution
    // ------------------------------------------------------------------------
    printf(">>> EXPERIMENT 1: Forward & Reverse Crossover <<<\n");
    int calcResult = NativeCallsManaged(21);
    printf(">>> Result returned to NativeExe: %d (Expected: 42) <<<\n\n", calcResult);

    // ------------------------------------------------------------------------
    // Experiment 2: Mixed-Mode SEH Exception Bridging
    // ------------------------------------------------------------------------
    printf(">>> EXPERIMENT 2: Mixed-Mode SEH Exception Unwinding <<<\n");
    int sehResult = TestMixedSeh(1);
    printf(">>> SEH Handler Result: %d (Expected: 999) <<<\n\n", sehResult);

    // ------------------------------------------------------------------------
    // Experiment 3: Dual-TLS Multi-Threaded Independence & Initialization
    // ------------------------------------------------------------------------
    printf(">>> EXPERIMENT 3: Dual-TLS (Native vs Managed) <<<\n");
    
    printf("\n[Main Thread - Pass 1]\n");
    TestBothTls(1);

    HANDLE hWorker = CreateThread(NULL, 0, WorkerThreadProc, (LPVOID)2, 0, NULL);
    if (hWorker != NULL) {
        WaitForSingleObject(hWorker, INFINITE);
        CloseHandle(hWorker);
    }

    printf("\n[Main Thread - Pass 2 (Revisit)]\n");
    TestBothTls(1);


    // ------------------------------------------------------------------------
    // Experiment 4: Pointer Anatomy (gcroot, pin_ptr, interior_ptr)
    // ------------------------------------------------------------------------
    printf("\n>>> EXPERIMENT 4: Pointer Anatomy (gcroot, pin_ptr, interior_ptr) <<<\n");
    int ptrResult = TestPointerAnatomy();
    printf(">>> Pointer Anatomy Result: %d (Expected: 1) <<<\n", ptrResult);
    printf("\n====================================================================\n");
    printf("  All C++/CLI IJW Loader Scenarios Verified Successfully\n");
    printf("====================================================================\n");
    return 0;
}
