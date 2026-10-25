// ============================================================================
// ComComponent.cpp — Native In-Process COM Component Implementation
//
// Implements:
//   - ICalculator interface
//   - IClassFactory interface
//   - DllGetClassObject, DllCanUnloadNow, DllMain
//
// Build: cl /nologo /LD /EHsc ComComponent.cpp user32.lib /Fe:ComComponent.dll
// ============================================================================

#include "ComComponent.h"
#include <stdio.h>
#include <atomic>

// ----------------------------------------------------------------------------
// Global Server Lock Counter
// ----------------------------------------------------------------------------
static std::atomic<long> g_serverLocks(0);
static HMODULE g_hModule = nullptr;

// Optional MessageBoxTimeoutW prototype
extern "C" {
    int WINAPI MessageBoxTimeoutW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption,
                                  UINT uType, WORD wLanguageId, DWORD dwMilliseconds);
}

// ----------------------------------------------------------------------------
// Calculator Implementation
// ----------------------------------------------------------------------------
class Calculator : public ICalculator {
public:
    Calculator() : refCount_(1) {
        g_serverLocks.fetch_add(1);
        printf("  [ComComponent!Calculator] Object created (this=%p, locks=%ld)\n",
               (void*)this, g_serverLocks.load());
    }

    virtual ~Calculator() {
        g_serverLocks.fetch_sub(1);
        printf("  [ComComponent!Calculator] Object destroyed (this=%p, locks=%ld)\n",
               (void*)this, g_serverLocks.load());
    }

    // IUnknown Methods
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;

