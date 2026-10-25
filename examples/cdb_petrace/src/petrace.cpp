// ============================================================================
// petrace.cpp — DbgEng Extension Entry Points & Command Dispatcher (ARM64)
// ============================================================================

#include "petrace.h"
#include "correlator.h"
#include "execution_tracer.h"

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
// Internal Breakpoint Trap Handlers: !peonhit & !peonret
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
    ctx.Out("       Reconstructs the basic block topology and CFG from function entry to ret.\n\n");
    ctx.Out("  LEVEL B & C: DYNAMIC CORRELATION & EXECUTION TRACE\n");
    ctx.Out("    !petrace <module_name>\n");
    ctx.Out("       Arms dynamic non-halting breakpoint traps on all Level A candidates.\n");
    ctx.Out("       When hit, logs CALL_OBSERVED and arms return traps for RETURN_OBSERVED.\n\n");
    ctx.Out("    !pechains\n");
    ctx.Out("       Displays all confirmed causal execution chains (Candidate -> Call -> Return).\n\n");
    ctx.Out("    !pesummary\n");
    ctx.Out("       Prints correlation engine summary counts across all 3 levels.\n\n");
    ctx.Out("    !peexport [filepath.jsonl]\n");
    ctx.Out("       Exports the full JSONL stream of all recorded Level A/B/C events.\n\n");
    ctx.Out("    !pedisarm\n");
    ctx.Out("       Removes all active tracer breakpoint traps.\n\n");
    ctx.Out("    !peclear\n");
    ctx.Out("       Resets correlation tables, module registry, and event streams.\n");
    ctx.Out("================================================================================\n\n");

    ctx.Release();
    return S_OK;
}

} // extern "C"
