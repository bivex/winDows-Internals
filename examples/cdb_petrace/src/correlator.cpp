// ============================================================================
// correlator.cpp — Invocation Correlation & Causal Chain Engine Implementation
// ============================================================================

#include "correlator.h"
#include <fstream>
#include <chrono>
#include <algorithm>

InvocationCorrelator& InvocationCorrelator::Instance() {
    static InvocationCorrelator instance;
    return instance;
}

uint64_t InvocationCorrelator::GetTimestampNow() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return (uli.QuadPart / 10); // microseconds since 1601
}

void InvocationCorrelator::Clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    modules_.clear();
    candidates_.clear();
    resolved_symbols_.clear();
    active_calls_by_tid_.clear();
    event_stream_.clear();
    causal_chains_.clear();
    next_seq_ = 1;
    next_chain_id_ = 1;
}

void InvocationCorrelator::RecordEventLocked(TraceEvent evt) {
    evt.sequence_id = next_seq_++;
    if (evt.timestamp_us == 0) {
        evt.timestamp_us = GetTimestampNow();
    }
    event_stream_.push_back(evt);
}

void InvocationCorrelator::RegisterModule(uint64_t base, uint32_t size, const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = modules_.find(base);
    if (it != modules_.end()) {
        it->second.ref_count++;
        TraceEvent evt;
        evt.kind = TraceEventKind::ModuleRefIncrement;
        evt.evidence = EvidenceSource::RuntimeObservation;
        evt.confidence = ConfidenceLevel::Confirmed;
        evt.module_base = base;
        evt.module_name = name;
        evt.target_va = base;
        evt.details = "Refcount incremented to " + std::to_string(it->second.ref_count) + " (Image already mapped; duplicate IMAGE_LOAD suppressed)";
        RecordEventLocked(evt);
        return;
    }

    LoadedModuleRecord rec;
    rec.base_va = base;
    rec.size = size;
    rec.ref_count = 1;
    rec.name = name;
    rec.load_time = GetTimestampNow();
    modules_[base] = rec;

    TraceEvent evt;
    evt.kind = TraceEventKind::ImageLoad;
    evt.evidence = EvidenceSource::RuntimeObservation;
    evt.confidence = ConfidenceLevel::Confirmed;
    evt.module_base = base;
    evt.module_name = name;
    evt.target_va = base;
    evt.details = "Initial Image Mapping (Size: 0x" + std::to_string(size) + ")";

    RecordEventLocked(evt);
}

void InvocationCorrelator::UnregisterModule(uint64_t base) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = modules_.find(base);
    if (it == modules_.end()) return;

    if (it->second.ref_count > 1) {
        it->second.ref_count--;
        TraceEvent evt;
        evt.kind = TraceEventKind::ModuleRefDecrement;
        evt.evidence = EvidenceSource::RuntimeObservation;
        evt.confidence = ConfidenceLevel::Confirmed;
        evt.module_base = base;
        evt.module_name = it->second.name;
        evt.target_va = base;
        evt.details = "Refcount decremented to " + std::to_string(it->second.ref_count) + " (Image remains mapped in memory)";
        RecordEventLocked(evt);
        return;
    }

    std::string modName = it->second.name;
    TraceEvent evt;
    evt.kind = TraceEventKind::ImageUnload;
    evt.evidence = EvidenceSource::RuntimeObservation;
    evt.confidence = ConfidenceLevel::Confirmed;
    evt.module_base = base;
    evt.module_name = modName;
    evt.target_va = base;
    evt.details = "Physical Image Unmapped";
    RecordEventLocked(evt);

    // Erase candidates belonging to this module to prevent stale address aliasing
    for (auto cIt = candidates_.begin(); cIt != candidates_.end(); ) {
        if (cIt->second.module_base == base) {
            cIt = candidates_.erase(cIt);
        } else {
            ++cIt;
        }
    }
    modules_.erase(base);
}

void InvocationCorrelator::AddCandidate(uint64_t target_va, uint64_t mod_base, const std::string& mod_name,
                                        const std::string& sym_name, const std::string& type_details) {
    std::lock_guard<std::mutex> lock(mtx_);
    TraceEvent evt;
    evt.kind = TraceEventKind::EntryCandidate;
    evt.evidence = EvidenceSource::StaticAnalysis;
    evt.confidence = ConfidenceLevel::Candidate;
    evt.target_va = target_va;
    evt.module_base = mod_base;
    evt.module_name = mod_name;
    evt.symbol_name = sym_name;
    evt.details = type_details;

    RecordEventLocked(evt);
    candidates_[target_va] = evt;
}

void InvocationCorrelator::RecordModuleLoadRequest(const std::string& module_name, uint32_t pid, uint32_t tid) {
    std::lock_guard<std::mutex> lock(mtx_);
    TraceEvent evt;
    evt.kind = TraceEventKind::ModuleLoadRequest;
    evt.evidence = EvidenceSource::ApiInstrumentation;
    evt.confidence = ConfidenceLevel::Probable;
    evt.pid = pid;
    evt.tid = tid;
    evt.symbol_name = module_name;
    evt.details = "Caller requested LdrLoadDll (image may or may not already be mapped)";
    RecordEventLocked(evt);
}

