// ============================================================================
// pe_analyzer.h — PE Invocation Vectors Analyzer Interface
// ============================================================================

#pragma once

#include "petrace.h"

HRESULT AnalyzePePaths(DbgEngContext& ctx, const char* args);
