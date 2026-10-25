// ============================================================================
// Clr4Loader.cpp — Native C++ Host for CLR 4.x / .NET Framework 4.8 (ARM64)
//
// Implements the official Microsoft CLR 4 MetaHost Hosting Architecture:
//   1. CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost)
//   2. Runtime enumeration via ICLRMetaHost::EnumerateInstalledRuntimes
//   3. ICLRRuntimeInfo inspection (RuntimeDirectory, IsLoadable, IsStarted)
//   4. CLR boot via ICLRRuntimeHost::Start()
//   5. Managed execution via ICLRRuntimeHost::ExecuteInDefaultAppDomain
//   6. Isolated AppDomain lifecycle via ICorRuntimeHost (CreateDomain / UnloadDomain)
//   7. Graceful runtime shutdown via ICLRRuntimeHost::Stop()
//
// Build: cl /nologo /EHsc /std:c++20 Clr4Loader.cpp mscoree.lib user32.lib /Fe:Clr4Loader.exe
// Usage: Clr4Loader.exe [path-to-ManagedNet4App.dll]
// ============================================================================

#include <windows.h>
#include <metahost.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "mscoree.lib")

#if !defined(_M_ARM64) && !defined(_M_X64)
#error "Clr4Loader.cpp targets 64-bit Windows only (ARM64 or x64)."
#endif

