// ============================================================================
// WinRtComponent.h — C++/WinRT In-Process Component Interfaces
//
// Defines modern C++/WinRT interfaces conforming to Windows Runtime COM ABI:
//   - IInspectable inheritance
//   - IActivationFactory activation contract
//   - DllGetActivationFactory export signature
// ============================================================================

#pragma once

#include <unknwn.h>
#include <inspectable.h>
#include <winnt.h>

// {6A2F1D3C-4E5B-4C2A-9E8F-1B3D5A7C9E01}
static constexpr GUID IID_IWinRtCalculator = {
    0x6a2f1d3c, 0x4e5b, 0x4c2a, {0x9e, 0x8f, 0x1b, 0x3d, 0x5a, 0x7c, 0x9e, 0x01}
};

struct __declspec(uuid("6A2F1D3C-4E5B-4C2A-9E8F-1B3D5A7C9E01")) __declspec(novtable)
IWinRtCalculator : ::IInspectable {
    virtual int32_t __stdcall Add(int32_t a, int32_t b) noexcept = 0;
    virtual int32_t __stdcall Compute(int32_t x) noexcept = 0;
    virtual int32_t __stdcall Factorial(int32_t n) noexcept = 0;
    virtual int32_t __stdcall ShowGuiDialog() noexcept = 0;
};
