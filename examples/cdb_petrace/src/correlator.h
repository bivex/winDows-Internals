// ============================================================================
// correlator.h — Invocation Correlation & Causal Chain Engine
// ============================================================================

#pragma once

#include <windows.h>
#include "trace_events.h"
#include <map>
#include <vector>
#include <mutex>
#include <memory>

// ----------------------------------------------------------------------------
// Causal Execution Chain (Level A -> Level B -> Level C)
// ----------------------------------------------------------------------------
struct CausalChain {
    uint64_t chain_id       = 0;
    std::string module_name;
    std::string symbol_name;
    uint64_t target_va     = 0;
    uint64_t caller_va     = 0;
    uint32_t tid           = 0;

    bool has_candidate     = false; // Level A
    TraceEvent candidate_event;

    bool has_resolved      = false; // Level B
    TraceEvent resolved_event;

    bool has_call          = false; // Level C
    TraceEvent call_event;

    bool has_return        = false; // Level C return
    TraceEvent return_event;
    uint64_t return_value  = 0;
};

// ----------------------------------------------------------------------------
// Active Loaded Module Record (Tracks Lifetime to Prevent Stale VA Aliasing)
// ----------------------------------------------------------------------------
struct LoadedModuleRecord {
    uint64_t base_va = 0;
    uint32_t size    = 0;
    std::string name;
    uint64_t load_time = 0;
};

// ----------------------------------------------------------------------------
// InvocationCorrelator Class
// ----------------------------------------------------------------------------
class InvocationCorrelator {
public:
    static InvocationCorrelator& Instance();

    void Clear();

    // Level A: Module & Candidate Registration
    void RegisterModule(uint64_t base, uint32_t size, const std::string& name);
    void UnregisterModule(uint64_t base);
    void AddCandidate(uint64_t target_va, uint64_t mod_base, const std::string& mod_name,
                      const std::string& sym_name, const std::string& type_details);

    // Level B: Invocation Correlation
    void RecordSymbolResolved(uint64_t target_va, const std::string& sym_name,
                              uint32_t pid, uint32_t tid, uint64_t caller_va);

    // Level C: Execution Trace
    uint64_t RecordCallObserved(uint64_t target_va, uint64_t source_va, uint64_t return_va,
                                uint32_t pid, uint32_t tid,
                                const std::vector<uint64_t>& stack,
                                const std::string& details);
    void RecordReturnObserved(uint64_t return_va, uint32_t pid, uint32_t tid, uint64_t ret_value);

    // Reporting & Output
    void PrintSummary(void (*out_fn)(const char*, void*), void* context);
    void PrintCausalChains(void (*out_fn)(const char*, void*), void* context);
    bool ExportJsonl(const char* filepath);

    size_t GetEventCount();
    size_t GetCandidateCount();
    size_t GetConfirmedCallCount();

    const std::map<uint64_t, TraceEvent>& GetCandidates() const { return candidates_; }

private:
    InvocationCorrelator() = default;

    std::mutex mtx_;
    uint64_t next_seq_ = 1;
    uint64_t next_chain_id_ = 1;

    std::map<uint64_t, LoadedModuleRecord> modules_;
    std::map<uint64_t, TraceEvent> candidates_;             // target_va -> Candidate
    std::map<uint64_t, TraceEvent> resolved_symbols_;        // target_va -> Resolved
    std::map<uint32_t, TraceEvent> active_calls_by_tid_;     // tid -> In-flight call
    std::vector<TraceEvent> event_stream_;
    std::vector<CausalChain> causal_chains_;

    uint64_t GetTimestampNow();
    void RecordEventLocked(TraceEvent evt);
};