void InvocationCorrelator::OnThreadExit(uint32_t tid) {
    std::lock_guard<std::mutex> lock(mtx_);
    active_calls_by_tid_.erase(tid); // pending return-trap is unreachable
}

void InvocationCorrelator::OnException(uint32_t tid) {
    std::lock_guard<std::mutex> lock(mtx_);
    active_calls_by_tid_.erase(tid); // unwinding invalidates LR-continuation assumption
}

void InvocationCorrelator::RecordSymbolResolved(uint64_t target_va, const std::string& sym_name,
                                                uint32_t pid, uint32_t tid, uint64_t caller_va) {
    std::lock_guard<std::mutex> lock(mtx_);
    TraceEvent evt;
    evt.kind = TraceEventKind::SymbolResolved;
    evt.evidence = EvidenceSource::ApiInstrumentation;
    evt.confidence = ConfidenceLevel::Probable;
    evt.pid = pid;
    evt.tid = tid;
    evt.source_va = caller_va;
    evt.target_va = target_va;
    evt.symbol_name = sym_name;
    evt.correlation_id = next_chain_id_++;
    evt.details = "Resolved by GetProcAddress / LdrGetProcedureAddress";

    // Link module name if candidate exists
    auto cIt = candidates_.find(target_va);
    if (cIt != candidates_.end()) {
        evt.module_name = cIt->second.module_name;
        evt.module_base = cIt->second.module_base;
    }

    RecordEventLocked(evt);
    resolved_symbols_[target_va] = evt;
}

uint64_t InvocationCorrelator::RecordCallObserved(uint64_t target_va, uint64_t source_va, uint64_t return_va,
                                                  uint32_t pid, uint32_t tid,
                                                  const std::vector<uint64_t>& stack,
                                                  const std::string& details) {
    std::lock_guard<std::mutex> lock(mtx_);

    uint64_t chain_id = 0;
    auto rIt = resolved_symbols_.find(target_va);
    if (rIt != resolved_symbols_.end()) {
        chain_id = rIt->second.correlation_id;
    } else {
        chain_id = next_chain_id_++;
    }

    TraceEvent evt;
    evt.kind = TraceEventKind::CallObserved;
    evt.evidence = EvidenceSource::DebuggerBreakpoint;
    evt.confidence = ConfidenceLevel::Confirmed;
    evt.pid = pid;
    evt.tid = tid;
    evt.source_va = source_va;
    evt.target_va = target_va;
    evt.return_va = return_va;
    evt.correlation_id = chain_id;
    evt.call_stack = stack;
    evt.details = details;

    auto cIt = candidates_.find(target_va);
    if (cIt != candidates_.end()) {
        evt.module_name = cIt->second.module_name;
        evt.module_base = cIt->second.module_base;
        evt.symbol_name = cIt->second.symbol_name;
    }

    RecordEventLocked(evt);
    active_calls_by_tid_[tid] = evt;

    // Build or update CausalChain
    CausalChain chain;
    chain.chain_id = chain_id;
    chain.target_va = target_va;
    chain.caller_va = source_va;
    chain.tid = tid;
    chain.module_name = evt.module_name;
    chain.symbol_name = evt.symbol_name;

    if (cIt != candidates_.end()) {
        chain.has_candidate = true;
        chain.candidate_event = cIt->second;
    }
    if (rIt != resolved_symbols_.end()) {
        chain.has_resolved = true;
        chain.resolved_event = rIt->second;
    }
    chain.has_call = true;
    chain.call_event = evt;

    causal_chains_.push_back(chain);
    return chain_id;
}

void InvocationCorrelator::RecordReturnObserved(uint64_t return_va, uint32_t pid, uint32_t tid, uint64_t ret_value) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto actIt = active_calls_by_tid_.find(tid);
    uint64_t chain_id = (actIt != active_calls_by_tid_.end()) ? actIt->second.correlation_id : 0;
    std::string sym = (actIt != active_calls_by_tid_.end()) ? actIt->second.symbol_name : "";
    std::string mod = (actIt != active_calls_by_tid_.end()) ? actIt->second.module_name : "";

    TraceEvent evt;
    evt.kind = TraceEventKind::ReturnObserved;
    evt.evidence = EvidenceSource::DebuggerBreakpoint;
    evt.confidence = ConfidenceLevel::Confirmed;
    evt.pid = pid;
    evt.tid = tid;
    evt.source_va = return_va;
    evt.target_va = return_va;
    evt.return_va = return_va;
    evt.correlation_id = chain_id;
    evt.module_name = mod;
    evt.symbol_name = sym;

    char retBuf[64] = {};
    snprintf(retBuf, sizeof(retBuf), "Return Value = 0x%llX (%lld)", (unsigned long long)ret_value, (long long)ret_value);
    evt.details = retBuf;

    RecordEventLocked(evt);

    // Update matching causal chain
    for (auto& ch : causal_chains_) {
        if (ch.chain_id == chain_id && ch.has_call && !ch.has_return) {
            ch.has_return = true;
            ch.return_event = evt;
            ch.return_value = ret_value;
            break;
        }
    }

    if (actIt != active_calls_by_tid_.end()) {
        active_calls_by_tid_.erase(actIt);
    }
}

