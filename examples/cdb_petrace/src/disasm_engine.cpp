// ============================================================================
// disasm_engine.cpp — Capstone-based basic block & branch CFG reconstruction
//
// Instruction decoding is delegated to the Capstone disassembly engine
// (cs_disasm + instruction groups), replacing the previous hand-rolled ARM64
// bit-pattern decoder. This gives correct classification of every branch
// flavor on ARM64 AND x64 (target arch selected via
// IDebugControl::GetEffectiveProcessorType):
//   - CS_GRP_CALL   : bl / blr (ARM64), call (x64)
//   - CS_GRP_JUMP   : b / b.cond / cbz / cbnz / tbz / tbnz / br, jmp / jcc
//   - CS_GRP_RET    : ret / retaa / retab, ret / iretd...
// Direct targets are read from the last IMM operand; REG/MEM operands mark
// indirect transfers (jump tables, vtable slots, CFG-guarded dispatch).
// ============================================================================

#include "petrace.h"
#include <capstone/capstone.h>
#include <capstone/arm64.h>
#include <sstream>
#include <queue>
#include <algorithm>

// ----------------------------------------------------------------------------
// Basic Block Representation
// ----------------------------------------------------------------------------
struct BasicBlock {
    ULONG64 startVa = 0;
    ULONG64 endVa = 0;
    std::vector<std::pair<ULONG64, std::string>> instructions;
    std::vector<ULONG64> successors;     // Branch targets (Taken, Fallthrough)
    std::vector<ULONG64> callTargets;    // Function calls
    bool isTerminal = false;             // Returns or halts
    bool isIndirectBranch = false;       // br xn / jmp reg (jump table, vtable)
    std::string branchType;
};

// ----------------------------------------------------------------------------
// Capstone session management (capstone 5.x API: CS_ARCH_ARM64)
// ----------------------------------------------------------------------------
struct CsSession {
    csh handle = 0;
    bool valid = false;
    bool isX64 = false;
    cs_arch arch = CS_ARCH_ARM64;
};

static bool CsInit(DbgEngContext& ctx, CsSession& s) {
    static CsSession cached = {};
    if (cached.valid) { s = cached; return true; }

    ULONG procType = 0;
    if (ctx.Control) ctx.Control->GetEffectiveProcessorType(&procType);

    cached.arch = CS_ARCH_ARM64;
    cs_mode mode = CS_MODE_LITTLE_ENDIAN;
    if (procType == IMAGE_FILE_MACHINE_AMD64) {
        cached.arch = CS_ARCH_X86;
        mode = (cs_mode)(CS_MODE_64 | CS_MODE_LITTLE_ENDIAN);
        cached.isX64 = true;
    }

    if (cs_open(cached.arch, mode, &cached.handle) != CS_ERR_OK) {
        cached.handle = 0;
        return false;
    }
    cs_option(cached.handle, CS_OPT_DETAIL, CS_OPT_ON);
    cached.valid = true;
    s = cached;
    return true;
}

// ----------------------------------------------------------------------------
// Branch classification via Capstone groups + IMM operands
// ----------------------------------------------------------------------------
enum BranchCategory {
    BRANCH_NONE,
    BRANCH_UNCONDITIONAL_DIRECT,   // b <target>, jmp <target>
    BRANCH_CONDITIONAL_DIRECT,     // b.cond, cbz, tbz, jcc
    BRANCH_CALL_DIRECT,            // bl <target>, call <target>
    BRANCH_CALL_INDIRECT,          // blr xn, call reg / call [mem]
    BRANCH_INDIRECT,               // br xn, jmp reg
    BRANCH_RETURN                  // ret / retaa / retab
};

