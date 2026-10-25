// ============================================================================
// trace_events.h — Unified 3-Level UM Trace Event Model & Schema (C++20)
// ============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

// ----------------------------------------------------------------------------
// Trace Event Kinds (Level A, Level B, Level C)
// ----------------------------------------------------------------------------
enum class TraceEventKind {
    // Level A — PE Discovery (Candidates, not proof of execution)
    ImageLoad,
    ImageUnload,
    EntryCandidate,

    // Level B — Invocation Correlation (Probable link to launch mechanism)
    SymbolResolved,

    // Level C — Execution Trace (Confirmed control transfer observed by CPU)
    CallObserved,
    ControlTransfer,
    ReturnObserved
};

// ----------------------------------------------------------------------------
// Provenance of Evidence
// ----------------------------------------------------------------------------
enum class EvidenceSource {
    StaticAnalysis,       // Discovered via PE header, EAT, TLS, CFG, or .pdata
    ApiInstrumentation,   // Intercepted via LdrLoadDll, GetProcAddress, etc.
    DebuggerBreakpoint,   // Confirmed by CPU execution breakpoint hit
    RuntimeObservation    // ETW or DbgEng runtime loader event
};

// ----------------------------------------------------------------------------
// Confidence Level
// ----------------------------------------------------------------------------
enum class ConfidenceLevel {
    Candidate = 1,   // Structurally callable (Level A)
    Probable  = 2,   // Address resolved by caller (Level B)
    Confirmed = 3    // Instruction pointer reached target (Level C)
};

// ----------------------------------------------------------------------------
// Unified Trace Event Record
// ----------------------------------------------------------------------------
struct TraceEvent {
    uint64_t sequence_id     = 0;
    TraceEventKind kind       = TraceEventKind::EntryCandidate;
    EvidenceSource evidence   = EvidenceSource::StaticAnalysis;
    ConfidenceLevel confidence = ConfidenceLevel::Candidate;

    uint32_t pid             = 0;
    uint32_t tid             = 0;
    uint64_t timestamp_us    = 0;

    uint64_t source_va       = 0;  // Where execution jumped FROM (LR / caller PC)
    uint64_t target_va       = 0;  // Where execution jumped TO (target address)
    uint64_t return_va       = 0;  // Continuation address for return
    uint64_t module_base     = 0;  // Base of owning module

    uint64_t correlation_id  = 0;  // Links SymbolResolved -> CallObserved -> Return

    std::string module_name;
    std::string symbol_name;
    std::string details;
    std::vector<uint64_t> call_stack;

    const char* GetKindString() const {
        switch (kind) {
        case TraceEventKind::ImageLoad:       return "IMAGE_LOAD";
        case TraceEventKind::ImageUnload:     return "IMAGE_UNLOAD";
        case TraceEventKind::EntryCandidate:  return "ENTRY_CANDIDATE";
        case TraceEventKind::SymbolResolved:  return "SYMBOL_RESOLVED";
        case TraceEventKind::CallObserved:    return "CALL_OBSERVED";
        case TraceEventKind::ControlTransfer: return "CONTROL_TRANSFER";
        case TraceEventKind::ReturnObserved:  return "RETURN_OBSERVED";
        default:                              return "UNKNOWN";
        }
    }

    const char* GetEvidenceString() const {
        switch (evidence) {
        case EvidenceSource::StaticAnalysis:     return "StaticAnalysis";
        case EvidenceSource::ApiInstrumentation: return "ApiInstrumentation";
        case EvidenceSource::DebuggerBreakpoint: return "DebuggerBreakpoint";
        case EvidenceSource::RuntimeObservation:  return "RuntimeObservation";
        default:                                 return "Unknown";
        }
    }

    const char* GetConfidenceString() const {
        switch (confidence) {
        case ConfidenceLevel::Candidate: return "Candidate (Level A)";
        case ConfidenceLevel::Probable:  return "Probable (Level B)";
        case ConfidenceLevel::Confirmed: return "CONFIRMED (Level C)";
        default:                         return "Unknown";
        }
    }

    // JSONL Line Serializer
    std::string ToJson() const {
        std::ostringstream ss;
        ss << "{"
           << "\"seq\":" << sequence_id << ","
           << "\"kind\":\"" << GetKindString() << "\","
           << "\"evidence\":\"" << GetEvidenceString() << "\","
           << "\"confidence\":\"" << GetConfidenceString() << "\","
           << "\"pid\":" << pid << ","
           << "\"tid\":" << tid << ","
           << "\"timestamp_us\":" << timestamp_us << ","
           << "\"source_va\":\"0x" << std::hex << source_va << "\","
           << "\"target_va\":\"0x" << std::hex << target_va << "\","
           << "\"return_va\":\"0x" << std::hex << return_va << "\","
           << "\"module_base\":\"0x" << std::hex << module_base << "\","
           << "\"correlation_id\":" << std::dec << correlation_id << ","
           << "\"module\":\"" << module_name << "\","
           << "\"symbol\":\"" << symbol_name << "\","
           << "\"details\":\"" << details << "\"";

        if (!call_stack.empty()) {
            ss << ",\"stack\":[";
            for (size_t i = 0; i < call_stack.size(); ++i) {
                if (i > 0) ss << ",";
                ss << "\"0x" << std::hex << call_stack[i] << "\"";
            }
            ss << "]";
        }
        ss << "}";
        return ss.str();
    }
};
