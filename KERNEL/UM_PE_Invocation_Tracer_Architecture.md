# User-Mode (UM) PE Invocation Tracer: 3-Level Correlation Architecture (ARM64)

**Date:** 2026-10-26  
**Target:** Windows 11 Pro ARM64 (Build 26200.1, 24H2), Parallels Desktop 20.2.2 on Apple Silicon  
**Toolchain:** MSVC Optimizing Compiler Version `19.51.36246` for ARM64 (`/std:c++20`), Windows SDK `10.0.26100.0` (DbgEng SDK)  
**Debugger:** `cdb.exe` ARM64 (DbgEng `10.0.26100.6584`)  
**Target Extension:** `petrace.dll`  
**Workspace:** `/Volumes/External/Code/winDows-Internals/KERNEL/UM_PE_Invocation_Tracer_Architecture.md`  

---

## 1. Executive Summary: The Three-Level Tracing Model

Traditional security tools and reverse engineering scripts often conflate **the possibility of code execution** with **actual execution**. For instance, observing that an executable imports `ComComponent.dll` or that `DllGetClassObject` exists in the Export Address Table (EAT) does not prove that `DllGetClassObject` was ever invoked. Furthermore, seeing an instruction pointer inside an executable page does not necessarily signify a clean ABI function call (it could be a tail call, an indirect jump, or exception filter dispatch).

To eliminate false positives and establish an unbroken chain of custody for code execution, the **User-Mode (UM) PE Invocation Tracer (`petrace.dll`)** implements a strict **Three-Level Observation Model**:

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                    THE THREE-LEVEL OBSERVATION MODEL                         │
├──────────────────────────────────────────────────────────────────────────────┤
│ LEVEL A — PE DISCOVERY (Static Candidates)                                   │
│   • Scans headers via IDebugDataSpaces::ReadVirtual                          │
│   • Enumerates EntryPoint, TLS Callbacks, EAT, CFG valid targets, .pdata     │
│   • Outcome: ENTRY_CANDIDATE events (Confidence: Candidate / 0.2)            │
├──────────────────────────────────────────────────────────────────────────────┤
│                                      │                                       │
│                                      ▼                                       │
│ LEVEL B — INVOCATION CORRELATION (Address Resolution & Launch Link)          │
│   • Intercepts GetProcAddress / LdrGetProcedureAddress                       │
│   • Correlates dynamic address resolution with candidate VAs                 │
│   • Outcome: SYMBOL_RESOLVED events (Confidence: Probable / 0.6)             │
├──────────────────────────────────────────────────────────────────────────────┤
│                                      │                                       │
│                                      ▼                                       │
│ LEVEL C — EXECUTION TRACE (Confirmed Runtime Observation)                    │
│   • Non-halting automated breakpoint traps on candidates (!peonhit; g)       │
│   • Captures: CPU Instruction Pointer, Link Register (LR), PID, TID, stack   │
│   • Arms one-shot continuation trap (!peonret; g) capturing return value (x0)│
│   • Outcome: CALL_OBSERVED & RETURN_OBSERVED (Confidence: CONFIRMED / 1.0)   │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Event Taxonomy & Evidence Provenance

Every event captured by the correlation engine records its exact **provenance** and **confidence rating**:

| Event Kind | Level | Evidence Source | Confidence | Technical Meaning |
|---|---|---|---|---|
| `IMAGE_LOAD` | A | `RuntimeObservation` | Confirmed | Module mapped into process memory space. |
| `IMAGE_UNLOAD` | A | `RuntimeObservation` | Confirmed | Module unmapped; candidates purged to prevent address aliasing. |
| `ENTRY_CANDIDATE` | A | `StaticAnalysis` | Candidate | Structurally callable vector identified in PE headers. |
| `SYMBOL_RESOLVED` | B | `ApiInstrumentation` | Probable | Calling code explicitly queried function pointer via `GetProcAddress`. |
| `CALL_OBSERVED` | C | `DebuggerBreakpoint` | **CONFIRMED** | CPU Instruction Pointer physically reached function entry. |
| `CONTROL_TRANSFER` | C | `DebuggerBreakpoint` | **CONFIRMED** | Indirect branch (`br xn`) or jump table transition observed. |
| `RETURN_OBSERVED` | C | `DebuggerBreakpoint` | **CONFIRMED** | Control safely returned to caller continuation address with return value. |

