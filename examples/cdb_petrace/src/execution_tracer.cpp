// ============================================================================
// execution_tracer.cpp — Level C trap engine + Level B API instrumentation
//
// Level C: candidate breakpoints (!peonhit) + one-shot return traps (!peonret).
//          Breakpoint callbacks report REAL OS PID/TID via
//          IDebugSystemObjects::GetCurrent{Process,Thread}SystemId.
// Level B: entry traps on KERNEL32!GetProcAddress and ntdll!LdrLoadDll capture
//          the caller's request (symbol name / module name) and the resolved
//          address, emitting SYMBOL_RESOLVED / MODULE_LOAD_REQUEST events.
// ============================================================================

#include "execution_tracer.h"
#include "event_callbacks.h"
#include <vector>
#include <set>
#include <map>
#include <string>

// Tracked active breakpoint IDs
static std::set<ULONG> g_activeBreakpointIds;

// ----------------------------------------------------------------------------
// Real OS identity helpers (NOT engine handles)
// ----------------------------------------------------------------------------
static uint32_t SysPid(DbgEngContext& ctx) {
    ULONG pid = 0;
    if (ctx.SystemObjects) ctx.SystemObjects->GetCurrentProcessSystemId(&pid);
    return pid;
}

static uint32_t SysTid(DbgEngContext& ctx) {
    ULONG tid = 0;
    if (ctx.SystemObjects) ctx.SystemObjects->GetCurrentThreadSystemId(&tid);
    return tid;
}

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
// ArmCandidateBreakpoints — Level A discovery + Level C traps + Level B hooks
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

    // Subscribe the engine event sink BEFORE arming, so lifecycle events
    // (LoadModule/UnloadModule/ExitThread/Exception) feed the correlator.
    PeTraceEventSink::Instance().Register((PDEBUG_CLIENT)ctx.Client);

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

                armTrap(base + fnRva, expName, "Exported Function");
            }
        }
    }

    ctx.Out("\n[tracer] Armed %d Level A candidate breakpoint(s).\n", armedCount);

    // ---- Level B: API instrumentation hooks ----
    int lvlB = 0;
    auto armApiTrap = [&](const char* sym, const char* cmd, const char* what) {
        ULONG64 va = 0;
        if (ctx.Symbols && SUCCEEDED(ctx.Symbols->GetOffsetByName(sym, &va)) && va > 0x10000) {
            IDebugBreakpoint* bp = nullptr;
            if (SUCCEEDED(ctx.Control->AddBreakpoint(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp)) && bp) {
                bp->SetOffset(va);
                bp->AddFlags(DEBUG_BREAKPOINT_ENABLED);
                bp->SetCommand(cmd);
                ULONG id = 0;
                bp->GetId(&id);
                g_activeBreakpointIds.insert(id);
                ctx.Out("  [LEVEL B BP #%u] %s @ 0x%016llX (%s)\n",
                        id, sym, (unsigned long long)va, what);
                bp->Release();
                lvlB++;
            }
        }
    };

    armApiTrap("KERNEL32!GetProcAddress", "!pegpa; g", "symbol resolution");
    armApiTrap("KERNELBASE!GetProcAddress", "!pegpa; g", "symbol resolution");
    armApiTrap("ntdll!LdrLoadDll", "!peldll; g", "module load request");

    ctx.Out("[tracer] Armed %d Level B API instrumentation trap(s).\n", lvlB);
    ctx.Out("================================================================================\n\n");
    return S_OK;
}

