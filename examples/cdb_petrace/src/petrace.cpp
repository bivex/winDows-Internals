// ============================================================================
// petrace.cpp — DbgEng Extension Entry Points & Command Dispatcher (ARM64)
// ============================================================================

#include "petrace.h"

#pragma comment(linker, "/EXPORT:DebugExtensionInitialize")
#pragma comment(linker, "/EXPORT:DebugExtensionUninitialize")
#pragma comment(linker, "/EXPORT:pepaths")
#pragma comment(linker, "/EXPORT:pecfg")
#pragma comment(linker, "/EXPORT:pehelp")

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
// Command: !pehelp
// ----------------------------------------------------------------------------
__declspec(dllexport) HRESULT CALLBACK pehelp(PDEBUG_CLIENT4 Client, PCSTR Args) {
    DbgEngContext ctx;
    if (!ctx.Init(Client)) return E_FAIL;

    ctx.Out("\n================================================================================\n");
    ctx.Out("  PE PATH TRACER (petrace) — CDB & WinDbg DbgEng Extension (ARM64)\n");
    ctx.Out("================================================================================\n");
    ctx.Out("  COMMANDS:\n");
    ctx.Out("    !pepaths <module | base_hex>\n");
    ctx.Out("       Enumerates all 6 static invocation vectors of the target PE binary:\n");
    ctx.Out("         [1] Canonical Entry Point (AddressOfEntryPoint)\n");
    ctx.Out("         [2] TLS Callbacks (executed before EntryPoint by ntdll)\n");
    ctx.Out("         [3] Export Address Table (EAT) callable exports & forwarders\n");
    ctx.Out("         [4] Control Flow Guard (CFG) valid indirect call targets\n");
    ctx.Out("         [5] Structured Exception Handling (.pdata) unwind handlers\n");
    ctx.Out("         [6] COM / CLI Metadata & C++/CLI VTableFixup transition thunks\n\n");
    ctx.Out("    !pecfg <function_symbol | address> [mermaid]\n");
    ctx.Out("       Reconstructs the basic block topology and control flow graph (CFG)\n");
    ctx.Out("       from function entry to terminal returns (ret), displaying branches,\n");
    ctx.Out("       call destinations, and transition edges. Add 'mermaid' for markdown.\n\n");
    ctx.Out("    !pehelp\n");
    ctx.Out("       Displays this interactive help manual.\n");
    ctx.Out("================================================================================\n\n");

    ctx.Release();
    return S_OK;
}

} // extern "C"
