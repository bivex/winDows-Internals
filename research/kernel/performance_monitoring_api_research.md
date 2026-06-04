# Performance Monitoring APIs — Kernel Path Research

> WinDbg Kernel Debugging | Windows 11 ARM64 | Parallels Desktop
> Target: ntoskrnl.exe (kernel) — ARM64 disassembly

---

## 1. OpenProcessToken + NtQueryInformationProcess

### 1.1 OpenProcessToken → NtOpenProcessToken

`NtOpenProcessToken` is a 3-instruction trampoline:

```
nt!NtOpenProcessToken:
fffff800`6fb25c80 aa0203e3  mov x3, x2       ; HandleAttributes → arg4
fffff800`6fb25c84 52800002  mov w2, #0       ; flags = 0
fffff800`6fb25c88 14000002  b  nt!NtOpenProcessTokenEx
```

All real work is in `NtOpenProcessTokenEx`. The trampoline sets `flags=0` and passes HandleAttributes as the 4th argument.

**Path:** `OpenProcessToken` → `NtOpenProcessToken` (trampoline) → `NtOpenProcessTokenEx`

### 1.2 NtQueryInformationProcess — Process Times

Entry at `fffff800`6fadedf0`. Uses jump-table dispatch on info class number.

**Kernel data sources for process times:**
- `KPROCESS+0x128`: CycleTime (Uint8B) — accumulated cycle time
- `KPROCESS+0x140`: KernelTime (Uint8B) — 100ns units
- `KPROCESS+0x148`: UserTime (Uint8B) — 100ns units
- `CreateTime` is at `EPROCESS` level, not `KPROCESS`

### 1.3 SeQueryInformationToken

Jump-table dispatch with **52 info classes** (0x34). Takes a shared resource lock (`ExAcquireSharedWaitForExclusive`) for most queries.

---

## 2. QueryProcessCycleTime / QueryThreadCycleTime

### 2.1 Thread Cycle Time — KeQueryTotalCycleTimeThread

**Fast path** (same CPU as thread):
1. Disable interrupts (`msr daifset, #2`)
2. Call `KiEndThreadCycleAccumulation`
3. Read `KTHREAD+0x048` (CycleTime)
4. Call `KiStartThreadCycleAccumulation`
5. Re-enable interrupts
6. Return CycleTime value

### 2.2 Process Cycle Time — PsQueryTotalCycleTimeProcess

~0x45 instructions, compact function:

```
1. KeFlushProcessWriteBuffers
2. KeUpdateTotalCyclesCurrentThread     ; flush current thread's cycles
3. PspLockProcessThreadListShared       ; acquire shared lock on thread list
4. Walk thread list at KPROCESS+0x360:
   - Sum KPROCESS.CycleTime + each KTHREAD.CycleTime
5. PspUnlockProcessThreadListShared
```

**Locking:** Shared lock on process thread list — concurrent reads allowed, exclusive for thread creation/exit.

### 2.3 Core Cycle Accounting — KiEndThreadCycleAccumulation

This is the heart of cycle time measurement:

```
1. Check KPRCB+0x167C (cycle counting active flag)
2. Call KeQueryPerformanceCounter (QPC)
3. Read KPRCB+0x78A (QPC shift factor)
4. Read KPRCB+0x798 (QPC-to-time multiplier)
5. Compute delta = (QPC_now - KPRCB+0xFC8) >> shift * multiplier
6. Call KiAccumulateTicksFromCycles (if tick accounting enabled)
7. Update KPRCB+0x16C8 (last QPC snapshot)
8. Update KPRCB+0xFC8 (last accumulated cycle time)
9. Add delta to KTHREAD+0x048 (CycleTime)
```

**Accounting chain:**
```
QPC raw counter
  → shift right by KPRCB+0x78A
  → multiply by KPRCB+0x798
  → compute delta from last snapshot
  → accumulate into KTHREAD.CycleTime
```

**Slow path** (different CPU from thread):
1. Raise IRQL to DISPATCH_LEVEL
2. Acquire shared spinlock (`KiCycleAccountingLock`)
3. Same accounting sequence
4. Release spinlock, lower IRQL

### 2.4 Per-Processor Cycle Stats — KeQueryCycleTimeStatsProcessor

**Ultra-fast 12-instruction leaf** — no locks, no branches beyond loop control:

