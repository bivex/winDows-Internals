// ============================================================================
// event_callbacks.h — DbgEng Event Sink (module/thread/exception lifecycle)
//
// Registers an IDebugEventCallbacks implementation so the correlation engine
// receives engine-level lifecycle events (LoadModule / UnloadModule /
// ExitThread / Exception) instead of relying on manual registration.
// ============================================================================

#pragma once
#include <windows.h>
#include <dbgeng.h>

class PeTraceEventSink : public IDebugEventCallbacks {
public:
    static PeTraceEventSink& Instance();

    HRESULT Register(PDEBUG_CLIENT client);
    HRESULT Unregister();

    // --- IUnknown ---
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override;
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    // --- IDebugEventCallbacks ---
    STDMETHODIMP GetInterestMask(PULONG Mask) override;
    STDMETHODIMP Breakpoint(PDEBUG_BREAKPOINT Bp) override;
    STDMETHODIMP ChangeDebuggeeState(ULONG Flags, ULONG64 Argument) override;
    STDMETHODIMP ChangeEngineState(ULONG Flags, ULONG64 Argument) override;
    STDMETHODIMP ChangeSymbolState(ULONG Flags, ULONG64 Argument) override;
    STDMETHODIMP CreateProcess(ULONG64 ImageFileHandle, ULONG64 Handle, ULONG64 BaseOffset,
                               ULONG ModuleSize, PCSTR ModuleName, PCSTR ImageName,
                               ULONG CheckSum, ULONG TimeDateStamp,
                               ULONG64 InitialThreadHandle, ULONG64 ThreadDataOffset,
                               ULONG64 ThreadStartOffset) override;
    STDMETHODIMP CreateThread(ULONG64 Handle, ULONG64 DataOffset, ULONG64 StartOffset) override;
    STDMETHODIMP Exception(PEXCEPTION_RECORD64 Exception, ULONG FirstChance) override;
    STDMETHODIMP ExitProcess(ULONG ExitCode) override;
    STDMETHODIMP ExitThread(ULONG ExitCode) override;
    STDMETHODIMP LoadModule(ULONG64 ImageFileHandle, ULONG64 BaseOffset, ULONG ModuleSize,
                            PCSTR ModuleName, PCSTR ImageName, ULONG CheckSum, ULONG TimeDateStamp) override;
    STDMETHODIMP SessionStatus(ULONG Status) override;
    STDMETHODIMP SystemError(ULONG Error, ULONG Level) override;
    STDMETHODIMP UnloadModule(PCSTR ImageBaseName, ULONG64 BaseOffset) override;

private:
    PeTraceEventSink() = default;
    PDEBUG_CLIENT m_client = nullptr;
    void Emit(const char* fmt, ...);
    uint32_t CurrentSystemTid();
};
