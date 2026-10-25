// ============================================================================
// execution_tracer.h — Dynamic Level C Execution Tracer Interface
// ============================================================================

#pragma once

#include "petrace.h"
#include "correlator.h"

// ----------------------------------------------------------------------------
// Execution Tracer Prototypes
// ----------------------------------------------------------------------------

// Sets up candidate traps on all discovered Level A vectors for the module
HRESULT ArmCandidateBreakpoints(DbgEngContext& ctx, const char* moduleName);

// Handlers for breakpoint traps triggered during target execution
HRESULT OnBreakpointHit(DbgEngContext& ctx);
HRESULT OnReturnHit(DbgEngContext& ctx);

// Removes all active tracer breakpoints
HRESULT DisarmAllBreakpoints(DbgEngContext& ctx);
