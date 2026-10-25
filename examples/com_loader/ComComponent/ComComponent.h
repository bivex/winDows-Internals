// ============================================================================
// ComComponent.h — Classic Native In-Process COM Component Interface & Declarations
// ============================================================================

#pragma once

#include <windows.h>
#include <unknwn.h>

// ----------------------------------------------------------------------------
// GUID Definitions
// CLSID_Calculator : {E5A20C21-48C7-4E26-9635-4D6F6A6E0001}
// IID_ICalculator  : {B4D10F32-7A9B-4C18-8A5E-2C3D4E5F6A7B}
// ----------------------------------------------------------------------------

static const GUID CLSID_Calculator =
    { 0xE5A20C21, 0x48C7, 0x4E26, { 0x96, 0x35, 0x4D, 0x6F, 0x6A, 0x6E, 0x00, 0x01 } };

static const GUID IID_ICalculator =
    { 0xB4D10F32, 0x7A9B, 0x4C18, { 0x8A, 0x5E, 0x2C, 0x3D, 0x4E, 0x5F, 0x6A, 0x7B } };

// ----------------------------------------------------------------------------
// ICalculator Interface
// ----------------------------------------------------------------------------
struct __declspec(uuid("B4D10F32-7A9B-4C18-8A5E-2C3D4E5F6A7B"))
ICalculator : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Add(int a, int b, int* result) = 0;
    virtual HRESULT STDMETHODCALLTYPE Subtract(int a, int b, int* result) = 0;
    virtual HRESULT STDMETHODCALLTYPE Multiply(int a, int b, int* result) = 0;
    virtual HRESULT STDMETHODCALLTYPE Divide(int a, int b, int* result) = 0;
    virtual HRESULT STDMETHODCALLTYPE ShowGuiDialog(int* dialogResult) = 0;
};

extern "C" {
    HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv);
    HRESULT WINAPI DllCanUnloadNow(void);
    HRESULT WINAPI DllRegisterServer(void);
    HRESULT WINAPI DllUnregisterServer(void);
    DWORD   WINAPI ComWorkerRoutine(LPVOID lpParam);
}