        if (riid == IID_IUnknown || riid == IID_ICalculator) {
            *ppvObject = static_cast<ICalculator*>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return (ULONG)refCount_.fetch_add(1) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = (ULONG)refCount_.fetch_sub(1) - 1;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // ICalculator Methods
    HRESULT STDMETHODCALLTYPE Add(int a, int b, int* result) override {
        if (!result) return E_POINTER;
        *result = a + b;
        printf("  [ComComponent!Add] %d + %d = %d\n", a, b, *result);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Subtract(int a, int b, int* result) override {
        if (!result) return E_POINTER;
        *result = a - b;
        printf("  [ComComponent!Subtract] %d - %d = %d\n", a, b, *result);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Multiply(int a, int b, int* result) override {
        if (!result) return E_POINTER;
        *result = a * b;
        printf("  [ComComponent!Multiply] %d * %d = %d\n", a, b, *result);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Divide(int a, int b, int* result) override {
        if (!result) return E_POINTER;
        if (b == 0) return E_INVALIDARG;
        *result = a / b;
        printf("  [ComComponent!Divide] %d / %d = %d\n", a, b, *result);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ShowGuiDialog(int* dialogResult) override {
        if (!dialogResult) return E_POINTER;
        printf("  [ComComponent!ShowGuiDialog] Displaying native Win32 GUI MessageBox...\n");

        LPCWSTR title = L"Native COM Component (ARM64)";
        LPCWSTR message = L"Hello from Native Classic In-Process COM!\n\n"
                          L"Component Model : Microsoft Component Object Model (COM)\n"
                          L"Interface       : ICalculator (uuid: B4D10F32-7A9B-4C18-8A5E-2C3D4E5F6A7B)\n"
                          L"Class Factory   : IClassFactory::CreateInstance\n"
                          L"Hosting Modes   : Registration-Free COM (SxS) & Manual PE Loader\n\n"
                          L"Click OK or wait 60 seconds for auto-close.";

        int res = MessageBoxTimeoutW(nullptr, message, title,
                                     MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND | MB_SYSTEMMODAL,
                                     0, 60000);
        *dialogResult = res;
        printf("  [ComComponent!ShowGuiDialog] DialogResult = %d\n", res);
        return S_OK;
    }

private:
    std::atomic<long> refCount_;
};

// ----------------------------------------------------------------------------
// CalculatorClassFactory Implementation
// ----------------------------------------------------------------------------
class CalculatorClassFactory : public IClassFactory {
public:
    CalculatorClassFactory() : refCount_(1) {
        g_serverLocks.fetch_add(1);
    }

    virtual ~CalculatorClassFactory() {
        g_serverLocks.fetch_sub(1);
    }

    // IUnknown Methods
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;

        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppvObject = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return (ULONG)refCount_.fetch_add(1) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = (ULONG)refCount_.fetch_sub(1) - 1;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // IClassFactory Methods
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;

        // In-process aggregation is not supported in this sample
        if (pUnkOuter != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }

        Calculator* pCalc = new (std::nothrow) Calculator();
        if (!pCalc) return E_OUTOFMEMORY;

        HRESULT hr = pCalc->QueryInterface(riid, ppvObject);
        pCalc->Release(); // Counterbalance initial ref count of 1
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override {
        if (fLock) {
            g_serverLocks.fetch_add(1);
        } else {
            g_serverLocks.fetch_sub(1);
        }
        return S_OK;
    }

private:
    std::atomic<long> refCount_;
};

// ----------------------------------------------------------------------------
// DllMain Entry Point
// ----------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        g_hModule = (HMODULE)hinstDLL;
        printf("[ComComponent!DllMain] DLL_PROCESS_ATTACH initialized (hModule=%p)\n", (void*)g_hModule);
        break;
    case DLL_PROCESS_DETACH:
        printf("[ComComponent!DllMain] DLL_PROCESS_DETACH finalized\n");
        break;
    }
    return TRUE;
}

// ----------------------------------------------------------------------------
// In-Process COM Server Standard Exports
// ----------------------------------------------------------------------------

#pragma comment(linker, "/EXPORT:DllGetClassObject")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")
#pragma comment(linker, "/EXPORT:DllRegisterServer,PRIVATE")
#pragma comment(linker, "/EXPORT:DllUnregisterServer,PRIVATE")
#pragma comment(linker, "/EXPORT:ComWorkerRoutine")
#pragma comment(linker, "/EXPORT:GetPrivateApcRoutinePointer")
#pragma comment(linker, "/EXPORT:ComSpecialApcRoutine")

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (rclsid != CLSID_Calculator) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    CalculatorClassFactory* pFactory = new (std::nothrow) CalculatorClassFactory();
    if (!pFactory) return E_OUTOFMEMORY;

    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release();
    return hr;
}

extern "C" HRESULT WINAPI DllCanUnloadNow(void) {
    return (g_serverLocks.load() == 0) ? S_OK : S_FALSE;
}

extern "C" HRESULT WINAPI DllRegisterServer(void) {
    // Intentionally empty: registration-free COM requires no registry writes
    return S_OK;
}

extern "C" HRESULT WINAPI DllUnregisterServer(void) {
    return S_OK;
}

extern "C" DWORD WINAPI ComWorkerRoutine(LPVOID lpParam) {
    printf("[ComComponent!ComWorkerRoutine] Worker thread entered! (TID=%lu, param=%p)\n",
           GetCurrentThreadId(), lpParam);
    return 42;
}

// ----------------------------------------------------------------------------
// Exotic Vector: Private Unexported Special User APC Routine
// ----------------------------------------------------------------------------
extern "C" void CALLBACK ComSpecialApcRoutine(ULONG_PTR dwData) {
    printf("[ComComponent!ComSpecialApcRoutine] >>> EXOTIC SPECIAL USER APC DELIVERED! <<<\n");
    printf("  Target Function : ComSpecialApcRoutine (VA: %p)\n", (void*)&ComSpecialApcRoutine);
    printf("  Mechanism       : QueueUserAPC2 -> ntdll!KiUserApcDispatcher\n");
    printf("  Interrupted Loop: Strictly non-alertable Sleep(10) interrupted mid-flight!\n");
    printf("  Argument dwData : 0x%llX\n", (unsigned long long)dwData);
    printf("  Current Thread  : TID %lu\n", GetCurrentThreadId());
}

extern "C" PAPCFUNC WINAPI GetPrivateApcRoutinePointer(void) {
    return (PAPCFUNC)&ComSpecialApcRoutine;
}