---

## 3. The Causal Execution Chain

The `InvocationCorrelator` links discrete events across process lifecycles into a unified causal graph:

$$\text{Causal Chain} = \text{ImageLoad} \longrightarrow \text{EntryCandidate} \longrightarrow [\text{SymbolResolved}] \longrightarrow \text{CallObserved} \longrightarrow \text{ReturnObserved}$$

### Example Correlated Chain in `ComComponent.dll`:
```text
[CHAIN #1] Target: 0x00007FF9B76315B0 (ComComponent!DllGetClassObject) (TID: 5824)
  [Level A - Candidate]   EAT Export #2 (RVA 0x000015B0) declared in ComComponent.dll
  [Level B - Resolved]    GetProcAddress resolved from caller 0x00007FF612341200
  [Level C - CONFIRMED]   CPU instruction pointer reached target from 0x00007FF612341250
                          Parameters: Args: x0=0x00007FF9B7632B00, x1=0x0000000000000001
  [Level C - Return]      Returned to 0x00007FF612341254 with Return Value = 0x0 (S_OK)
```

---

## 4. Automated Non-Halting Trapping Engine (`!peonhit` & `!peonret`)

To observe production binaries without hanging processes or requiring interactive debugger commands:
1. `!petrace <module>` parses the target PE in memory and sets code breakpoints on all Level A candidates:
   ```cpp
   bp->SetCommand("!peonhit; g");
   ```
2. When the CPU reaches the candidate address, CDB pauses execution, executes `!peonhit`, logs registers (`x0`..`x7`), evaluates the Link Register (`lr` / `x30` on ARM64), captures the stack, and automatically resumes (`g`).
3. `!peonhit` dynamically installs a one-shot breakpoint on `lr`:
   ```cpp
   retBp->SetCommand("!peonret; g");
   ```
4. When the function returns, `!peonret` reads the return register `x0` and resumes immediately.

This guarantees **zero human intervention, zero process freezing, and 100% verifiable Level C execution evidence**.

---

## 5. JSONL Event Streaming

For external SIEM, automated fuzzing, or security telemetry ingestion, `petrace` exports the raw event stream via `!peexport <path.jsonl>`:

```json
{"seq":1,"kind":"IMAGE_LOAD","evidence":"RuntimeObservation","confidence":"CONFIRMED (Level C)","pid":6288,"tid":1024,"timestamp_us":13345210000000,"source_va":"0x0","target_va":"0x7ff9b7630000","return_va":"0x0","module_base":"0x7ff9b7630000","correlation_id":0,"module":"ComComponent.dll","symbol":"","details":"Size: 0x28000"}
{"seq":2,"kind":"ENTRY_CANDIDATE","evidence":"StaticAnalysis","confidence":"Candidate (Level A)","pid":0,"tid":0,"timestamp_us":13345210000010,"source_va":"0x0","target_va":"0x7ff9b76315b0","return_va":"0x0","module_base":"0x7ff9b7630000","correlation_id":0,"module":"ComComponent.dll","symbol":"DllGetClassObject","details":"Exported Function"}
{"seq":3,"kind":"CALL_OBSERVED","evidence":"DebuggerBreakpoint","confidence":"CONFIRMED (Level C)","pid":6288,"tid":1024,"timestamp_us":13345210000050,"source_va":"0x7ff612341250","target_va":"0x7ff9b76315b0","return_va":"0x7ff612341254","module_base":"0x7ff9b7630000","correlation_id":1,"module":"ComComponent.dll","symbol":"DllGetClassObject","details":"Args: x0=0x00007FF9B7632B00, x1=0x0000000000000001"}
{"seq":4,"kind":"RETURN_OBSERVED","evidence":"DebuggerBreakpoint","confidence":"CONFIRMED (Level C)","pid":6288,"tid":1024,"timestamp_us":13345210000060,"source_va":"0x7ff612341254","target_va":"0x7ff612341254","return_va":"0x7ff612341254","module_base":"0x7ff9b7630000","correlation_id":1,"module":"ComComponent.dll","symbol":"DllGetClassObject","details":"Return Value = 0x0 (0)"}
```
