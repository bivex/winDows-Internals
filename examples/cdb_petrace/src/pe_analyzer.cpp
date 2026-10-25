// ============================================================================
// pe_analyzer.cpp — Complete 6-Vector PE Invocation Analysis Implementation
// ============================================================================

#include "petrace.h"

// ----------------------------------------------------------------------------
// ResolveModuleBase Helper
// ----------------------------------------------------------------------------
ULONG64 DbgEngContext::ResolveModuleBase(const char* moduleOrAddr) {
    if (!Symbols) return 0;

    // Default to main module (index 0) if no argument
    if (!moduleOrAddr || moduleOrAddr[0] == '\0') {
        ULONG64 base = 0;
        if (SUCCEEDED(Symbols->GetModuleByIndex(0, &base))) {
            return base;
        }
        return 0;
    }

    // Check if numeric hex address
    if ((moduleOrAddr[0] == '0' && (moduleOrAddr[1] == 'x' || moduleOrAddr[1] == 'X')) ||
        (isxdigit(moduleOrAddr[0]) && strlen(moduleOrAddr) >= 8)) {
        char* end = nullptr;
        ULONG64 addr = strtoull(moduleOrAddr, &end, 16);
        if (addr > 0x10000) return addr;
    }

    // Try GetModuleByModuleName
    ULONG idx = 0;
    ULONG64 base = 0;
    if (SUCCEEDED(Symbols->GetModuleByModuleName(moduleOrAddr, 0, &idx, &base))) {
        return base;
    }

    // Try appending .dll
    std::string withDll = std::string(moduleOrAddr) + ".dll";
    if (SUCCEEDED(Symbols->GetModuleByModuleName(withDll.c_str(), 0, &idx, &base))) {
        return base;
    }

    // Try appending .exe
    std::string withExe = std::string(moduleOrAddr) + ".exe";
    if (SUCCEEDED(Symbols->GetModuleByModuleName(withExe.c_str(), 0, &idx, &base))) {
        return base;
    }

    // Try evaluating expression/symbol
    if (Control) {
        DEBUG_VALUE val = {};
        if (SUCCEEDED(Control->Evaluate(moduleOrAddr, DEBUG_VALUE_INT64, &val, nullptr))) {
            return val.I64;
        }
    }

    return 0;
}