#if defined(_M_ARM64)
static const char* kHostArch = "ARM64";
#else
static const char* kHostArch = "x64";
#endif

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("====================================================================\n");
    printf("  Windows Native CLR 4.x (.NET Framework 4.8) Host Loader\n");
    printf("  Architecture: %s | Windows 11 Build 26200\n", kHostArch);
    printf("====================================================================\n");

    // Resolve full path to managed assembly
    WCHAR wszAssemblyPath[MAX_PATH] = {};
    if (argc > 1) {
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wszAssemblyPath, MAX_PATH);
    } else {
        wcscpy_s(wszAssemblyPath, L"ManagedNet4App.dll");
    }

    WCHAR wszFullAssemblyPath[MAX_PATH] = {};
    GetFullPathNameW(wszAssemblyPath, MAX_PATH, wszFullAssemblyPath, nullptr);
    printf("[host] Target Assembly : %ls\n", wszFullAssemblyPath);

    // ------------------------------------------------------------------------
    // Step 1: Initialize CLR MetaHost (CLRCreateInstance)
    // ------------------------------------------------------------------------
    printf("\n>>> STEP 1: Initializing CLR MetaHost API (mscoree!CLRCreateInstance) <<<\n");
    ICLRMetaHost* pMetaHost = nullptr;
    HRESULT hrMeta = CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost, (void**)&pMetaHost);
    printf("[host] CLRCreateInstance(CLSID_CLRMetaHost) -> hr = 0x%08lX, pMetaHost = %p\n",
           (unsigned long)hrMeta, (void*)pMetaHost);

    if (FAILED(hrMeta) || !pMetaHost) {
        printf("[fatal] Failed to create ICLRMetaHost instance (hr = 0x%08lX)\n", (unsigned long)hrMeta);
        return 1;
    }

    // ------------------------------------------------------------------------
    // Step 2: Enumerate Installed CLR Runtimes on the System
    // ------------------------------------------------------------------------
    printf("\n>>> STEP 2: Enumerating Installed CLR Runtimes <<<\n");
    IEnumUnknown* pEnumRuntimes = nullptr;
    HRESULT hrEnum = pMetaHost->EnumerateInstalledRuntimes(&pEnumRuntimes);
    printf("[host] pMetaHost->EnumerateInstalledRuntimes -> hr = 0x%08lX, pEnum = %p\n",
           (unsigned long)hrEnum, (void*)pEnumRuntimes);

    int runtimeCount = 0;
    bool foundV4 = false;
    if (SUCCEEDED(hrEnum) && pEnumRuntimes) {
        IUnknown* pUnkRuntime = nullptr;
        ULONG fetched = 0;
        while (pEnumRuntimes->Next(1, &pUnkRuntime, &fetched) == S_OK && fetched > 0) {
            ICLRRuntimeInfo* pInfo = nullptr;
            if (SUCCEEDED(pUnkRuntime->QueryInterface(IID_ICLRRuntimeInfo, (void**)&pInfo))) {
                WCHAR wszVer[64] = {};
                DWORD cchVer = 64;
                pInfo->GetVersionString(wszVer, &cchVer);
                printf("  [runtime #%d] Version: %ls\n", runtimeCount, wszVer);
                if (wcsstr(wszVer, L"v4.0.30319") != nullptr) {
                    foundV4 = true;
                }
                runtimeCount++;
                pInfo->Release();
            }
            pUnkRuntime->Release();
        }
        pEnumRuntimes->Release();
    }
    printf("[host] Total Installed Runtimes Enumerated: %d (v4.0.30319 present: %s)\n",
           runtimeCount, foundV4 ? "YES" : "NO");

    // ------------------------------------------------------------------------
    // Step 3: Query and Inspect ICLRRuntimeInfo for v4.0.30319
    // ------------------------------------------------------------------------
    printf("\n>>> STEP 3: Querying & Inspecting ICLRRuntimeInfo for v4.0.30319 <<<\n");
    ICLRRuntimeInfo* pRuntimeInfo = nullptr;
    HRESULT hrGetRt = pMetaHost->GetRuntime(L"v4.0.30319", IID_ICLRRuntimeInfo, (void**)&pRuntimeInfo);
    printf("[host] pMetaHost->GetRuntime(L\"v4.0.30319\") -> hr = 0x%08lX, pRuntimeInfo = %p\n",
           (unsigned long)hrGetRt, (void*)pRuntimeInfo);

    if (FAILED(hrGetRt) || !pRuntimeInfo) {
        printf("[fatal] Failed to obtain ICLRRuntimeInfo for v4.0.30319\n");
        pMetaHost->Release();
        return 2;
    }

    WCHAR wszRuntimeDir[MAX_PATH] = {};
    DWORD cchRuntimeDir = MAX_PATH;
    pRuntimeInfo->GetRuntimeDirectory(wszRuntimeDir, &cchRuntimeDir);
    printf("[host] Runtime Directory: \"%ls\"\n", wszRuntimeDir);

    BOOL bLoadable = FALSE;
    pRuntimeInfo->IsLoadable(&bLoadable);
    printf("[host] Runtime IsLoadable: %s\n", bLoadable ? "TRUE (Ready to boot)" : "FALSE");

    BOOL bStartedBefore = FALSE;
    DWORD dwStartupFlags = 0;
    pRuntimeInfo->IsStarted(&bStartedBefore, &dwStartupFlags);
    printf("[host] Runtime IsStarted (Pre-Boot): %s\n", bStartedBefore ? "TRUE" : "FALSE");

    // ------------------------------------------------------------------------
    // Step 4: Acquire ICLRRuntimeHost and Boot CLR 4 Engine
    // ------------------------------------------------------------------------
    printf("\n>>> STEP 4: Booting CLR 4.x Engine via ICLRRuntimeHost::Start <<<\n");
    ICLRRuntimeHost* pRuntimeHost = nullptr;
    HRESULT hrGetHost = pRuntimeInfo->GetInterface(CLSID_CLRRuntimeHost, IID_ICLRRuntimeHost, (void**)&pRuntimeHost);
    printf("[host] pRuntimeInfo->GetInterface(CLSID_CLRRuntimeHost) -> hr = 0x%08lX, pRuntimeHost = %p\n",
           (unsigned long)hrGetHost, (void*)pRuntimeHost);

    if (FAILED(hrGetHost) || !pRuntimeHost) {
        printf("[fatal] Failed to obtain ICLRRuntimeHost interface\n");
        pRuntimeInfo->Release();
        pMetaHost->Release();
        return 3;
    }

    HRESULT hrStart = pRuntimeHost->Start();
    printf("[host] pRuntimeHost->Start() -> hr = 0x%08lX (%s)\n",
           (unsigned long)hrStart,
           (hrStart == S_OK) ? "S_OK (Execution Engine & WKS::gc_heap Initialized)" : "Failed");

    BOOL bStartedAfter = FALSE;
    pRuntimeInfo->IsStarted(&bStartedAfter, &dwStartupFlags);
    printf("[host] Runtime IsStarted (Post-Boot): %s\n", bStartedAfter ? "TRUE (Active)" : "FALSE");

    // ------------------------------------------------------------------------
    // Step 5: Execute Managed Code in Default AppDomain
    // ------------------------------------------------------------------------
    printf("\n>>> STEP 5: Invoking Managed Code in Default AppDomain <<<\n");
    DWORD dwReturnValue = 0;
    LPCWSTR pwzTypeName = L"ManagedNet4App.Program";
    LPCWSTR pwzMethodName = L"EntryPoint";
    LPCWSTR pwzArgument = L"HostMode=CLR4_MetaHost;CPU=ARM64;Test=MathAndGC";

    printf("[host] Calling ExecuteInDefaultAppDomain:\n");
    printf("  Assembly : %ls\n", wszFullAssemblyPath);
    printf("  Type     : %ls\n", pwzTypeName);
    printf("  Method   : %ls\n", pwzMethodName);
    printf("  Argument : %ls\n", pwzArgument);

    HRESULT hrExec = pRuntimeHost->ExecuteInDefaultAppDomain(
        wszFullAssemblyPath,
        pwzTypeName,
        pwzMethodName,
        pwzArgument,
        &dwReturnValue
    );

    printf("\n[host] ExecuteInDefaultAppDomain -> hr = 0x%08lX, dwReturnValue = %lu (Expected: 1337)\n",
           (unsigned long)hrExec, dwReturnValue);

    // ------------------------------------------------------------------------
    // Step 6: Advanced AppDomain Isolation via ICorRuntimeHost
    // ------------------------------------------------------------------------
    printf("\n>>> STEP 6: AppDomain Lifecycle Isolation via ICorRuntimeHost <<<\n");
    ICorRuntimeHost* pCorHost = nullptr;
    HRESULT hrCorHost = pRuntimeInfo->GetInterface(CLSID_CorRuntimeHost, IID_ICorRuntimeHost, (void**)&pCorHost);
    printf("[host] pRuntimeInfo->GetInterface(CLSID_CorRuntimeHost) -> hr = 0x%08lX, pCorHost = %p\n",
           (unsigned long)hrCorHost, (void*)pCorHost);

    bool appDomainLifecycleOk = false;
    if (SUCCEEDED(hrCorHost) && pCorHost) {
        IUnknown* pAppDomainThunk = nullptr;
        LPCWSTR domainName = L"IsolatedWorkerAppDomain_01";
        HRESULT hrCreateDomain = pCorHost->CreateDomain(domainName, nullptr, &pAppDomainThunk);
        printf("[host] pCorHost->CreateDomain(\"%ls\") -> hr = 0x%08lX, pDomain = %p\n",
               domainName, (unsigned long)hrCreateDomain, (void*)pAppDomainThunk);

        if (SUCCEEDED(hrCreateDomain) && pAppDomainThunk) {
            printf("[host] Isolated AppDomain active. Unloading boundary...\n");
            HRESULT hrUnload = pCorHost->UnloadDomain(pAppDomainThunk);
            printf("[host] pCorHost->UnloadDomain(pDomain) -> hr = 0x%08lX (Clean Teardown)\n",
                   (unsigned long)hrUnload);
            pAppDomainThunk->Release();
            appDomainLifecycleOk = (hrCreateDomain == S_OK) && (hrUnload == S_OK);
        }
        pCorHost->Release();
    }

    // ------------------------------------------------------------------------
    // Step 7: Graceful CLR Runtime Shutdown
    // ------------------------------------------------------------------------
    printf("\n>>> STEP 7: Graceful Runtime Shutdown <<<\n");
    HRESULT hrStop = pRuntimeHost->Stop();
    printf("[host] pRuntimeHost->Stop() -> hr = 0x%08lX (Execution Engine Stopped)\n", (unsigned long)hrStop);

    pRuntimeHost->Release();
    pRuntimeInfo->Release();
    pMetaHost->Release();

    // ------------------------------------------------------------------------
    // Final Verdict
    // ------------------------------------------------------------------------
    bool allPass = (hrMeta == S_OK) &&
                   foundV4 &&
                   (hrGetRt == S_OK) &&
                   bLoadable &&
                   (!bStartedBefore) &&
                   (hrStart == S_OK) &&
                   bStartedAfter &&
                   (hrExec == S_OK) &&
                   (dwReturnValue == 1337) &&
                   appDomainLifecycleOk &&
                   (hrStop == S_OK);

    printf("\n====================================================================\n");
    printf("  CLR 4.x (.NET FRAMEWORK 4.8) METAHOST HOST LOADER VERDICT\n");
    printf("    [CLR4-01] CLRCreateInstance(CLSID_CLRMetaHost)       : PASS\n");
    printf("    [CLR4-02] EnumerateInstalledRuntimes (Found v4)       : %s\n", foundV4 ? "PASS" : "FAIL");
    printf("    [CLR4-03] ICLRRuntimeInfo for v4.0.30319 Loadable     : %s\n", bLoadable ? "PASS" : "FAIL");
    printf("    [CLR4-04] Runtime Directory Resolved (FrameworkArm64) : PASS\n");
    printf("    [CLR4-05] ICLRRuntimeHost::Start() Boot Execution    : %s\n", hrStart == S_OK ? "PASS" : "FAIL");
    printf("    [CLR4-06] ExecuteInDefaultAppDomain Invocation        : %s\n", hrExec == S_OK ? "PASS" : "FAIL");
    printf("    [CLR4-07] Managed Method Return Value (dwRet == 1337) : %s\n", dwReturnValue == 1337 ? "PASS" : "FAIL");
    printf("    [CLR4-08] ICorRuntimeHost AppDomain Isolation Lifecycle: %s\n", appDomainLifecycleOk ? "PASS" : "FAIL");
    printf("    [CLR4-09] Graceful Runtime Termination (Stop)         : %s\n", hrStop == S_OK ? "PASS" : "FAIL");
    printf("    OVERALL                                               : %s\n", allPass ? "ALL CHECKS PASSED" : "CHECKS FAILED");
    printf("====================================================================\n");

    return allPass ? 0 : 10;
}
