// ============================================================================
// execution_tracer.cpp — Dynamic Level C Execution Tracer Implementation
// ============================================================================

#include "execution_tracer.h"
#include <vector>
#include <set>

// Tracked active breakpoint IDs
static std::set<ULONG> g_activeBreakpointIds;

// ----------------------------------------------------------------------------
// Helper: Disarm All Breakpoints
// ----------------------------------------------------------------------------
HRESULT DisarmAllBreakpoints(DbgEngContext& ctx) {
    if (!ctx.Control) return E_FAIL;

    for (ULONG id : g_activeBreakpointIds) {
        IDebugBreakpoint* bp = nullptr;
        if (SUCCEEDED(ctx.Control->GetBreakpointById(id, &bp)) && bp) {
            ctx.Control->RemoveBreakpoint(bp);
            bp->Release();
        }
    }
    g_activeBreakpointIds.clear();
    ctx.Out("[tracer] All active candidate breakpoints disarmed.\n");
    return S_OK;
}

// ----------------------------------------------------------------------------
// ArmCandidateBreakpoints
// ----------------------------------------------------------------------------
HRESULT ArmCandidateBreakpoints(DbgEngContext& ctx, const char* moduleName) {
    ULONG64 base = ctx.ResolveModuleBase(moduleName);
    if (base == 0) {
        ctx.Out("[error] Could not resolve module base for: \"%s\"\n", moduleName ? moduleName : "<default>");
        return E_INVALIDARG;
    }

    IMAGE_DOS_HEADER dos = {};
    if (!ctx.ReadMemory(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        ctx.Out("[error] Address 0x%016llX is not a valid PE image.\n", (unsigned long long)base);
        return E_FAIL;
    }

    IMAGE_NT_HEADERS64 nt = {};
    if (!ctx.ReadMemory(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) {
        ctx.Out("[error] Invalid NT headers for module.\n");
        return E_FAIL;
    }

    std::string modStr = moduleName ? moduleName : "TargetModule";
    InvocationCorrelator::Instance().RegisterModule(base, nt.OptionalHeader.SizeOfImage, modStr);

    ctx.Out("\n================================================================================\n");
    ctx.Out("  ARMING DYNAMIC CANDIDATE TRAPS (Level A -> Level C Tracing)\n");
    ctx.Out("  Module: %s (Base: 0x%016llX)\n", modStr.c_str(), (unsigned long long)base);
    ctx.Out("================================================================================\n");

    int armedCount = 0;

    auto armTrap = [&](ULONG64 va, const std::string& sym, const std::string& type) {
        if (va == 0) return;
        InvocationCorrelator::Instance().AddCandidate(va, base, modStr, sym, type);

        IDebugBreakpoint* bp = nullptr;
        HRESULT hr = ctx.Control->AddBreakpoint(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp);
        if (SUCCEEDED(hr) && bp) {
            bp->SetOffset(va);
            bp->AddFlags(DEBUG_BREAKPOINT_ENABLED);
            // Non-halting automated event logging command in CDB:
            bp->SetCommand("!peonhit; g");

            ULONG id = 0;
            bp->GetId(&id);
            g_activeBreakpointIds.insert(id);

            ctx.Out("  [ARMED BP #%u] 0x%016llX -> %s (%s)\n",
                    id, (unsigned long long)va, sym.c_str(), type.c_str());
            bp->Release();
            armedCount++;
        }
    };

    // 1. Entry Point
    if (nt.OptionalHeader.AddressOfEntryPoint != 0) {
        ULONG64 epVa = base + nt.OptionalHeader.AddressOfEntryPoint;
        std::string epSym = ctx.GetSymbolName(epVa);
        if (epSym.empty()) epSym = "AddressOfEntryPoint";
        armTrap(epVa, epSym, "Canonical EntryPoint");
    }

    // 2. TLS Callbacks
    auto& tlsDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tlsDir.VirtualAddress != 0 && tlsDir.Size > 0) {
        IMAGE_TLS_DIRECTORY64 tls = {};
        if (ctx.ReadMemory(base + tlsDir.VirtualAddress, &tls, sizeof(tls)) && tls.AddressOfCallBacks != 0) {
            ULONG64 cbArray = tls.AddressOfCallBacks;
            ULONG64 pCb = 0;
            int cbIdx = 0;
            while (ctx.ReadMemory(cbArray, &pCb, sizeof(pCb)) && pCb != 0) {
                char cbName[64] = {};
                snprintf(cbName, sizeof(cbName), "TlsCallback_%d", cbIdx++);
                armTrap(pCb, cbName, "TLS Callback (Pre-Entry)");
                cbArray += sizeof(ULONG64);
            }
        }
    }

    // 3. Export Address Table (EAT)
    auto& expDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (expDir.VirtualAddress != 0 && expDir.Size > 0) {
        IMAGE_EXPORT_DIRECTORY exp = {};
        if (ctx.ReadMemory(base + expDir.VirtualAddress, &exp, sizeof(exp))) {
            std::vector<DWORD> funcRvas(exp.NumberOfFunctions);
            std::vector<DWORD> nameRvas(exp.NumberOfNames);
            std::vector<WORD>  ordinals(exp.NumberOfNames);

            ctx.ReadMemory(base + exp.AddressOfFunctions, funcRvas.data(), (ULONG)(funcRvas.size() * sizeof(DWORD)));
            ctx.ReadMemory(base + exp.AddressOfNames, nameRvas.data(), (ULONG)(nameRvas.size() * sizeof(DWORD)));
            ctx.ReadMemory(base + exp.AddressOfNameOrdinals, ordinals.data(), (ULONG)(ordinals.size() * sizeof(WORD)));

            for (DWORD i = 0; i < exp.NumberOfNames; ++i) {
                char expName[256] = {};
                ctx.ReadMemory(base + nameRvas[i], expName, sizeof(expName) - 1);
                WORD ordIndex = ordinals[i];
                DWORD fnRva = funcRvas[ordIndex];

                // Skip forwarders
                if (fnRva >= expDir.VirtualAddress && fnRva < expDir.VirtualAddress + expDir.Size) continue;

                ULONG64 fnVa = base + fnRva;
                armTrap(fnVa, expName, "Exported Function");
            }
        }
    }

    ctx.Out("\n[tracer] Successfully armed %d candidate breakpoint(s). Resume target ('g') to trace!\n", armedCount);
    ctx.Out("================================================================================\n\n");
    return S_OK;
}

// ----------------------------------------------------------------------------
// OnBreakpointHit
// ----------------------------------------------------------------------------
HRESULT OnBreakpointHit(DbgEngContext& ctx) {
    if (!ctx.Control) return E_FAIL;

    // Read Instruction Pointer & Link Register
    DEBUG_VALUE dvPc = {}, dvLr = {}, dvX0 = {}, dvX1 = {};
    ULONG64 pc = 0, lr = 0, x0 = 0, x1 = 0;

    if (SUCCEEDED(ctx.Control->Evaluate("@$ip", DEBUG_VALUE_INT64, &dvPc, nullptr))) {
        pc = (ULONG64)dvPc.I64;
    }
    // ARM64 Link Register is @lr (x30); x64 return address is on stack [@rsp]
    if (FAILED(ctx.Control->Evaluate("@lr", DEBUG_VALUE_INT64, &dvLr, nullptr))) {
        ctx.Control->Evaluate("poi(@$csp)", DEBUG_VALUE_INT64, &dvLr, nullptr);
    }
    lr = (ULONG64)dvLr.I64;

    ctx.Control->Evaluate("@x0", DEBUG_VALUE_INT64, &dvX0, nullptr);
    x0 = (ULONG64)dvX0.I64;
    ctx.Control->Evaluate("@x1", DEBUG_VALUE_INT64, &dvX1, nullptr);
    x1 = (ULONG64)dvX1.I64;

    ULONG pid = 0, tid = 0;
    if (ctx.SystemObjects) {
        ctx.SystemObjects->GetCurrentProcessId(&pid);
        ctx.SystemObjects->GetCurrentThreadId(&tid);
    }

    // Capture stack trace (up to 6 frames)
    DEBUG_STACK_FRAME frames[6] = {};
    ULONG filled = 0;
    std::vector<uint64_t> stack;
    if (SUCCEEDED(ctx.Control->GetStackTrace(0, 0, 0, frames, 6, &filled))) {
        for (ULONG i = 0; i < filled; ++i) {
            stack.push_back(frames[i].InstructionOffset);
        }
    }

    char det[256] = {};
    snprintf(det, sizeof(det), "Args: x0=0x%016llX, x1=0x%016llX",
             (unsigned long long)x0, (unsigned long long)x1);

    std::string sym = ctx.GetSymbolName(pc);
    uint64_t chainId = InvocationCorrelator::Instance().RecordCallObserved(
        pc, lr, lr, pid, tid, stack, det
    );

    ctx.Out("\n>>> [LEVEL C: CALL_OBSERVED] (Chain #%llu) <<<\n", (unsigned long long)chainId);
    ctx.Out("  Target VA      : 0x%016llX %s%s%s\n",
            (unsigned long long)pc, sym.empty() ? "" : "(", sym.c_str(), sym.empty() ? "" : ")");
    ctx.Out("  Source / Caller: 0x%016llX\n", (unsigned long long)lr);
    ctx.Out("  Thread ID      : %u (PID: %u)\n", tid, pid);
    ctx.Out("  Parameters     : %s\n", det);

    // Set one-shot return breakpoint on continuation address (lr)
    if (lr > 0x10000) {
        IDebugBreakpoint* retBp = nullptr;
        if (SUCCEEDED(ctx.Control->AddBreakpoint(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &retBp)) && retBp) {
            retBp->SetOffset(lr);
            retBp->AddFlags(DEBUG_BREAKPOINT_ENABLED | DEBUG_BREAKPOINT_ONE_SHOT);
            retBp->SetCommand("!peonret; g");
            retBp->Release();
        }
    }

    return S_OK;
}

// ----------------------------------------------------------------------------
// OnReturnHit
// ----------------------------------------------------------------------------
HRESULT OnReturnHit(DbgEngContext& ctx) {
    if (!ctx.Control) return E_FAIL;

    DEBUG_VALUE dvPc = {}, dvX0 = {};
    ULONG64 pc = 0, x0 = 0;

    if (SUCCEEDED(ctx.Control->Evaluate("@$ip", DEBUG_VALUE_INT64, &dvPc, nullptr))) {
        pc = (ULONG64)dvPc.I64;
    }
    // ARM64 return value in x0; x64 return value in rax
    if (FAILED(ctx.Control->Evaluate("@x0", DEBUG_VALUE_INT64, &dvX0, nullptr))) {
        ctx.Control->Evaluate("@rax", DEBUG_VALUE_INT64, &dvX0, nullptr);
    }
    x0 = (ULONG64)dvX0.I64;

    ULONG pid = 0, tid = 0;
    if (ctx.SystemObjects) {
        ctx.SystemObjects->GetCurrentProcessId(&pid);
        ctx.SystemObjects->GetCurrentThreadId(&tid);
    }

    InvocationCorrelator::Instance().RecordReturnObserved(pc, pid, tid, x0);

    ctx.Out("<<< [LEVEL C: RETURN_OBSERVED] Return to: 0x%016llX | Return Val: 0x%llX (%lld) | TID: %u >>>\n\n",
            (unsigned long long)pc, (unsigned long long)x0, (long long)x0, tid);

    return S_OK;
}
