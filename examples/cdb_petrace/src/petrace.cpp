// ============================================================================
// petrace.cpp — DbgEng Extension Entry Points & Command Dispatcher (ARM64)
// ============================================================================

#include "petrace.h"
#include "correlator.h"
#include "execution_tracer.h"
#include "event_callbacks.h"

#pragma comment(linker, "/EXPORT:DebugExtensionInitialize")
#pragma comment(linker, "/EXPORT:DebugExtensionUninitialize")
#pragma comment(linker, "/EXPORT:pepaths")
#pragma comment(linker, "/EXPORT:pecfg")
#pragma comment(linker, "/EXPORT:petrace")
#pragma comment(linker, "/EXPORT:pechains")
#pragma comment(linker, "/EXPORT:pesummary")
#pragma comment(linker, "/EXPORT:peexport")
#pragma comment(linker, "/EXPORT:pedisarm")
#pragma comment(linker, "/EXPORT:peclear")
#pragma comment(linker, "/EXPORT:peonhit")
#pragma comment(linker, "/EXPORT:peonret")
#pragma comment(linker, "/EXPORT:pegpa")
#pragma comment(linker, "/EXPORT:pegparet")
#pragma comment(linker, "/EXPORT:peldll")
#pragma comment(linker, "/EXPORT:pehelp")

static void DbgEngOutputAdapter(const char* text, void* context) {
    auto pCtx = (DbgEngContext*)context;
    if (pCtx && pCtx->Control) {
        pCtx->Control->Output(DEBUG_OUTPUT_NORMAL, "%s", text);
    }
}