// ----------------------------------------------------------------------------
// Level C: candidate hit
// ----------------------------------------------------------------------------
HRESULT OnBreakpointHit(DbgEngContext& ctx) {
    if (!ctx.Control) return E_FAIL;

    DEBUG_VALUE dvPc = {}, dvLr = {}, dvX0 = {}, dvX1 = {}, dvX2 = {};
    ULONG64 pc = 0, lr = 0, x0 = 0, x1 = 0, x2 = 0;

    if (SUCCEEDED(ctx.Control->Evaluate("@$ip", DEBUG_VALUE_INT64, &dvPc, nullptr))) {
        pc = (ULONG64)dvPc.I64;
    }
    // ARM64: link register @lr (x30); x64: return address at [@rsp]
    if (FAILED(ctx.Control->Evaluate("@lr", DEBUG_VALUE_INT64, &dvLr, nullptr))) {
        ctx.Control->Evaluate("poi(@$csp)", DEBUG_VALUE_INT64, &dvLr, nullptr);
    }
    lr = (ULONG64)dvLr.I64;

    ctx.Control->Evaluate("@x0", DEBUG_VALUE_INT64, &dvX0, nullptr);
    x0 = (ULONG64)dvX0.I64;
    ctx.Control->Evaluate("@x1", DEBUG_VALUE_INT64, &dvX1, nullptr);
    x1 = (ULONG64)dvX1.I64;
    ctx.Control->Evaluate("@x2", DEBUG_VALUE_INT64, &dvX2, nullptr);
    x2 = (ULONG64)dvX2.I64;

    uint32_t pid = SysPid(ctx);
    uint32_t tid = SysTid(ctx);

    // Capture stack trace (up to 6 frames)
    DEBUG_STACK_FRAME frames[6] = {};
    ULONG filled = 0;
    std::vector<uint64_t> stack;
    if (SUCCEEDED(ctx.Control->GetStackTrace(0, 0, 0, frames, 6, &filled))) {
        for (ULONG i = 0; i < filled; ++i) {
            stack.push_back(frames[i].InstructionOffset);
        }
    }

    std::string sym = ctx.GetSymbolName(pc);
    std::string bareSym = sym;
    size_t bang = bareSym.find('!');
    if (bang != std::string::npos) bareSym = bareSym.substr(bang + 1);
    size_t plus = bareSym.find('+');
    if (plus != std::string::npos) bareSym = bareSym.substr(0, plus);

    // Arity-aware argument reporting: never publish leftover register noise as
    // "arguments" of a void(HRESULT) export.
    int arity = GetFunctionArity(bareSym);
    char det[320] = {};
    if (arity == 0) {
        snprintf(det, sizeof(det), "Parameters: void (no arguments)");
    } else if (arity == 1) {
        snprintf(det, sizeof(det), "Arg: param0=0x%016llX", (unsigned long long)x0);
    } else if (bareSym.find("DllMain") != std::string::npos) {
        snprintf(det, sizeof(det),
                 "DllMain Args: hinst=0x%016llX, fdwReason=%llu (%s), lpvReserved=0x%016llX",
                 (unsigned long long)x0, (unsigned long long)x1,
                 (x1 == 1) ? "DLL_PROCESS_ATTACH" :
                 (x1 == 0) ? "DLL_PROCESS_DETACH" :
                 (x1 == 2) ? "DLL_THREAD_ATTACH" : "DLL_THREAD_DETACH",
                 (unsigned long long)x2);
    } else if (arity == 3) {
        snprintf(det, sizeof(det), "Args: x0=0x%016llX, x1=0x%016llX, x2=0x%016llX",
                 (unsigned long long)x0, (unsigned long long)x1, (unsigned long long)x2);
    } else {
        snprintf(det, sizeof(det), "Raw Regs: x0=0x%016llX, x1=0x%016llX (arity unverified)",
                 (unsigned long long)x0, (unsigned long long)x1);
    }

    uint64_t chainId = InvocationCorrelator::Instance().RecordCallObserved(
        pc, lr, lr, pid, tid, stack, det
    );

    ctx.Out("\n>>> [LEVEL C: CALL_OBSERVED] (Chain #%llu) <<<\n", (unsigned long long)chainId);
    ctx.Out("  Target VA      : 0x%016llX %s%s%s\n",
            (unsigned long long)pc, sym.empty() ? "" : "(", sym.c_str(), sym.empty() ? "" : ")");
    ctx.Out("  Source / Caller: 0x%016llX\n", (unsigned long long)lr);
    ctx.Out("  Thread ID      : %u (PID: %u)\n", tid, pid);
    ctx.Out("  %s\n", det);

    // One-shot continuation trap on LR. NOTE: this assumes an ordinary return;
    // tail calls / exceptions are handled by the event sink invalidating the
    // in-flight call (OnException / OnThreadExit), not by this trap.
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
// Level C: continuation reached
// ----------------------------------------------------------------------------
HRESULT OnReturnHit(DbgEngContext& ctx) {
    if (!ctx.Control) return E_FAIL;

    DEBUG_VALUE dvPc = {}, dvX0 = {};
    ULONG64 pc = 0, x0 = 0;

    if (SUCCEEDED(ctx.Control->Evaluate("@$ip", DEBUG_VALUE_INT64, &dvPc, nullptr))) {
        pc = (ULONG64)dvPc.I64;
    }
    if (FAILED(ctx.Control->Evaluate("@x0", DEBUG_VALUE_INT64, &dvX0, nullptr))) {
        ctx.Control->Evaluate("@rax", DEBUG_VALUE_INT64, &dvX0, nullptr);
    }
    x0 = (ULONG64)dvX0.I64;

    uint32_t pid = SysPid(ctx);
    uint32_t tid = SysTid(ctx);

    InvocationCorrelator::Instance().RecordReturnObserved(pc, pid, tid, x0);

    ctx.Out("<<< [LEVEL C: RETURN_OBSERVED] Return to: 0x%016llX | Ret: 0x%llX (%lld) | TID: %u >>>\n\n",
            (unsigned long long)pc, (unsigned long long)x0, (long long)x0, tid);

    return S_OK;
}

// ============================================================================
// LEVEL B — API instrumentation (GetProcAddress / LdrLoadDll)
// ============================================================================
struct GpaPending {
    std::string name;
    ULONG64 lr = 0;
};

// Per-OS-TID stacks of in-flight API requests (recursion-safe)
static std::map<uint32_t, std::vector<GpaPending>> g_gpaPending;

static std::string ReadAnsiString(DbgEngContext& ctx, ULONG64 va, size_t maxLen = 128) {
    std::string out;
    if (va <= 0x10000) return out;
    char buf[129] = {};
    if (!ctx.ReadMemory(va, buf, (ULONG)maxLen)) return out;
    buf[maxLen] = '\0';
    out.assign(buf);
    return out;
}

HRESULT OnGetProcAddressEntry(DbgEngContext& ctx) {
    if (!ctx.Control) return E_FAIL;

    DEBUG_VALUE dvX1 = {}, dvLr = {};
    ULONG64 x1 = 0, lr = 0;
    ctx.Control->Evaluate("@x1", DEBUG_VALUE_INT64, &dvX1, nullptr);
    x1 = (ULONG64)dvX1.I64;
    ctx.Control->Evaluate("@lr", DEBUG_VALUE_INT64, &dvLr, nullptr);
    lr = (ULONG64)dvLr.I64;

    std::string name;
    if (x1 > 0 && x1 <= 0xFFFF) {
        name = "#" + std::to_string((unsigned)(x1 & 0xFFFF)); // ordinal
    } else {
        name = ReadAnsiString(ctx, x1);
    }

    uint32_t tid = SysTid(ctx);
    g_gpaPending[tid].push_back({ name, lr });

    // One-shot exit trap on the caller continuation captures the resolved VA
    if (lr > 0x10000) {
        IDebugBreakpoint* bp = nullptr;
        if (SUCCEEDED(ctx.Control->AddBreakpoint(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp)) && bp) {
            bp->SetOffset(lr);
            bp->AddFlags(DEBUG_BREAKPOINT_ENABLED | DEBUG_BREAKPOINT_ONE_SHOT);
            bp->SetCommand("!pegparet; g");
            bp->Release();
        }
    }
    return S_OK;
}

HRESULT OnGetProcAddressReturn(DbgEngContext& ctx) {
    if (!ctx.Control) return E_FAIL;

    DEBUG_VALUE dvX0 = {}, dvLr = {};
    ULONG64 x0 = 0, lr = 0;
    ctx.Control->Evaluate("@x0", DEBUG_VALUE_INT64, &dvX0, nullptr);
    x0 = (ULONG64)dvX0.I64;
    ctx.Control->Evaluate("@lr", DEBUG_VALUE_INT64, &dvLr, nullptr);
    lr = (ULONG64)dvLr.I64;

    uint32_t pid = SysPid(ctx);
    uint32_t tid = SysTid(ctx);

    auto it = g_gpaPending.find(tid);
    if (it == g_gpaPending.end() || it->second.empty()) return S_OK; // stale / foreign one-shot

    GpaPending p = it->second.back();
    it->second.pop_back();
    if (it->second.empty()) g_gpaPending.erase(it);

    if (x0 > 0x10000) {
        InvocationCorrelator::Instance().RecordSymbolResolved(x0, p.name, pid, tid, p.lr);
        ctx.Out(">>> [LEVEL B: SYMBOL_RESOLVED] \"%s\" -> 0x%016llX (from caller 0x%016llX, TID %u)\n",
                p.name.c_str(), (unsigned long long)x0, (unsigned long long)p.lr, tid);
    }
    return S_OK;
}

HRESULT OnLdrLoadDllEntry(DbgEngContext& ctx) {
    if (!ctx.Control) return E_FAIL;

    // LdrLoadDll(PWSTR path, PDWORD flags, PUNICODE_STRING name, PHMODULE base)
    // x2 = PUNICODE_STRING { USHORT Length; USHORT MaximumLength; [pad] PWCH Buffer; }
    DEBUG_VALUE dvX2 = {};
    ULONG64 x2 = 0;
    ctx.Control->Evaluate("@x2", DEBUG_VALUE_INT64, &dvX2, nullptr);
    x2 = (ULONG64)dvX2.I64;

    uint32_t pid = SysPid(ctx);
    uint32_t tid = SysTid(ctx);

    std::string name = "<unreadable>";
    if (x2 > 0x10000) {
        unsigned char us[16] = {};
        if (ctx.ReadMemory(x2, us, sizeof(us))) {
            USHORT len = *(USHORT*)(us + 0);
            ULONG64 buf = *(ULONG64*)(us + 8);
            if (len > 0 && len <= 520 && buf > 0x10000) {
                std::vector<WCHAR> w(len / 2 + 1, L'\0');
                if (ctx.ReadMemory(buf, w.data(), len)) {
                    char narrow[264] = {};
                    WideCharToMultiByte(CP_ACP, 0, w.data(), -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
                    name = narrow;
                }
            }
        }
    }

    InvocationCorrelator::Instance().RecordModuleLoadRequest(name, pid, tid);
    ctx.Out(">>> [LEVEL B: MODULE_LOAD_REQUEST] \"%s\" (TID %u)\n", name.c_str(), tid);
    return S_OK;
}