size_t InvocationCorrelator::GetEventCount() {
    std::lock_guard<std::mutex> lock(mtx_);
    return event_stream_.size();
}

size_t InvocationCorrelator::GetCandidateCount() {
    std::lock_guard<std::mutex> lock(mtx_);
    return candidates_.size();
}

size_t InvocationCorrelator::GetConfirmedCallCount() {
    std::lock_guard<std::mutex> lock(mtx_);
    size_t count = 0;
    for (const auto& ch : causal_chains_) {
        if (ch.has_call) count++;
    }

    return count;
}

size_t InvocationCorrelator::GetResolvedCount() {
    std::lock_guard<std::mutex> lock(mtx_);
    return resolved_symbols_.size();
}

void InvocationCorrelator::PrintSummary(void (*out_fn)(const char*, void*), void* context) {
    std::lock_guard<std::mutex> lock(mtx_);
    char buf[512] = {};

    snprintf(buf, sizeof(buf), "\n================================================================================\n"
                               "  CORRELATION ENGINE SUMMARY:\n"
                               "    Total Events Recorded      : %zu\n"
                               "    Level A Entry Candidates   : %zu\n"
                               "    Level B Symbols Resolved   : %zu\n"
                               "    Level C Confirmed Calls    : %zu\n"
                               "    Complete Causal Chains     : %zu\n"
                               "================================================================================\n",
             event_stream_.size(), candidates_.size(), resolved_symbols_.size(),
             causal_chains_.size(), causal_chains_.size());
    out_fn(buf, context);
}

void InvocationCorrelator::PrintCausalChains(void (*out_fn)(const char*, void*), void* context) {
    std::lock_guard<std::mutex> lock(mtx_);

    out_fn("\n================================================================================\n", context);
    out_fn("  CONFIRMED CAUSAL EXECUTION CHAINS (Level A -> Level B -> Level C):\n", context);
    out_fn("================================================================================\n", context);

    if (causal_chains_.empty()) {
        out_fn("  [info] No execution chains confirmed yet. Run target to hit entry candidates.\n", context);
        return;
    }

    int idx = 1;
    for (const auto& ch : causal_chains_) {
        char header[256] = {};
        snprintf(header, sizeof(header), "\n[CHAIN #%d] Target: 0x%016llX %s%s%s (TID: %u)\n",
                 idx++, (unsigned long long)ch.target_va,
                 ch.symbol_name.empty() ? "" : "(",
                 ch.symbol_name.empty() ? ch.module_name.c_str() : (ch.module_name + "!" + ch.symbol_name).c_str(),
                 ch.symbol_name.empty() ? "" : ")",
                 ch.tid);
        out_fn(header, context);

        if (ch.has_candidate) {
            char stepA[256] = {};
            snprintf(stepA, sizeof(stepA), "  [Level A - Candidate]   %s\n", ch.candidate_event.details.c_str());
            out_fn(stepA, context);
        } else {
            out_fn("  [Level A - Candidate]   Dynamic target (not declared in static PE headers)\n", context);
        }

        if (ch.has_resolved) {
            char stepB[256] = {};
            snprintf(stepB, sizeof(stepB), "  [Level B - Resolved]    GetProcAddress resolved from caller 0x%016llX\n",
                     (unsigned long long)ch.resolved_event.source_va);
            out_fn(stepB, context);
        } else {
            out_fn("  [Level B - Correlation] Direct call / Static import / Indirect transfer\n", context);
        }

        if (ch.has_call) {
            char stepC[256] = {};
            snprintf(stepC, sizeof(stepC), "  [Level C - CONFIRMED]   CPU instruction pointer reached target from 0x%016llX\n",
                     (unsigned long long)ch.call_event.source_va);
            out_fn(stepC, context);
            if (!ch.call_event.details.empty()) {
                char det[256] = {};
                snprintf(det, sizeof(det), "                          %s\n", ch.call_event.details.c_str());
                out_fn(det, context);
            }
        }

        if (ch.has_return) {
            char stepRet[256] = {};
            snprintf(stepRet, sizeof(stepRet), "  [Level C - Return]      Returned to 0x%016llX with %s\n",
                     (unsigned long long)ch.return_event.return_va, ch.return_event.details.c_str());
            out_fn(stepRet, context);
        }
    }
    out_fn("\n================================================================================\n\n", context);
}

bool InvocationCorrelator::ExportJsonl(const char* filepath) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::ofstream ofs(filepath);
    if (!ofs.is_open()) return false;

    for (const auto& evt : event_stream_) {
        ofs << evt.ToJson() << "\n";
    }
    return true;
}