static BranchCategory ClassifyInsn(csh h, const cs_insn& insn, ULONG64* directTarget, cs_arch arch) {
    if (directTarget) *directTarget = 0;

    if (cs_insn_group(h, &insn, CS_GRP_RET))  return BRANCH_RETURN;
    if (cs_insn_group(h, &insn, CS_GRP_IRET)) return BRANCH_RETURN;

    bool isCall = cs_insn_group(h, &insn, CS_GRP_CALL);
    bool isJump = cs_insn_group(h, &insn, CS_GRP_JUMP);
    if (!isCall && !isJump) return BRANCH_NONE;

    // Direct target = last immediate operand (ARM64 b/bl put the target in
    // op0; cbz/cbnz/tbz/tbnz put it last; x64 jmp/call rel uses op0 IMM).
    const cs_detail* d = insn.detail;
    uint64_t imm = 0;
    bool hasImm = false;

    if (arch == CS_ARCH_ARM64) {
        for (uint8_t i = 0; i < d->arm64.op_count; ++i) {
            if (d->arm64.operands[i].type == ARM64_OP_IMM) {
                imm = (uint64_t)d->arm64.operands[i].imm;
                hasImm = true; // keep last IMM (cbz/tbz: target is last)
            }
        }
    } else { // CS_ARCH_X86
        for (uint8_t i = 0; i < d->x86.op_count; ++i) {
            if (d->x86.operands[i].type == X86_OP_IMM) {
                imm = (uint64_t)d->x86.operands[i].imm;
                hasImm = true;
            }
        }
    }

    if (isCall) {
        if (hasImm) {
            if (directTarget) *directTarget = imm;
            return BRANCH_CALL_DIRECT;
        }
        return BRANCH_CALL_INDIRECT;
    }

    // Jumps
    if (hasImm) {
        if (directTarget) *directTarget = imm;
        bool conditional;
        if (arch == CS_ARCH_ARM64) {
            conditional = (insn.id == ARM64_INS_CBZ || insn.id == ARM64_INS_CBNZ ||
                           insn.id == ARM64_INS_TBZ || insn.id == ARM64_INS_TBNZ ||
                           d->arm64.cc != ARM64_CC_INVALID);
        } else {
            conditional = (insn.id != X86_INS_JMP);
        }
        return conditional ? BRANCH_CONDITIONAL_DIRECT : BRANCH_UNCONDITIONAL_DIRECT;
    }
    return BRANCH_INDIRECT;
}

