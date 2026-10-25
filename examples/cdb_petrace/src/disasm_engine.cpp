// ============================================================================
// disasm_engine.cpp — ARM64/x64 Basic Block & Branch CFG Reconstruction
// ============================================================================

#include "petrace.h"
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
    bool isIndirectBranch = false;       // br xn / blr xn
    std::string branchType;
};

// ----------------------------------------------------------------------------
// Branch Instruction Classifier
// ----------------------------------------------------------------------------
enum BranchCategory {
    BRANCH_NONE,
    BRANCH_UNCONDITIONAL_DIRECT,   // b <target>
    BRANCH_CONDITIONAL_DIRECT,     // b.cond, cbz, cbnz, tbz, tbnz
    BRANCH_CALL_DIRECT,            // bl <target>
    BRANCH_CALL_INDIRECT,          // blr xn
    BRANCH_INDIRECT,               // br xn
    BRANCH_RETURN                  // ret
};

static BranchCategory ClassifyInstruction(ULONG64 va, uint32_t rawInsn, const std::string& disasm,
                                          ULONG64* branchTarget, ULONG64* callTarget) {
    if (branchTarget) *branchTarget = 0;
    if (callTarget) *callTarget = 0;

    // ARM64 Direct Opcode Decoding:
    // 1. ret: 0xd65f03c0 (ret lr) or 0xd65f0xxx
    if ((rawInsn & 0xFFFFFC1F) == 0xD65F0000 || disasm.find("ret") != std::string::npos) {
        return BRANCH_RETURN;
    }

    // 2. bl <target>: 0x94000000 (Imm26 signed offset * 4)
    if ((rawInsn & 0xFC000000) == 0x94000000) {
        int32_t imm26 = (int32_t)(rawInsn & 0x03FFFFFF);
        // Sign extend from 26 to 32 bits
        if (imm26 & 0x02000000) imm26 |= 0xFC000000;
        int64_t offset = (int64_t)imm26 * 4;
        if (callTarget) *callTarget = va + offset;
        return BRANCH_CALL_DIRECT;
    }

    // 3. b <target>: 0x14000000 (Imm26 signed offset * 4)
    if ((rawInsn & 0xFC000000) == 0x14000000) {
        int32_t imm26 = (int32_t)(rawInsn & 0x03FFFFFF);
        if (imm26 & 0x02000000) imm26 |= 0xFC000000;
        int64_t offset = (int64_t)imm26 * 4;
        if (branchTarget) *branchTarget = va + offset;
        return BRANCH_UNCONDITIONAL_DIRECT;
    }

    // 4. b.cond <target>: 0x54000000 (Imm19 signed offset * 4)
    if ((rawInsn & 0xFF000010) == 0x54000000) {
        int32_t imm19 = (int32_t)((rawInsn >> 5) & 0x0007FFFF);
        if (imm19 & 0x00040000) imm19 |= 0xFFF80000;
        int64_t offset = (int64_t)imm19 * 4;
        if (branchTarget) *branchTarget = va + offset;
        return BRANCH_CONDITIONAL_DIRECT;
    }

    // 5. cbz / cbnz <reg>, <target>: 0x34000000 / 0x35000000 (Imm19 * 4)
    if ((rawInsn & 0x7E000000) == 0x34000000) {
        int32_t imm19 = (int32_t)((rawInsn >> 5) & 0x0007FFFF);
        if (imm19 & 0x00040000) imm19 |= 0xFFF80000;
        int64_t offset = (int64_t)imm19 * 4;
        if (branchTarget) *branchTarget = va + offset;
        return BRANCH_CONDITIONAL_DIRECT;
    }

    // 6. tbz / tbnz <reg>, #bit, <target>: 0x36000000 / 0x37000000 (Imm14 * 4)
    if ((rawInsn & 0x7E000000) == 0x36000000) {
        int32_t imm14 = (int32_t)((rawInsn >> 5) & 0x00003FFF);
        if (imm14 & 0x00002000) imm14 |= 0xFFFFC000;
        int64_t offset = (int64_t)imm14 * 4;
        if (branchTarget) *branchTarget = va + offset;
        return BRANCH_CONDITIONAL_DIRECT;
    }

    // 7. blr <reg>: 0xd63f0000
    if ((rawInsn & 0xFFFFFC1F) == 0xD63F0000 || disasm.find("blr ") != std::string::npos) {
        return BRANCH_CALL_INDIRECT;
    }

    // 8. br <reg>: 0xd61f0000
    if ((rawInsn & 0xFFFFFC1F) == 0xD61F0000 || disasm.find("br ") != std::string::npos) {
        return BRANCH_INDIRECT;
    }

    return BRANCH_NONE;
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

    // Parse arguments: function name and optional "mermaid" flag
    char targetName[256] = {};
    char mode[64] = {};
    sscanf(args, "%255s %63s", targetName, mode);

    bool outputMermaid = (strcmp(mode, "mermaid") == 0);

    ULONG64 startVa = 0;
    // Check if hex address
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
                                    WORD ord = ordinals[i];
                                    startVa = modBase + funcRvas[ord];
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

    std::string funcSym = ctx.GetSymbolName(startVa);
    ctx.Out("\n================================================================================\n");
    ctx.Out("  CONTROL FLOW GRAPH (CFG) RECONSTRUCTION: %s\n", funcSym.empty() ? targetName : funcSym.c_str());
    ctx.Out("  Entry Address  : 0x%016llX\n", (unsigned long long)startVa);
    ctx.Out("================================================================================\n");

    std::map<ULONG64, BasicBlock> blocks;
    std::set<ULONG64> leaders;
    leaders.insert(startVa);

    std::queue<ULONG64> workQueue;
    workQueue.push(startVa);

    std::set<ULONG64> visitedLeaders;
    const size_t kMaxInstructions = 400;
    size_t totalInsnCount = 0;

    // Phase 1: Disassemble and find all leaders & blocks
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
            uint32_t raw = 0;
            ctx.ReadMemory(pc, &raw, 4);

            ULONG64 nextPc = 0;
            std::string disasm = ctx.DisassembleInstruction(pc, &nextPc);
            totalInsnCount++;

            bb.instructions.push_back({ pc, disasm });

            ULONG64 branchTarget = 0;
            ULONG64 callTarget = 0;
            BranchCategory cat = ClassifyInstruction(pc, raw, disasm, &branchTarget, &callTarget);

            switch (cat) {
            case BRANCH_RETURN:
                bb.isTerminal = true;
                bb.branchType = "return";
                blockEnded = true;
                break;

            case BRANCH_UNCONDITIONAL_DIRECT:
                bb.branchType = "jump";
                if (branchTarget != 0) {
                    bb.successors.push_back(branchTarget);
                    leaders.insert(branchTarget);
                    if (visitedLeaders.count(branchTarget) == 0) workQueue.push(branchTarget);
                }
                blockEnded = true;
                break;

            case BRANCH_CONDITIONAL_DIRECT:
                bb.branchType = "cond_branch";
                // Edge 1: Taken
                if (branchTarget != 0) {
                    bb.successors.push_back(branchTarget);
                    leaders.insert(branchTarget);
                    if (visitedLeaders.count(branchTarget) == 0) workQueue.push(branchTarget);
                }
                // Edge 2: Fallthrough
                bb.successors.push_back(nextPc);
                leaders.insert(nextPc);
                if (visitedLeaders.count(nextPc) == 0) workQueue.push(nextPc);
                blockEnded = true;
                break;

            case BRANCH_CALL_DIRECT:
                if (callTarget != 0) {
                    bb.callTargets.push_back(callTarget);
                }
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
        }

        blocks[blockStart] = bb;
    }

    ctx.Out("[cfg] Basic Blocks Discovered: %zu | Total Instructions Traced: %zu\n\n",
            blocks.size(), totalInsnCount);

    // Phase 2: Output CFG
    if (outputMermaid) {
        ctx.Out("```mermaid\ngraph TD;\n");
        for (const auto& [bStart, bb] : blocks) {
            char bId[64] = {};
            snprintf(bId, sizeof(bId), "BB_%llX", (unsigned long long)bStart);

            std::string label = "Block 0x" + std::to_string(bStart) + "\\n";
            for (const auto& [iVa, dis] : bb.instructions) {
                // escape quotes
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
        // ASCII Tree Output
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
                ctx.Out("    \\-- Indirect Jump: br xn (Dynamic jump table / vtable)\n");
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
