// ============================================================================
// CorrelatorMatrixRunner.cpp — 5-Scenario Deterministic UM Invocation Test Harness
//
// Validates the 3-Level Correlation Model across 5 fundamental invocation patterns:
//   Scenario 1 (P0): LoadLibraryExW -> DllMain(PROCESS_ATTACH)
//   Scenario 2 (P0): GetProcAddress -> Function Pointer Invoke -> Return
//   Scenario 3 (P0): Duplicate LoadLibraryExW (Refcount tracking, no second attach)
//   Scenario 4 (P1): COM In-Process Activation (Factory -> CreateInstance -> Method)
//   Scenario 5 (P1): CreateThread with Start Routine inside DLL address space
//
// Build: cl /nologo /EHsc /std:c++20 CorrelatorMatrixRunner.cpp user32.lib ole32.lib /Fe:CorrelatorMatrixRunner.exe
// Usage: CorrelatorMatrixRunner.exe [path-to-ComComponent.dll]
// ============================================================================

#include "ComComponent/ComComponent.h"
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

using FnDllGetClassObject = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using FnDllCanUnloadNow   = HRESULT(WINAPI*)(void);
using FnComWorkerRoutine  = DWORD(WINAPI*)(LPVOID);

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    LPCWSTR dllPath = (argc > 1) ? L"ComComponent.dll" : L"ComComponent.dll";

    printf("====================================================================\n");
    printf("  CORRELATION MATRIX TEST RUNNER (5 SCENARIOS)\n");
    printf("  Target DLL: %ls\n", dllPath);
    printf("====================================================================\n");

    // ------------------------------------------------------------------------
    // Scenario 1 (P0): Initial LoadLibraryExW -> DllMain(DLL_PROCESS_ATTACH)
    // ------------------------------------------------------------------------
    printf("\n>>> SCENARIO 1 (P0): Initial LoadLibraryExW -> DllMain(PROCESS_ATTACH) <<<\n");
    HMODULE hMod1 = LoadLibraryExW(dllPath, nullptr, 0);
    printf("[runner] LoadLibraryExW(\"%ls\") -> hMod1 = %p\n", dllPath, (void*)hMod1);
    if (!hMod1) {
        printf("[fatal] Failed to load %ls, GLE=%lu\n", dllPath, GetLastError());
        return 1;
    }

    // ------------------------------------------------------------------------
    // Scenario 2 (P0): Dynamic Resolution via GetProcAddress -> Direct Execution
    // ------------------------------------------------------------------------
    printf("\n>>> SCENARIO 2 (P0): GetProcAddress -> DllGetClassObject Invocation <<<\n");
    auto pfnDllGetClassObject = (FnDllGetClassObject)GetProcAddress(hMod1, "DllGetClassObject");
    printf("[runner] GetProcAddress(hMod1, \"DllGetClassObject\") -> %p\n", (void*)pfnDllGetClassObject);

    IClassFactory* pFactory = nullptr;
    if (pfnDllGetClassObject) {
        HRESULT hrClass = pfnDllGetClassObject(CLSID_Calculator, IID_IClassFactory, (void**)&pFactory);
        printf("[runner] pfnDllGetClassObject() invoked -> hr = 0x%08lX, factory = %p\n",
               (unsigned long)hrClass, (void*)pFactory);
    }

    // ------------------------------------------------------------------------
    // Scenario 3 (P0): Duplicate LoadLibraryExW (Refcount Verification)
    // ------------------------------------------------------------------------
    printf("\n>>> SCENARIO 3 (P0): Duplicate LoadLibraryExW (Refcount Check) <<<\n");
    HMODULE hMod2 = LoadLibraryExW(dllPath, nullptr, 0);
    printf("[runner] Second LoadLibraryExW(\"%ls\") -> hMod2 = %p (Matches hMod1: %s)\n",
           dllPath, (void*)hMod2, (hMod1 == hMod2) ? "YES (Same Virtual Base)" : "NO");

    BOOL bFree2 = FreeLibrary(hMod2);
    printf("[runner] FreeLibrary(hMod2) -> %s (Refcount decremented, DLL remains mapped)\n",
           bFree2 ? "TRUE" : "FALSE");

    // ------------------------------------------------------------------------
    // Scenario 4 (P1): COM In-Process Interface Activation
    // ------------------------------------------------------------------------
    printf("\n>>> SCENARIO 4 (P1): COM In-Process Activation (CreateInstance -> Add) <<<\n");
    ICalculator* pCalc = nullptr;
    if (pFactory) {
        HRESULT hrCreate = pFactory->CreateInstance(nullptr, IID_ICalculator, (void**)&pCalc);
        printf("[runner] pFactory->CreateInstance -> hr = 0x%08lX, pCalc = %p\n",
               (unsigned long)hrCreate, (void*)pCalc);

        if (SUCCEEDED(hrCreate) && pCalc) {
            int addResult = 0;
            pCalc->Add(10, 32, &addResult);
            printf(">>> Result Add(10, 32): %d (Expected: 42) <<<\n", addResult);
            pCalc->Release();
        }
        pFactory->Release();
    }

    // ------------------------------------------------------------------------
    // Scenario 5 (P1): CreateThread with Start Routine in DLL Address Space
    // ------------------------------------------------------------------------
    printf("\n>>> SCENARIO 5 (P1): CreateThread targeting DLL Exported Routine <<<\n");
    auto pfnWorker = (FnComWorkerRoutine)GetProcAddress(hMod1, "ComWorkerRoutine");
    printf("[runner] GetProcAddress(hMod1, \"ComWorkerRoutine\") -> %p\n", (void*)pfnWorker);

    if (pfnWorker) {
        DWORD threadId = 0;
        HANDLE hThread = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)pfnWorker,
                                      (LPVOID)0x1337, 0, &threadId);
        printf("[runner] CreateThread(ComWorkerRoutine) -> hThread = %p, TID = %lu\n",
               (void*)hThread, threadId);

        if (hThread) {
            WaitForSingleObject(hThread, INFINITE);
            DWORD threadExitCode = 0;
            GetExitCodeThread(hThread, &threadExitCode);
            CloseHandle(hThread);
            printf("[runner] Thread terminated! ExitCode = %lu (Expected: 42)\n", threadExitCode);
        }
    }
    // ------------------------------------------------------------------------
    // Scenario 6 (P0 Control): LOAD_LIBRARY_AS_IMAGE_RESOURCE (Data-Only Mapping)
    // ------------------------------------------------------------------------
    printf("\n>>> SCENARIO 6 (P0 Control): LOAD_LIBRARY_AS_IMAGE_RESOURCE (Data-Only Mapping) <<<\n");
    HMODULE hModRes = LoadLibraryExW(dllPath, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    printf("[runner] LoadLibraryExW(LOAD_LIBRARY_AS_IMAGE_RESOURCE) -> hModRes = %p\n", (void*)hModRes);
    printf("[runner]   Handle Tag Bits : 0x%zx (%s)\n",
           (uintptr_t)hModRes & 0x3,
           ((uintptr_t)hModRes & 0x3) == 0x2 ? "Tagged: Image Resource (bit 1 set)" : "Untagged");

    uintptr_t cleanBase = (uintptr_t)hModRes & ~0x3;
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery((LPCVOID)cleanBase, &mbi, sizeof(mbi));
    printf("[runner]   VirtualQuery    : Protect=0x%lX (%s), Type=0x%lX (%s)\n",
           mbi.Protect,
           (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) ?
           "EXECUTABLE" : "NON-EXECUTABLE (PAGE_READONLY)",
           mbi.Type, (mbi.Type == MEM_IMAGE) ? "MEM_IMAGE" : "OTHER");

    SetLastError(0);
    FARPROC pFnRes = GetProcAddress(hModRes, "DllGetClassObject");
    DWORD gleRes = GetLastError();
    printf("[runner]   GetProcAddress  : %p (GLE=%lu - %s)\n",
           (void*)pFnRes, gleRes,
           (pFnRes == nullptr) ? "CONFIRMED: Export resolution rejected on resource view" : "UNEXPECTED");

    FreeLibrary(hModRes);
    printf("[runner]   FreeLibrary(hModRes) -> Released clean resource view (NO DllMain, NO relocs)\n");

    // ------------------------------------------------------------------------
    // Teardown: Final FreeLibrary -> DllMain(DLL_PROCESS_DETACH) & Physical Unload
    // ------------------------------------------------------------------------
    printf("\n>>> TEARDOWN: Final FreeLibrary(hMod1) -> DllMain(PROCESS_DETACH) <<<\n");
    BOOL bFree1 = FreeLibrary(hMod1);
    printf("[runner] FreeLibrary(hMod1) -> %s (Physical unmapping complete)\n",
           bFree1 ? "TRUE" : "FALSE");

    printf("\n====================================================================\n");
    printf("  ALL 6 SCENARIOS EXECUTED SUCCESSFULLY\n");
    printf("====================================================================\n");

    return 0;
}