// ----------------------------------------------------------------------------
// AnalyzePePaths Implementation
// ----------------------------------------------------------------------------
HRESULT AnalyzePePaths(DbgEngContext& ctx, const char* args) {
    ULONG64 base = ctx.ResolveModuleBase(args);
    if (base == 0) {
        ctx.Out("[error] Could not resolve module base for: \"%s\"\n", args ? args : "<default>");
        ctx.Out("        Usage: !pepaths <module_name | base_address>\n");
        return E_INVALIDARG;
    }

    // 1. Read DOS Header
    IMAGE_DOS_HEADER dos = {};
    if (!ctx.ReadMemory(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        ctx.Out("[error] Address 0x%016llX does not point to a valid PE image (MZ missing)\n",
                (unsigned long long)base);
        return E_FAIL;
    }

    // 2. Read NT Headers (64-bit)
    IMAGE_NT_HEADERS64 nt = {};
    ULONG64 ntOffset = base + dos.e_lfanew;
    if (!ctx.ReadMemory(ntOffset, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) {
        ctx.Out("[error] Invalid PE NT Signature at offset +0x%lx\n", dos.e_lfanew);
        return E_FAIL;
    }

    const char* archName = (nt.FileHeader.Machine == IMAGE_FILE_MACHINE_ARM64) ? "ARM64" :
                           (nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64) ? "x64 (AMD64)" :
                           (nt.FileHeader.Machine == 0x014c) ? "x86 (I386)" : "Unknown Architecture";

    ctx.Out("\n================================================================================\n");
    ctx.Out("  PE INVOCATION VECTORS & CALL PATH ANALYSIS\n");
    ctx.Out("  Target Base    : 0x%016llX (Size: 0x%08X)\n", (unsigned long long)base, nt.OptionalHeader.SizeOfImage);
    ctx.Out("  Architecture   : 0x%04X (%s) | Characteristics: 0x%04X (%s)\n",
            nt.FileHeader.Machine, archName,
            nt.FileHeader.Characteristics,
            (nt.FileHeader.Characteristics & IMAGE_FILE_DLL) ? "DLL" : "EXE");
    ctx.Out("================================================================================\n");

    int totalVectors = 0;

    // ------------------------------------------------------------------------
    // VECTOR 1: AddressOfEntryPoint
    // ------------------------------------------------------------------------
    ctx.Out("\n[VECTOR 1] CANONICAL ENTRY POINT (AddressOfEntryPoint)\n");
    if (nt.OptionalHeader.AddressOfEntryPoint != 0) {
        ULONG64 entryVa = base + nt.OptionalHeader.AddressOfEntryPoint;
        std::string sym = ctx.GetSymbolName(entryVa);
        ctx.Out("  RVA            : 0x%08X\n", nt.OptionalHeader.AddressOfEntryPoint);
        ctx.Out("  VirtualAddress : 0x%016llX %s%s%s\n",
                (unsigned long long)entryVa,
                sym.empty() ? "" : "(", sym.c_str(), sym.empty() ? "" : ")");
        ctx.Out("  Mechanism      : Executed by ntdll!LdrpCallInitRoutine (DLL) / BaseThreadInitThunk (EXE)\n");
        ctx.Out("  Disassembly of Initial Instructions:\n");

        ULONG64 next = entryVa;
        for (int i = 0; i < 3; ++i) {
            ULONG64 cur = next;
            std::string line = ctx.DisassembleInstruction(cur, &next);
            ctx.Out("    0x%016llX: %s\n", (unsigned long long)cur, line.c_str());
        }
        totalVectors++;
    } else {
        ctx.Out("  Status         : ABSENT (0x00000000 - Pure resource library or metadata container)\n");
    }

    // ------------------------------------------------------------------------
    // VECTOR 2: Thread Local Storage (TLS) Callbacks (Pre-Entry-Point Execution)
    // ------------------------------------------------------------------------
    ctx.Out("\n[VECTOR 2] THREAD LOCAL STORAGE (TLS) CALLBACKS (Pre-Entry Execution)\n");
    auto& tlsDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tlsDir.VirtualAddress != 0 && tlsDir.Size > 0) {
        IMAGE_TLS_DIRECTORY64 tls = {};
        if (ctx.ReadMemory(base + tlsDir.VirtualAddress, &tls, sizeof(tls))) {
            ctx.Out("  TLS Directory  : RVA 0x%08X (Size: 0x%08X)\n", tlsDir.VirtualAddress, tlsDir.Size);
            ctx.Out("  Callback Array : 0x%016llX\n", (unsigned long long)tls.AddressOfCallBacks);

            if (tls.AddressOfCallBacks != 0) {
                int cbCount = 0;
                ULONG64 pCallback = 0;
                ULONG64 cbArray = tls.AddressOfCallBacks;

                while (ctx.ReadMemory(cbArray, &pCallback, sizeof(pCallback)) && pCallback != 0) {
                    std::string sym = ctx.GetSymbolName(pCallback);
                    ctx.Out("    [TLS #%d] 0x%016llX %s%s%s\n",
                            cbCount, (unsigned long long)pCallback,
                            sym.empty() ? "" : "(", sym.c_str(), sym.empty() ? "" : ")");

                    ULONG64 next = 0;
                    std::string dis = ctx.DisassembleInstruction(pCallback, &next);
                    ctx.Out("      Initial: %s\n", dis.c_str());

                    cbCount++;
                    cbArray += sizeof(ULONG64);
                    totalVectors++;
                }
                ctx.Out("  Summary        : %d TLS callback(s) executed by ntdll BEFORE EntryPoint!\n", cbCount);
            }
        }
    } else {
        ctx.Out("  Status         : ABSENT (No pre-entry TLS callbacks declared)\n");
    }

    // ------------------------------------------------------------------------
    // VECTOR 3: Export Address Table (EAT)
    // ------------------------------------------------------------------------
    ctx.Out("\n[VECTOR 3] EXPORT ADDRESS TABLE (EAT) / CALLABLE ENTRY POINTS\n");
    auto& expDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (expDir.VirtualAddress != 0 && expDir.Size > 0) {
        IMAGE_EXPORT_DIRECTORY exp = {};
        if (ctx.ReadMemory(base + expDir.VirtualAddress, &exp, sizeof(exp))) {
            char modDllName[256] = {};
            if (exp.Name != 0) {
                ctx.ReadMemory(base + exp.Name, modDllName, sizeof(modDllName) - 1);
            }

            ctx.Out("  Export Name    : \"%s\"\n", modDllName[0] ? modDllName : "<unknown>");
            ctx.Out("  Base Ordinal   : %u | Total Functions: %u | Total Names: %u\n",
                    (unsigned)exp.Base, (unsigned)exp.NumberOfFunctions, (unsigned)exp.NumberOfNames);

            std::vector<DWORD> funcRvas(exp.NumberOfFunctions);
            std::vector<DWORD> nameRvas(exp.NumberOfNames);
            std::vector<WORD>  ordinals(exp.NumberOfNames);

            ctx.ReadMemory(base + exp.AddressOfFunctions, funcRvas.data(), (ULONG)(funcRvas.size() * sizeof(DWORD)));
            ctx.ReadMemory(base + exp.AddressOfNames, nameRvas.data(), (ULONG)(nameRvas.size() * sizeof(DWORD)));
            ctx.ReadMemory(base + exp.AddressOfNameOrdinals, ordinals.data(), (ULONG)(ordinals.size() * sizeof(WORD)));

            ULONG expLimit = (ULONG)min((size_t)exp.NumberOfNames, (size_t)25);
            for (ULONG i = 0; i < expLimit; ++i) {
                char expName[256] = {};
                ctx.ReadMemory(base + nameRvas[i], expName, sizeof(expName) - 1);
                WORD ordIndex = ordinals[i];
                DWORD fnRva = funcRvas[ordIndex];
                ULONG64 fnVa = base + fnRva;

                // Check if forwarder
                if (fnRva >= expDir.VirtualAddress && fnRva < expDir.VirtualAddress + expDir.Size) {
                    char fwd[256] = {};
                    ctx.ReadMemory(base + fnRva, fwd, sizeof(fwd) - 1);
                    ctx.Out("    [Ord %4u] %-35s -> Forwarder: %s\n",
                            (unsigned)(exp.Base + ordIndex), expName, fwd);
                } else {
                    ULONG64 next = 0;
                    std::string dis = ctx.DisassembleInstruction(fnVa, &next);
                    ctx.Out("    [Ord %4u] %-35s (RVA: 0x%08X, VA: 0x%016llX)\n",
                            (unsigned)(exp.Base + ordIndex), expName, fnRva, (unsigned long long)fnVa);
                    ctx.Out("      Initial: %s\n", dis.c_str());
                }
                totalVectors++;
            }
            if (exp.NumberOfNames > expLimit) {
                ctx.Out("    ... [%u more exports elided]\n", (unsigned)(exp.NumberOfNames - expLimit));
                totalVectors += (exp.NumberOfNames - expLimit);
            }
        }
    } else {
        ctx.Out("  Status         : ABSENT (No public exported functions)\n");
    }

    // ------------------------------------------------------------------------
    // VECTOR 4: Control Flow Guard (CFG) Valid Indirect Targets
    // ------------------------------------------------------------------------
    ctx.Out("\n[VECTOR 4] CONTROL FLOW GUARD (CFG) LOAD_CONFIG TABLE\n");
    auto& cfgDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    if (cfgDir.VirtualAddress != 0 && cfgDir.Size > 0) {
        IMAGE_LOAD_CONFIG_DIRECTORY64 lc = {};
        if (ctx.ReadMemory(base + cfgDir.VirtualAddress, &lc, sizeof(lc))) {
            ctx.Out("  CFG GuardFlags : 0x%08X\n", lc.GuardFlags);
            bool isInstrumented = (lc.GuardFlags & 0x00000100) != 0;
            bool hasFuncTable   = (lc.GuardFlags & 0x00000400) != 0;
            ctx.Out("  CFG Active     : %s\n", isInstrumented ? "YES (GuardCheckICall enforced)" : "NO");

            if (hasFuncTable && lc.GuardCFFunctionTable != 0 && lc.GuardCFFunctionCount > 0) {
                ctx.Out("  CFG Targets    : %llu address-taken functions declared in bitmap\n",
                        (unsigned long long)lc.GuardCFFunctionCount);
                totalVectors += (int)lc.GuardCFFunctionCount;
            }
        }
    } else {
        ctx.Out("  Status         : ABSENT (No LoadConfig / CFG directory)\n");
    }

    // ------------------------------------------------------------------------
    // VECTOR 5: Exception Handlers & Landing Pads (.pdata)
    // ------------------------------------------------------------------------
    ctx.Out("\n[VECTOR 5] STRUCTURED EXCEPTION HANDLING (.pdata Unwind Handlers)\n");
    auto& excDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (excDir.VirtualAddress != 0 && excDir.Size > 0) {
        DWORD funcCount = excDir.Size / (DWORD)sizeof(PE_ARM64_RUNTIME_FUNCTION);
        ctx.Out("  Exception Table: RVA 0x%08X (Size: 0x%08X)\n", excDir.VirtualAddress, excDir.Size);
        ctx.Out("  Runtime Entries: %lu protected function scopes registered in ntdll\n", funcCount);
        ctx.Out("  Invocation     : Invoked by ntdll!RtlDispatchException during exception unwinding\n");
    } else {
        ctx.Out("  Status         : ABSENT (No exception unwinding tables)\n");
    }

    // ------------------------------------------------------------------------
    // VECTOR 6: COM / CLI Header & C++/CLI VTable Fixups
    // ------------------------------------------------------------------------
    ctx.Out("\n[VECTOR 6] COM / CLI HYBRID HEADERS (VTableFixups & Managed Tokens)\n");
    auto& comDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COMHEADER];
    if (comDir.VirtualAddress != 0 && comDir.Size > 0) {
        PE_COR20_HEADER cor = {};
        if (ctx.ReadMemory(base + comDir.VirtualAddress, &cor, sizeof(cor))) {
            ctx.Out("  COR20 Runtime  : %u.%u\n", (unsigned)cor.MajorRuntimeVersion, (unsigned)cor.MinorRuntimeVersion);
            ctx.Out("  Flags          : 0x%08X (ILOnly=%d, NativeEntry=%d)\n",
                    cor.Flags, (cor.Flags & 1) ? 1 : 0, (cor.Flags & 0x10) ? 1 : 0);

            if (cor.VTableFixups.VirtualAddress != 0 && cor.VTableFixups.Size > 0) {
                DWORD fixupCount = cor.VTableFixups.Size / (DWORD)sizeof(PE_COR_VTABLEFIXUP);
                ctx.Out("  VTableFixups   : %lu hybrid crossover descriptor(s) found!\n", fixupCount);

                std::vector<PE_COR_VTABLEFIXUP> fixups(fixupCount);
                ctx.ReadMemory(base + cor.VTableFixups.VirtualAddress, fixups.data(), (ULONG)(fixups.size() * sizeof(PE_COR_VTABLEFIXUP)));

                for (DWORD i = 0; i < fixupCount; ++i) {
                    ULONG64 slotVa = base + fixups[i].RVA;
                    ULONG64 targetVal = 0;
                    ctx.ReadMemory(slotVa, &targetVal, sizeof(targetVal));
                    std::string sym = ctx.GetSymbolName(targetVal);

                    ctx.Out("    [Fixup #%u] Slot RVA 0x%08X (VA: 0x%016llX) -> Target: 0x%016llX %s%s%s\n",
                            (unsigned)i, fixups[i].RVA, (unsigned long long)slotVa,
                            (unsigned long long)targetVal,
                            sym.empty() ? "" : "(", sym.c_str(), sym.empty() ? "" : ")");

                    totalVectors++;
                }
            } else {
                ctx.Out("  VTableFixups   : None (Pure metadata or pure managed assembly)\n");
            }
        }
    } else {
        ctx.Out("  Status         : ABSENT (Pure Native PE - No CLI metadata container)\n");
    }

    ctx.Out("\n================================================================================\n");
    ctx.Out("  INVOCATION SURFACE SUMMARY: %d Potential Entry Point(s) / Vectors Discovered\n", totalVectors);
    ctx.Out("================================================================================\n\n");

    return S_OK;
}