// ----------------------------------------------------------------------------
// AnalyzeControlFlowGraph Implementation
// ----------------------------------------------------------------------------
HRESULT AnalyzeControlFlowGraph(DbgEngContext& ctx, const char* args) {
    if (!args || args[0] == '\0') {
        ctx.Out("[error] Missing function symbol or address.\n");
        ctx.Out("        Usage: !pecfg <function_symbol | address> [mermaid]\n");
        return E_INVALIDARG;
    }

    char targetName[256] = {};
    char mode[64] = {};
    sscanf(args, "%255s %63s", targetName, mode);
    bool outputMermaid = (strcmp(mode, "mermaid") == 0);

    ULONG64 startVa = 0;
    if ((targetName[0] == '0' && (targetName[1] == 'x' || targetName[1] == 'X')) ||
        (isxdigit(targetName[0]) && strlen(targetName) >= 8 && strchr(targetName, '!') == nullptr)) {
        startVa = strtoull(targetName, nullptr, 16);
    } else {
        if (ctx.Control) {
            DEBUG_VALUE val = {};
            if (SUCCEEDED(ctx.Control->Evaluate(targetName, DEBUG_VALUE_INT64, &val, nullptr))) {
                startVa = (ULONG64)val.I64;
            }
        }
        if (startVa < 0x10000 && ctx.Symbols) {
            ctx.Symbols->GetOffsetByName(targetName, &startVa);
        }
        if (startVa < 0x10000) {
            // Fallback: resolve via Module Export Address Table (EAT)
            const char* bang = strchr(targetName, '!');
            if (bang) {
                std::string modPart(targetName, bang - targetName);
                const char* fnPart = bang + 1;
                ULONG64 modBase = ctx.ResolveModuleBase(modPart.c_str());
                if (modBase != 0) {
                    IMAGE_DOS_HEADER dos = {};
                    IMAGE_NT_HEADERS64 nt = {};
                    if (ctx.ReadMemory(modBase, &dos, sizeof(dos)) &&
                        ctx.ReadMemory(modBase + dos.e_lfanew, &nt, sizeof(nt))) {
                        auto& expDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                        if (expDir.VirtualAddress != 0) {
                            IMAGE_EXPORT_DIRECTORY exp = {};
                            ctx.ReadMemory(modBase + expDir.VirtualAddress, &exp, sizeof(exp));
                            std::vector<DWORD> funcRvas(exp.NumberOfFunctions);
                            std::vector<DWORD> nameRvas(exp.NumberOfNames);
                            std::vector<WORD>  ordinals(exp.NumberOfNames);
                            ctx.ReadMemory(modBase + exp.AddressOfFunctions, funcRvas.data(), (ULONG)(funcRvas.size() * sizeof(DWORD)));
                            ctx.ReadMemory(modBase + exp.AddressOfNames, nameRvas.data(), (ULONG)(nameRvas.size() * sizeof(DWORD)));
                            ctx.ReadMemory(modBase + exp.AddressOfNameOrdinals, ordinals.data(), (ULONG)(ordinals.size() * sizeof(WORD)));

                            for (DWORD i = 0; i < exp.NumberOfNames; ++i) {
                                char expName[256] = {};
                                ctx.ReadMemory(modBase + nameRvas[i], expName, sizeof(expName) - 1);
                                if (_stricmp(expName, fnPart) == 0) {
                                    startVa = modBase + funcRvas[ordinals[i]];
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (startVa == 0) {
        ctx.Out("[error] Could not resolve address for: \"%s\"\n", targetName);
        return E_FAIL;
    }

    CsSession cs;
    if (!CsInit(ctx, cs)) {
        ctx.Out("[error] Failed to initialize capstone disassembler.\n");
        return E_FAIL;
    }

    std::string funcSym = ctx.GetSymbolName(startVa);
    ctx.Out("\n================================================================================\n");
    ctx.Out("  CONTROL FLOW GRAPH (capstone %s decoder): %s\n",
            cs.isX64 ? "x64" : "ARM64",
            funcSym.empty() ? targetName : funcSym.c_str());
    ctx.Out("  Entry Address  : 0x%016llX\n", (unsigned long long)startVa);
    ctx.Out("================================================================================\n");

    std::map<ULONG64, BasicBlock> blocks;
    std::queue<ULONG64> workQueue;
    workQueue.push(startVa);

    std::set<ULONG64> visitedLeaders;
    const size_t kMaxInstructions = 400;
    size_t totalInsnCount = 0;

    while (!workQueue.empty() && totalInsnCount < kMaxInstructions) {
        ULONG64 blockStart = workQueue.front();
        workQueue.pop();

        if (visitedLeaders.count(blockStart) > 0) continue;
        visitedLeaders.insert(blockStart);

        BasicBlock bb;
        bb.startVa = blockStart;

        ULONG64 pc = blockStart;
        bool blockEnded = false;

        while (!blockEnded && totalInsnCount < kMaxInstructions) {
            uint8_t code[16] = {};
            if (!ctx.ReadMemory(pc, code, sizeof(code))) break;

            cs_insn* insnPtr = nullptr;
            size_t count = cs_disasm(cs.handle, code, sizeof(code), pc, 1, &insnPtr);
            if (count == 0) {
                // Undecodable word: emit raw hex and advance (4 on ARM64, 1 on x64)
                char raw[64] = {};
                uint32_t word = 0;
                memcpy(&word, code, 4);
                snprintf(raw, sizeof(raw), "%s0x%08x  (undecoded)",
                         cs.isX64 ? "db " : ".word ", word);
                bb.instructions.push_back({ pc, raw });
                totalInsnCount++;
                bb.endVa = pc;
                pc += cs.isX64 ? 1 : 4;
                continue;
            }

            const cs_insn& insn = *insnPtr;
            totalInsnCount++;
            char line[256] = {};
            snprintf(line, sizeof(line), "%-8s %s", insn.mnemonic, insn.op_str);
            bb.instructions.push_back({ pc, line });

            ULONG64 nextPc = pc + insn.size;
            ULONG64 direct = 0;
            BranchCategory cat = ClassifyInsn(cs.handle, insn, &direct, cs.arch);

            switch (cat) {
            case BRANCH_RETURN:
                bb.isTerminal = true;
                bb.branchType = "return";
                blockEnded = true;
                break;

            case BRANCH_UNCONDITIONAL_DIRECT:
                bb.branchType = "jump";
                bb.successors.push_back(direct);
                if (visitedLeaders.count(direct) == 0) workQueue.push(direct);
                blockEnded = true;
                break;

            case BRANCH_CONDITIONAL_DIRECT:
                bb.branchType = "cond_branch";
                bb.successors.push_back(direct);            // Taken edge
                if (visitedLeaders.count(direct) == 0) workQueue.push(direct);
                bb.successors.push_back(nextPc);            // Fallthrough edge
                if (visitedLeaders.count(nextPc) == 0) workQueue.push(nextPc);
                blockEnded = true;
                break;

            case BRANCH_CALL_DIRECT:
                bb.callTargets.push_back(direct);
                break;

            case BRANCH_CALL_INDIRECT:
                bb.branchType = "indirect_call";
                break;

            case BRANCH_INDIRECT:
                bb.isIndirectBranch = true;
                bb.branchType = "indirect_jump";
                blockEnded = true;
                break;

            default:
                break;
            }

            bb.endVa = pc;
            pc = nextPc;
            cs_free(insnPtr, 1);
        }

        blocks[blockStart] = bb;
    }

    ctx.Out("[cfg] Basic Blocks Discovered: %zu | Total Instructions Traced: %zu\n\n",
            blocks.size(), totalInsnCount);

    if (outputMermaid) {
        ctx.Out("```mermaid\ngraph TD;\n");
        for (const auto& [bStart, bb] : blocks) {
            char bId[64] = {};
            snprintf(bId, sizeof(bId), "BB_%llX", (unsigned long long)bStart);
            std::string label = "Block 0x" + std::to_string(bStart) + "\\n";
            for (const auto& [iVa, dis] : bb.instructions) {
                label += dis + "\\n";
            }
            ctx.Out("  %s[\"%s\"];\n", bId, label.c_str());
            for (size_t s = 0; s < bb.successors.size(); ++s) {
                char sId[64] = {};
                snprintf(sId, sizeof(sId), "BB_%llX", (unsigned long long)bb.successors[s]);
                const char* edgeLabel = (bb.successors.size() == 2) ? ((s == 0) ? "Taken" : "Fallthrough") : "Jump";
                ctx.Out("  %s -->|%s| %s;\n", bId, edgeLabel, sId);
            }
            if (bb.isTerminal) {
                ctx.Out("  %s --> Exit([Function Return]);\n", bId);
            }
        }
        ctx.Out("```\n\n");
    } else {
        ctx.Out("--------------------------------------------------------------------------------\n");
        ctx.Out("  BASIC BLOCK TOPOLOGY & EXECUTION EDGES:\n");
        ctx.Out("--------------------------------------------------------------------------------\n");

        for (const auto& [bStart, bb] : blocks) {
            ctx.Out("\n[BLOCK 0x%016llX - 0x%016llX] (%zu insns)%s\n",
                    (unsigned long long)bb.startVa, (unsigned long long)bb.endVa,
                    bb.instructions.size(),
                    (bStart == startVa) ? " <-- [ENTRY]" : "");

            for (const auto& [iVa, dis] : bb.instructions) {
                ctx.Out("    0x%016llX: %s\n", (unsigned long long)iVa, dis.c_str());
            }

            for (ULONG64 callTgt : bb.callTargets) {
                std::string cSym = ctx.GetSymbolName(callTgt);
                ctx.Out("    ==> Calls: 0x%016llX %s%s%s\n",
                        (unsigned long long)callTgt,
                        cSym.empty() ? "" : "(", cSym.c_str(), cSym.empty() ? "" : ")");
            }

            if (bb.isTerminal) {
                ctx.Out("    \\-- Terminal: ret (Exits to caller)\n");
            } else if (bb.isIndirectBranch) {
                ctx.Out("    \\-- Indirect Jump: br/jmp reg (Dynamic jump table / vtable)\n");
            } else if (bb.successors.size() == 1) {
                ctx.Out("    \\-- Direct Jump ---> [Block 0x%016llX]\n",
                        (unsigned long long)bb.successors[0]);
            } else if (bb.successors.size() == 2) {
                ctx.Out("    |-- Branch Taken ---> [Block 0x%016llX]\n",
                        (unsigned long long)bb.successors[0]);
                ctx.Out("    \\-- Fallthrough  ---> [Block 0x%016llX]\n",
                        (unsigned long long)bb.successors[1]);
            }
        }
    }

    ctx.Out("\n================================================================================\n\n");
    return S_OK;
}