extern "C" {

// ----------------------------------------------------------------------------
// Extension Lifecycle Initialization
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK DebugExtensionInitialize(PULONG Version, PULONG Flags) {
    *Version = DEBUG_EXTENSION_VERSION(1, 0);
    *Flags = 0;
    return S_OK;
}

__declspec(dllexport) void CALLBACK DebugExtensionUninitialize(void) {
    // Drop the engine event sink if the extension is being torn down.
    PeTraceEventSink::Instance().Unregister();
}

// ----------------------------------------------------------------------------
// Command: !pepaths <module_name | base_address>
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK pepaths(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;

    HRESULT hr = AnalyzePePaths(ctx, Args);
    ctx.Release();
    return hr;
}

// ----------------------------------------------------------------------------
// Command: !pecfg <function_symbol | address> [mermaid]
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK pecfg(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;

    HRESULT hr = AnalyzeControlFlowGraph(ctx, Args);
    ctx.Release();
    return hr;
}

// ----------------------------------------------------------------------------
// Command: !petrace <module_name>
// Arms Level A/C candidate traps + Level B API hooks + engine event sink.
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK petrace(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;

    HRESULT hr = ArmCandidateBreakpoints(ctx, Args);
    ctx.Release();
    return hr;
}

// ----------------------------------------------------------------------------
// Command: !pechains
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK pechains(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;

    InvocationCorrelator::Instance().PrintCausalChains(DbgEngOutputAdapter, &ctx);
    ctx.Release();
    return S_OK;
}

// ----------------------------------------------------------------------------
// Command: !pesummary
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK pesummary(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;

    InvocationCorrelator::Instance().PrintSummary(DbgEngOutputAdapter, &ctx);
    ctx.Release();
    return S_OK;
}

// ----------------------------------------------------------------------------
// Command: !peexport <filepath.jsonl>
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK peexport(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;

    char path[MAX_PATH] = "trace.jsonl";
    if (Args && Args[0] != '\0') {
        sscanf(Args, "%255s", path);
    }

    bool ok = InvocationCorrelator::Instance().ExportJsonl(path);
    if (ok) {
        ctx.Out("[export] Successfully exported %zu trace event(s) to \"%s\"\n",
                InvocationCorrelator::Instance().GetEventCount(), path);
    } else {
        ctx.Out("[error] Failed to export trace events to \"%s\"\n", path);
    }

    ctx.Release();
    return ok ? S_OK : E_FAIL;
}

// ----------------------------------------------------------------------------
// Command: !pedisarm
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK pedisarm(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;

    HRESULT hr = DisarmAllBreakpoints(ctx);
    ctx.Release();
    return hr;
}

// ----------------------------------------------------------------------------
// Command: !peclear
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK peclear(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;

    InvocationCorrelator::Instance().Clear();
    ctx.Out("[correlator] Trace state, candidates, and event history cleared.\n");
    ctx.Release();
    return S_OK;
}

// ----------------------------------------------------------------------------
// Internal Breakpoint Trap Handlers: !peonhit & !peonret (Level C)
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK peonhit(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;

    HRESULT hr = OnBreakpointHit(ctx);
    ctx.Release();
    return hr;
}

__declspec(dllexport) HRESULT CALLBACK peonret(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;

    HRESULT hr = OnReturnHit(ctx);
    ctx.Release();
    return hr;
}

// ----------------------------------------------------------------------------
// Internal Level B API Trap Handlers: !pegpa / !pegparet / !peldll
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK pegpa(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;
    HRESULT hr = OnGetProcAddressEntry(ctx);
    ctx.Release();
    return hr;
}

__declspec(dllexport) HRESULT CALLBACK pegparet(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;
    HRESULT hr = OnGetProcAddressReturn(ctx);
    ctx.Release();
    return hr;
}

__declspec(dllexport) HRESULT CALLBACK peldll(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;
    HRESULT hr = OnLdrLoadDllEntry(ctx);
    ctx.Release();
    return hr;
}

// ----------------------------------------------------------------------------
// Command: !pehelp
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK pehelp(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;

    ctx.Out("\n================================================================================\n");
    ctx.Out("  PE PATH TRACER & CORRELATOR (petrace) — DbgEng Extension (ARM64)\n");
    ctx.Out("================================================================================\n");
    ctx.Out("  LEVEL A: PE DISCOVERY\n");
    ctx.Out("    !pepaths <module | base_hex>\n");
    ctx.Out("       Enumerates all 6 static invocation vectors (EntryPoint, TLS, EAT,\n");
    ctx.Out("       CFG targets, .pdata SEH, COM/CLI headers) and registers candidates.\n\n");
    ctx.Out("    !pecfg <function_symbol | address> [mermaid]\n");
    ctx.Out("       Basic-block CFG reconstruction (capstone decoder; AArch64 + x64).\n\n");
    ctx.Out("  LEVEL B: INVOCATION CORRELATION (API instrumentation)\n");
    ctx.Out("    Armed automatically by !petrace: entry traps on\n");
    ctx.Out("    KERNEL32/KERNELBASE!GetProcAddress and ntdll!LdrLoadDll.\n");
    ctx.Out("    Emits SYMBOL_RESOLVED (name -> address) and MODULE_LOAD_REQUEST.\n\n");
    ctx.Out("  LEVEL C: EXECUTION TRACE\n");
    ctx.Out("    !petrace <module_name>\n");
    ctx.Out("       Arms non-halting traps on all Level A candidates. Each hit logs\n");
    ctx.Out("       CALL_OBSERVED (with real OS PID/TID, stack, arity-aware args)\n");
    ctx.Out("       plus a one-shot return trap (RETURN_OBSERVED). Engine lifecycle\n");
    ctx.Out("       events (LoadModule/UnloadModule/ExitThread/Exception) are sinked\n");
    ctx.Out("       automatically and invalidate stale return traps.\n\n");
    ctx.Out("  REPORTING\n");
    ctx.Out("    !pechains    Confirmed causal chains (A -> B -> C)\n");
    ctx.Out("    !pesummary   Level counters\n");
    ctx.Out("    !peexport [file.jsonl]  JSONL event stream export\n");
    ctx.Out("    !pedisarm    Remove tracer breakpoints\n");
    ctx.Out("    !peclear     Reset correlator state\n");
    ctx.Out("================================================================================\n\n");

    ctx.Release();
    return S_OK;
}

} // extern "C"
