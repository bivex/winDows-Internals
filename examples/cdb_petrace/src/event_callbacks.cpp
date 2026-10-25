// ============================================================================
// event_callbacks.cpp — DbgEng Event Sink Implementation
// ============================================================================

#include "event_callbacks.h"
#include "correlator.h"
#include <cstdarg>
#include <cstdio>

PeTraceEventSink& PeTraceEventSink::Instance() {
    static PeTraceEventSink sink;
    return sink;
}

HRESULT PeTraceEventSink::Register(PDEBUG_CLIENT client) {
    if (!client) return E_POINTER;
    if (m_client) {
        if (m_client == client) return S_OK;   // already registered on this client
        m_client->SetEventCallbacks(nullptr); // switch to the new client
        m_client->Release();
        m_client = nullptr;
    }
    // Keep the exact interface pointer the engine handed us (no QI): the sink
    // must be registered against the same client identity that runs commands.
    m_client = client;
    m_client->AddRef();
    return m_client->SetEventCallbacks(this);
}

HRESULT PeTraceEventSink::Unregister() {
    if (!m_client) return S_OK;
    HRESULT hr = m_client->SetEventCallbacks(nullptr);
    m_client->Release();
    m_client = nullptr;
    return hr;
}

void PeTraceEventSink::Emit(const char* fmt, ...) {
    if (!m_client) return;
    IDebugControl4* ctl = nullptr;
    if (SUCCEEDED(m_client->QueryInterface(__uuidof(IDebugControl4), (void**)&ctl)) && ctl) {
        char buf[512] = {};
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        ctl->Output(DEBUG_OUTPUT_NORMAL, "%s", buf);
        ctl->Release();
    }
}

uint32_t PeTraceEventSink::CurrentSystemTid() {
    if (!m_client) return 0;
    IDebugSystemObjects* so = nullptr;
    ULONG tid = 0;
    if (SUCCEEDED(m_client->QueryInterface(__uuidof(IDebugSystemObjects), (void**)&so)) && so) {
        so->GetCurrentThreadSystemId(&tid);   // REAL OS thread id, not engine handle
        so->Release();
    }
    return tid;
}

HRESULT PeTraceEventSink::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDebugEventCallbacks)) {
        *ppvObject = static_cast<IDebugEventCallbacks*>(this);
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

HRESULT PeTraceEventSink::GetInterestMask(PULONG Mask) {
    if (!Mask) return E_POINTER;
    // NOTE: DEBUG_EVENT_BREAKPOINT intentionally excluded — breakpoint command
    // strings ("!peonhit; g") own breakpoint reporting; a sink callback here
    // would double-log every trap.
    *Mask = DEBUG_EVENT_LOAD_MODULE | DEBUG_EVENT_UNLOAD_MODULE |
            DEBUG_EVENT_CREATE_THREAD | DEBUG_EVENT_EXIT_THREAD |
            DEBUG_EVENT_EXCEPTION | DEBUG_EVENT_SESSION_STATUS;
    return S_OK;
}

HRESULT PeTraceEventSink::Breakpoint(PDEBUG_BREAKPOINT) { return DEBUG_STATUS_NO_CHANGE; }
HRESULT PeTraceEventSink::ChangeDebuggeeState(ULONG, ULONG64) { return DEBUG_STATUS_NO_CHANGE; }
HRESULT PeTraceEventSink::ChangeEngineState(ULONG, ULONG64) { return DEBUG_STATUS_NO_CHANGE; }
HRESULT PeTraceEventSink::ChangeSymbolState(ULONG, ULONG64) { return DEBUG_STATUS_NO_CHANGE; }
HRESULT PeTraceEventSink::SessionStatus(ULONG) { return DEBUG_STATUS_NO_CHANGE; }
HRESULT PeTraceEventSink::SystemError(ULONG, ULONG) { return DEBUG_STATUS_NO_CHANGE; }

HRESULT PeTraceEventSink::CreateProcess(ULONG64, ULONG64, ULONG64, ULONG, PCSTR,
                                        PCSTR, ULONG, ULONG, ULONG64, ULONG64, ULONG64) {
    return DEBUG_STATUS_NO_CHANGE;
}

HRESULT PeTraceEventSink::CreateThread(ULONG64, ULONG64, ULONG64 StartOffset) {
    Emit("[sink] CREATE_THREAD start=0x%016llX\n", (unsigned long long)StartOffset);
    return DEBUG_STATUS_NO_CHANGE;
}
HRESULT PeTraceEventSink::ExitProcess(ULONG ExitCode) {
    Emit("[sink] EXIT_PROCESS code=0x%08lX\n", (unsigned long)ExitCode);
    return DEBUG_STATUS_NO_CHANGE;
}


HRESULT PeTraceEventSink::ExitThread(ULONG) {
    uint32_t tid = CurrentSystemTid();
    InvocationCorrelator::Instance().OnThreadExit(tid);
    Emit("[sink] EXIT_THREAD tid=%u (in-flight call state invalidated)\n", tid);
    return DEBUG_STATUS_NO_CHANGE;
}

HRESULT PeTraceEventSink::Exception(PEXCEPTION_RECORD64 Exception, ULONG FirstChance) {
    uint32_t tid = CurrentSystemTid();
    // An unwind crossing our in-flight call makes its pending LR return-trap
    // meaningless — drop it so the chain is not corrupted by a bogus return.
    InvocationCorrelator::Instance().OnException(tid);
    Emit("[sink] EXCEPTION code=0x%08lX addr=0x%016llX tid=%u first=%lu\n",
         (unsigned long)Exception->ExceptionCode,
         (unsigned long long)Exception->ExceptionAddress, tid, FirstChance);
    return DEBUG_STATUS_NO_CHANGE;
}

HRESULT PeTraceEventSink::LoadModule(ULONG64, ULONG64 BaseOffset, ULONG ModuleSize,
                                     PCSTR ModuleName, PCSTR ImageName, ULONG, ULONG) {
    const char* src = ImageName ? ImageName : (ModuleName ? ModuleName : "<unknown>");
    std::string name(src);
    size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    InvocationCorrelator::Instance().RegisterModule(BaseOffset, ModuleSize, name);
    Emit("[sink] IMAGE_LOAD base=0x%016llX size=0x%lX %s\n",
         (unsigned long long)BaseOffset, ModuleSize, name.c_str());
    return DEBUG_STATUS_NO_CHANGE;
}

HRESULT PeTraceEventSink::UnloadModule(PCSTR ImageBaseName, ULONG64 BaseOffset) {
    std::string name = ImageBaseName ? ImageBaseName : "<unknown>";
    InvocationCorrelator::Instance().UnregisterModule(BaseOffset);
    Emit("[sink] IMAGE_UNLOAD base=0x%016llX %s\n",
         (unsigned long long)BaseOffset, name.c_str());
    return DEBUG_STATUS_NO_CHANGE;
}