```
nt!KeQueryCycleTimeStatsProcessor:
fffff800`6f458fd0  d2822f08  mov x8, #0x1178    ; offset to stats array
fffff800`6f458fd4  8b080009  add x9, x0, x8     ; x9 = KPRCB+0x1178
fffff800`6f458fd8  52800088  mov w8, #4         ; 4 groups
fffff800`6f458fdc  5280004a  mov w10, #2        ; 2 QWORDs per group
fffff800`6f458fe0  f840852b  ldr x11, [x9], #8  ; copy loop
fffff800`6f458fe4  5100054a  sub w10, w10, #1
fffff800`6f458fe8  f800842b  str x11, [x1], #8
fffff800`6f458fec  35ffffaa  cbnz w10, ...      ; inner loop (2 iterations)
fffff800`6f458ff0  51000508  sub w8, w8, #1     ; outer loop
fffff800`6f458ff4  35ffff48  cbnz w8, ...       ; 4 iterations
fffff800`6f458ff8  d65f03c0  ret
```

Copies 8 QWORDs (64 bytes) from `KPRCB+0x1178` to output buffer. Pure memory copy, zero synchronization.

**KPRCB Cycle Time Stats layout (live data from processor 0):**
```
KPRCB+0x1170  CycleTime:        00000006`039898bb
KPRCB+0x1178  stats[0] (Idle):  00000000`00000000
KPRCB+0x1180  stats[1]:         00000000`00000000
KPRCB+0x1188  stats[2]:         00000000`00000000
KPRCB+0x1190  stats[3]:         00000000`00000000
KPRCB+0x1198  stats[4] (DPC):   00000006`039898bb
KPRCB+0x11A0  stats[5]:         00000000`00000000
KPRCB+0x11A8  stats[6]:         00000000`00000000
KPRCB+0x11B0  stats[7]:         00000000`00000000
```

**KPRCB key offsets summary:**
| Offset | Field | Type | Purpose |
|--------|-------|------|---------|
| +0x048 | (KTHREAD.CycleTime) | Uint8B | Per-thread accumulated cycles |
| +0x78A | QPC shift factor | Uint2B | Shift for QPC→time conversion |
| +0x798 | QPC multiplier | Uint8B | Multiply for QPC→time conversion |
| +0x980 | Current KTHREAD | Ptr | Currently running thread |
| +0x988 | Current EPROCESS | Ptr | Currently running process |
| +0xFC8 | Last accumulated cycle time | Uint8B | Previous snapshot |
| +0x1170 | CycleTime | Uint8B | Per-processor total |
| +0x1178 | CycleTimeStats[8] | Uint8B[8] | 4 groups × 2 (idle/dpc breakdown) |
| +0x16C8 | Last QPC snapshot | Uint8B | For delta computation |
| +0x167C | Cycle counting active | Uint1B | Flag |

---

## 3. GetProcessTimes / GetThreadTimes

### 3.1 GetProcessTimes kernel path

```
GetProcessTimes (kernel32)
  → NtQueryInformationProcess(ProcessTimes)
    → PsQueryTotalCycleTimeProcess (for cycle time data)
    → Reads KPROCESS+0x140 (KernelTime)
    → Reads KPROCESS+0x148 (UserTime)
    → Reads EPROCESS CreateTime
```

**Data flow:**
1. `KernelTime` and `UserTime` are 100ns-precision counters stored in `KPROCESS`
2. Updated during context switch by `KiEndThreadCycleAccumulation` → `KiAccumulateTicksFromCycles`
3. `CreateTime` is set once during `PspAllocateProcess`
4. `ExitTime` is set during `PspExitProcess`

### 3.2 GetThreadTimes kernel path

```
GetThreadTimes (kernel32)
  → NtQueryInformationThread(ThreadTimes)
    → Reads KTHREAD+0x048 (CycleTime) via KeQueryTotalCycleTimeThread
    → Reads KTHREAD.KernelTime
    → Reads KTHREAD.UserTime
    → Reads KTHREAD.CreateTime
```

**Thread time fields mirror process fields** but are per-thread. Updated by the same `KiEndThreadCycleAccumulation` mechanism during every context switch.

---

## 4. PDH API (Performance Data Helper)

### 4.1 Kernel path: NONE

PDH is **entirely user-mode**. No kernel symbols found:

```
kd> x nt!*Pdh*       → (no matches)
kd> x nt!*PerfData*   → (no matches)
```

### 4.2 Data source: Registry

PDH reads from `HKEY_PERFORMANCE_DATA` which is a **virtual registry hive** mapped by the kernel.

