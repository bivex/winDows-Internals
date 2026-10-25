// ============================================================================
// disasm_engine.h — Control Flow Graph (CFG) & Basic Block Analyzer Interface
// ============================================================================

#pragma once

#include "petrace.h"

HRESULT AnalyzeControlFlowGraph(DbgEngContext& ctx, const char* args);
