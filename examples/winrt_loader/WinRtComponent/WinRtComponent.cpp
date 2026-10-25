// ============================================================================
// WinRtComponent.cpp — Modern C++/WinRT In-Process Component Implementation
//
// Implements:
//   1. Modern C++/WinRT class using winrt::implements & winrt::make
//   2. Windows Runtime activation factory (IActivationFactory)
//   3. Standard WinRT exported entry points:
//        - DllGetActivationFactory
//        - DllCanUnloadNow
//   4. Native Win32 GUI dialog via user32!MessageBoxTimeoutW
//
// Build: cl /nologo /LD /EHsc /std:c++20 WinRtComponent.cpp windowsapp.lib user32.lib /Fe:WinRtComponent.dll
#include <unknwn.h>
#include <inspectable.h>
#include <activation.h>
#include <winstring.h>
#include "WinRtComponent.h"
#include <winrt/base.h>
#include <stdio.h>
#include <windows.h>

extern "C" {
    int WINAPI MessageBoxTimeoutW(
        HWND hWnd,
        LPCWSTR lpText,
        LPCWSTR lpCaption,
        UINT uType,
        WORD wLanguageId,
        DWORD dwMilliseconds);
}

// ----------------------------------------------------------------------------
// WinRtCalculator Implementation
// ----------------------------------------------------------------------------
struct WinRtCalculator : winrt::implements<WinRtCalculator, IWinRtCalculator> {
    int32_t __stdcall Add(int32_t a, int32_t b) noexcept override {
        printf("  [WinRtComponent!Add] %d + %d = %d\n", a, b, a + b);
        return a + b;
    }

    int32_t __stdcall Compute(int32_t x) noexcept override {
        printf("  [WinRtComponent!Compute] %d * 2 = %d\n", x, x * 2);
        return x * 2;
    }

    int32_t __stdcall Factorial(int32_t n) noexcept override {
        int32_t res = 1;
        for (int32_t i = 2; i <= n; ++i) res *= i;
        printf("  [WinRtComponent!Factorial] %d! = %d\n", n, res);
        return res;
    }

    int32_t __stdcall ShowGuiDialog() noexcept override {
        printf("  [WinRtComponent!ShowGuiDialog] Displaying native WinRT GUI MessageBox...\n");

        LPCWSTR title = L"C++/WinRT Manual Loader (ARM64)";
        LPCWSTR message =
            L"Hello from modern C++/WinRT running inside a Manual PE Loader!\n\n"
            L"Host Loader     : WinRtLoader.exe (Native C++)\n"
            L"Component Model : C++/WinRT 2.0 (Modern C++ Projection)\n"
            L"Activation Path : DllGetActivationFactory -> IActivationFactory\n"
            L"Interface ABI   : IInspectable -> IWinRtCalculator\n"
            L"Apartment State : RO_INIT_MULTITHREADED (combase.dll)\n\n"
            L"Click OK to close (or auto-closes in 30 seconds).";

        int res = MessageBoxTimeoutW(
            nullptr,
            message,
            title,
            MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND,
            0,
            30000);

        printf("  [WinRtComponent!ShowGuiDialog] DialogResult = %d\n", res);
        return res;
    }

    winrt::hstring GetRuntimeClassName() const noexcept {
        return L"WinRtComponent.Calculator";
    }
};

// ----------------------------------------------------------------------------
// WinRtCalculatorFactory (IActivationFactory implementation)
// ----------------------------------------------------------------------------
struct WinRtCalculatorFactory : winrt::implements<WinRtCalculatorFactory, ::IActivationFactory> {
    HRESULT __stdcall ActivateInstance(::IInspectable** instance) noexcept override {
        if (!instance) return E_POINTER;
        auto calc = winrt::make<WinRtCalculator>();
        *instance = (::IInspectable*)calc.detach();
        return S_OK;
    }
};
#pragma comment(linker, "/EXPORT:DllGetActivationFactory")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow")

STDAPI DllGetActivationFactory(
    HSTRING activatableClassId,
    void** factory)
{
    if (!factory) return E_POINTER;
    *factory = nullptr;

    UINT32 length = 0;
    PCWSTR buffer = WindowsGetStringRawBuffer(activatableClassId, &length);
    if (!buffer) return E_INVALIDARG;

    wprintf(L"[WinRtComponent!DllGetActivationFactory] Querying class ID: \"%ls\"\n", buffer);

    if (wcscmp(buffer, L"WinRtComponent.Calculator") == 0) {
        auto fact = winrt::make<WinRtCalculatorFactory>();
        *factory = (::IUnknown*)fact.detach();
        printf("[WinRtComponent!DllGetActivationFactory] Factory successfully instantiated: %p\n", *factory);
        return S_OK;
    }

    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}

// ----------------------------------------------------------------------------
// Standard DLL Entry Point
// ----------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    (void)hinstDLL;
    (void)lpReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        printf("[WinRtComponent!DllMain] DLL_PROCESS_ATTACH initialized\n");
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        printf("[WinRtComponent!DllMain] DLL_PROCESS_DETACH finalized\n");
    }
    return TRUE;
}