**Kernel side:**
- `NtQueryValueKey` on `HKEY_PERFORMANCE_DATA` triggers collection
- Each perf counter provider DLL collects data when queried
- Data is returned as `PERF_DATA_BLOCK` structures

### 4.3 PDH call chain (user-mode only)

```
PdhOpenQuery        → pdh.dll (user-mode)
PdhAddCounter       → pdh.dll → stores counter path
PdhCollectQueryData → pdh.dll → RegQueryValueEx(HKEY_PERFORMANCE_DATA)
                      → NtQueryValueKey → kernel collects counter data
```

**Key insight:** The kernel's only involvement is the registry virtualization. PDH itself has no dedicated kernel API — it piggybacks on the standard registry query path.

---

## 5. NtQuerySystemInformation

Entry: `nt!ExpQuerySystemInformation` at `fffff800`6fb76c18`.

### 5.1 Architecture

Massive function with jump-table dispatch. Two dispatch mechanisms:

1. **Pre-dispatch** (small info classes): Direct `cmp`/`beq` chain for classes 0-0x100
2. **Jump table** at `fffff800`6fb79eac`: Indexed by info class number, 32-bit signed offsets relative to `fffff800`6fb78e38`

**Jump table access:**
```
fffff800`6fb770dc  adr x9, nt!ExpQuerySystemInformation+0x3294  ; table base
fffff800`6fb770e0  ldrsw x8, [x9, w24 uxtw #2]                 ; w24 = info class
fffff800`6fb770e4  adr x9, nt!ExpQuerySystemInformation+0x2220  ; code base
fffff800`6fb770e8  add x8, x9, x8 lsl #2                       ; target address
fffff800`6fb770ec  br x8                                         ; indirect jump
```

### 5.2 SystemProcessorPerformanceInformation (class 8)

**Pre-dispatch:** Info class 8 and 0x17 (23) share the same ProbeForWrite validation path, then both go to the jump table entry at `fffff800`6fb773e4`.

**Handler at +0x7cc:**
```
cmp w24, #8            ; distinguish class 8 vs class 0x17
mov w9, #0x48          ; element size for class 8 (0x48 = 72 bytes)
mov w8, #0x30          ; element size for class 0x17 (0x30 = 48 bytes)
cselne w27, w9, w8     ; select based on class
```

**Loop body (per processor):**
1. `KeGetProcessorIndexFromNumber` — map group+number → processor index
2. Load KPRCB pointer from `KiProcessorBlock` array
3. Call `PoGetIdleTimes` — get idle timing data
4. Multiply KPRCB timing fields by global multiplier (`PpmPolicyConfigTable+0xAA0`)
5. Store results into output buffer:
   - `[x19+0x00]` = IdleTime (from PoGetIdleTimes + KPRCB+0xF80)
   - `[x19+0x08]` = KernelTime (from PoGetIdleTimes)
   - `[x19+0x10]` = UserTime (from PoGetIdleTimes + KPRCB+0xF88)
   - `[x19+0x18]` = DpcTime (from KPRCB+0xF8C)
   - `[x19+0x20]` = InterruptTime (from KPRCB+0xF90)
   - `[x19+0x28]` = InterruptCount (from KPRCB+0xF80, raw count)

**Output structure** (48 bytes for class 0x17, 72 bytes for class 8):
```
Offset  Field              Source
0x00    IdleTime           PoGetIdleTimes.IdleTime
0x08    KernelTime         PoGetIdleTimes.KernelTime
0x10    UserTime           PoGetIdleTimes.UserTime
0x18    DpcTime            KPRCB+0xF8C × multiplier
0x20    InterruptTime      KPRCB+0xF90 × multiplier
0x28    InterruptCount     KPRCB+0xF80 (raw)
0x30-0x47 (class 8 only: extended fields)
```

### 5.3 SystemProcessInformation (class 5)

**Handler:** Calls `ExpGetProcessInformation` at `fffff800`6fb75140`.

```
ExpGetProcessInformation(Buffer, Size, Counter, InfoClass):
  1. Acquires PsActiveProcessHead lock
  2. Walks process list (EPROCESS.ActiveProcessLinks)
  3. For each process:
     - Copies image name, PID, session ID
     - Walks thread list (KPROCESS+0x360)
     - For each thread: copies state, priority, times
  4. Releases lock
```

**Thread data per entry includes:**
- `KERNEL_USER_TIMES` (KernelTime + UserTime + CreateTime + ExitTime)
- Start address
- Priority / base priority
- Thread state
- Wait reason

### 5.4 SystemInterruptInformation (class 23)

Shares the same handler path as class 8 (SystemProcessorPerformanceInformation), but uses **0x30 (48) byte elements** instead of 0x48 (72).

The output structure is the same first 48 bytes — IdleTime, KernelTime, UserTime, DpcTime, InterruptTime, InterruptCount — without the extended fields.

### 5.5 ExpQuerySystemInformation summary

| Info Class | Name | Handler | Key Call |
|-----------|------|---------|----------|
| 2 | SystemPerformanceInformation | `fffff800`6fb77398 | Direct KPRCB reads |
| 5 | SystemProcessInformation | `fffff800`6fb778c0 | `ExpGetProcessInformation` |
| 8 | SystemProcessorPerformanceInformation | `fffff800`6fb773e4 | `PoGetIdleTimes` + KPRCB |
| 0x17 (23) | SystemInterruptInformation | `fffff800`6fb773e4 | Same as class 8 (48-byte mode) |
| 0x9D (157) | (various) | `fffff800`6fb76d24 | ProbeForWrite + group check |

---

## 6. ETW (Event Tracing for Windows)

### 6.1 Kernel Entry Points

Two syscalls form the kernel boundary:

- **`NtTraceControl`** at `fffff800`6fb5f1e0` — Start/Stop/Enable/Query trace sessions
- **`NtTraceEvent`** at `fffff800`6f561700` — Write events to trace sessions

### 6.2 NtTraceControl — Session Management

**Initialization:**
1. Read `EPROCESS+0x252` (container/silo flag)
2. Call `PsGetCurrentServerSiloGlobals` → get silo ETW globals
3. Validate buffer sizes against `0x7FFFFFFF0000` (user-mode buffer limit)
4. Probe user buffers with `ProbeForWrite`

**Dispatch (two-level switch):**

Level 1 — Control code range check:
```
cmp w25, #0x1B           ; max code 27
bhi <fallback>           ; > 27 → bitmap check
mov w8, #0x5000 | 0x80000000  ; bitmap
lsr w8, w8, w25          ; bit test
tbnz w8, #0, <validate>  ; if set, validate buffers
```

Level 2 — Jump table at `fffff800`6fb604c4`:
```
sub w10, w25, #1         ; 0-based index
cmp w10, #0x2E           ; max 46 entries
ldrsw x8, [x9, w10 uxtw #2]  ; load offset
add x8, x9_base, x8 lsl #2   ; compute target
br x8                    ; dispatch
```

**Key dispatch targets:**
| Control Code | Function | Purpose |
|-------------|----------|---------|
| Start (code 1) | `EtwpStartTrace` → `EtwpStartLogger` | Create/start trace session |
| Update (code 2) | `EtwpUpdateTrace` | Modify trace session |
| Flush (code 3) | `EtwpFlushTrace` | Flush buffers to disk |
| Stop (code 4) | `EtwpAcquireLoggerContext` + cleanup | Stop trace session |
| Enable (code 5+) | Various Etwp* functions | Enable/disable providers |

### 6.3 EtwpStartTrace

```
EtwpStartTrace(SiloGlobals, Properties):
  1. Read EPROCESS+0x1DC (logger semaphore count) -1
  2. KeWaitForSingleObject(logger semaphore)     ; serialize session starts
  3. Call EtwpStartLogger(SiloGlobals, Properties)
  4. Return logger status
```

Takes a kernel semaphore to serialize session creation. `EtwpStartLogger` does the actual logger context allocation.

### 6.4 NtTraceEvent — Event Writing

**Entry validation:**
```
and w8, w21, #0xFF00     ; extract flags
cmp w8, #0x300           ; check flag 0x300
bne <reject>             ; must have 0x300 set
```

**Only accepts events with flag 0x300** — kernel-mode traced events.

**User-mode event path:**
1. Check buffer alignment (`tst x20, #3`)
2. Validate buffer against `0x7FFFFFFF0000` limit
3. Read event header fields from user buffer
4. `ObReferenceObjectByHandle` — resolve trace handle to logger object
5. Call `EtwpWriteUserEvent` — the core write function

**EtwpWriteUserEvent** at `fffff800`6fb541e8`:
- Large function (0x5B0 bytes stack frame)
- Handles event filtering, payload validation
- Writes to circular buffer or real-time consumers
- Calls `EtwpGetTimeStampAndQpcDelta` for timing

### 6.5 Kernel-Mode ETW Write

Two exported functions for kernel-mode code:

**`EtwTraceEvent`** at `fffff800`6f560848`:
- Direct kernel event write (no user-mode validation)
- Used by kernel providers (drivers, kernel subsystems)

**`EtwWriteEx`** at `fffff800`6f55e1c0`:
- Extended write with additional filtering support
- Called by kernel-mode ETW providers

### 6.6 ETW Architecture Summary

```
User Mode                          Kernel Mode
────────                          ──────────
StartTrace ─────────────────→ NtTraceControl
                                   ├→ EtwpStartTrace → EtwpStartLogger
                                   ├→ EtwpUpdateTrace
                                   ├→ EtwpFlushTrace
                                   └→ EtwpStopTrace

EnableTraceEx2 ─────────────→ NtTraceControl (control code for enable)
                                   └→ EtwpUpdateRegEntryEnableMask

OpenTrace/ProcessTrace ─────→ (user-mode only, reads buffers)

EtwWrite (user) ────────────→ NtTraceEvent
                                   └→ EtwpWriteUserEvent

EtwWrite (kernel) ──────────→ EtwTraceEvent / EtwWriteEx
                                   └→ EtwpLogSystemEventUnsafe (fast path)
```

**Key ETW internal functions found:**
| Function | Purpose |
|----------|---------|
| `EtwpStartTrace` | Start a new trace session |
| `EtwpStartLogger` | Allocate and initialize logger context |
| `EtwpUpdateTrace` | Modify session properties |
| `EtwpFlushTrace` | Flush buffers to disk |
| `EtwpWriteUserEvent` | Write user-mode event to buffer |
| `EtwpTraceHandle` | Handle-based event write |
| `EtwpGetTimeStampAndQpcDelta` | Timestamp collection |
| `EtwpLogSystemEventUnsafe` | Fast kernel event write (no locks) |
| `EtwpTraceImageUnload` | Image load/unload tracing |
| `EtwpNetProvTraceNetwork` | Network event provider |
| `EtwpCreateLogFile` | Create log file for trace session |
| `EtwpFreeLoggerContext` | Cleanup logger on stop |
| `EtwpDeleteRegistrationObject` | Remove provider registration |
| `EtwTraceSiloKernelEvent` | Silo-aware kernel tracing |

---

## 7. Performance Optimization Implications

### 7.1 Hot Path Rankings (fastest → slowest)

| API | Overhead | Synchronization |
|-----|----------|-----------------|
| KeQueryCycleTimeStatsProcessor | ~12 instructions, no locks | None (per-processor data) |
| KeQueryTotalCycleTimeThread (same CPU) | ~20 instructions | Interrupt disable only |
| GetProcessTimes | Medium | Process lock (shared) |
| GetThreadTimes | Medium | Thread lock (shared) |
| QueryProcessCycleTime | Medium | Shared thread list lock |
| NtQuerySystemInformation(class 8) | Medium | Per-processor loop |
| NtQuerySystemInformation(class 5) | Heavy | Process list walk + per-thread |
| NtTraceControl | Heavy | Semaphore + logger context lock |
| NtTraceEvent | Medium-Heavy | Handle lookup + buffer write |
| PDH (PdhCollectQueryData) | Heavy | Registry → provider DLL load |

### 7.2 Locking Summary

| Resource | Lock Type | Scope |
|----------|-----------|-------|
| KTHREAD.CycleTime | Interrupt disable | Per-thread, same-CPU fast path |
| KTHREAD.CycleTime | Spinlock (DISPATCH) | Cross-CPU slow path |
| KPROCESS thread list | Shared/exclusive lock | Per-process |
| ETW logger semaphore | Kernel semaphore | Global (serializes session start) |
| ETW logger context | Mutex | Per-session |
| Performance registry | None (virtual) | Provider DLL re-entrancy |

### 7.3 Key Findings

1. **Cycle time is the cheapest metric** — 12-instruction leaf for per-processor, interrupt-disable-only for per-thread (same CPU)
2. **Process times require a thread list walk** — shared lock is lightweight but scales with thread count
3. **SystemProcessorPerformanceInformation uses PoGetIdleTimes** — separate from simple KPRCB reads, involves power management subsystem
4. **PDH has zero kernel footprint** — all overhead is user-mode DLL + registry virtualization
5. **ETW is the heaviest** — semaphore serialization for session management, buffer allocation and filtering for event writes
6. **NtOpenProcessToken is trivial** — 3-instruction trampoline, all work in NtOpenProcessTokenEx
