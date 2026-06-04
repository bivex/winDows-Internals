# Windows Timer API — Kernel Research Guide

> Цель: классификация WinAPI таймеров — какие требуют kernel-level исследования для оптимизации.
> Build: Windows 11 26100 ARM64 | WinDbg kernel debugging

---

## 1. Сводная таблица

| WinAPI | NT Syscall / Kernel Func | Kernel Backend | Сложность | Kernel Dev? |
|--------|--------------------------|----------------|-----------|-------------|
| `timeBeginPeriod` / `timeEndPeriod` | `NtSetTimerResolution` | `ExpUpdateTimerResolution` + atomic ops on EPROCESS | **HIGH** | **Да** |
| `NtQueryTimerResolution` | Direct syscall | Read from `PpmPolicyConfigTable+0xAA0` globals | LOW | Нет |
| `CreateWaitableTimerEx` | `NtCreateTimerEx` | Object manager → KTIMER init | **MEDIUM** | Частично |
| `SetWaitableTimer` / `SetWaitableTimerEx` | `NtSetTimerEx` | `KeSetTimerEx` → `KiSetTimerEx` → timer wheel insertion | **VERY HIGH** | **Да** |
| `QueryPerformanceCounter` | `KeQueryPerformanceCounter` | HAL timer table → hardware counter read | **VERY HIGH** | **Да** |
| `QueryPerformanceFrequency` | Same func (dual return) | `HAL_TIMER_TABLE+0xC0` frequency field | LOW | Нет |
| `QueryInterruptTime` | `KeQueryInterruptTime` | Shared tick counter (exported) | LOW | Нет |
| `QueryInterruptTimePrecise` | `KeQueryInterruptTimePrecise` | `ldapr` + `dmb` barrier read | LOW | Нет |
| `QueryUnbiasedInterruptTime` | `KeQueryUnbiasedInterruptTime` | Seqlock read pattern (~10 instructions) | **MEDIUM** | Частично |
| `Sleep` / `SleepEx` | `NtDelayExecution` | KTIMER + wait | **MEDIUM** | Частично |

---

## 2. KTIMER Structure (ARM64)

Core kernel timer object used by all timer APIs:

```
dt nt!_KTIMER
  +0x000 Header              : _DISPATCHER_HEADER
  +0x018 DueTime             : _ULARGE_INTEGER       ← absolute expiration time (100ns units)
  +0x020 TimerListEntry      : _LIST_ENTRY            ← entry in timer wheel bucket
  +0x030 Dpc                 : Ptr64 _KDPC            ← deferred procedure call on expiry
  +0x038 Processor           : Uint2B                 ← target processor index
  +0x03a TimerType           : Uint2B                 ← timer type classification
  +0x03c Period              : Uint4B                 ← periodic interval (0 = one-shot)
```

### DISPATCHER_HEADER (timer-relevant bits)

```
+0x000 Type                   : UChar          ← 0x0E = Timer type
+0x001 Absolute               : Pos 0, 1 Bit   ← absolute vs relative time
+0x001 Wake                   : Pos 1, 1 Bit   ← wake system from sleep
+0x001 EncodedTolerableDelay  : Pos 2, 6 Bits  ← coalescing tolerance
+0x003 Inserted               : Pos 6, 1 Bit   ← timer is in timer wheel
+0x003 Expired                : Pos 7, 1 Bit   ← timer has fired
+0x004 SignalState            : Int4B           ← signaled state (for waitable timers)
+0x008 WaitListHead           : _LIST_ENTRY     ← waiters list
```

> **EncodedTolerableDelay** (bits 2-7 of byte +0x001): 6-bit value used for timer coalescing.
> Allows the kernel to delay timer expiry by up to this amount to batch multiple timers together.

---

## 3. NtSetTimerResolution — Timer Resolution Control

**WinAPI path:** `timeBeginPeriod(ms)` / `timeEndPeriod(ms)` → `NtSetTimerResolution`

### 3.1 Disassembly

```asm
nt!NtSetTimerResolution @ fffff800`6fb7b560
  ; Parameters:
  ;   w0 = DesiredResolution (100ns units)
  ;   w1 = SetResolution (0=clear, 1=set)
  ;   x2 = *CurrentResolution (output)

  mov   w23, w0                    ; DesiredResolution
  uxtb  w22, w1                    ; SetResolution flag (byte-extended)
  mov   x24, x2                    ; CurrentResolution output ptr
  bl    ExAcquireTimeRefreshLockExclusive

  ldr   w21, [x20]                 ; read current resolution value

  cbnz  w22, set_path              ; if SetResolution=1 → set new resolution

  ; === CLEAR PATH (timeEndPeriod) ===
  ; Atomic clear of resolution bits in EPROCESS+0x1E4
  mov   w8, #0x1000
  ldclral w8, w8, [x9]             ; atomic clear bits (exclusive)

  ; Decrement ExpTimerResolutionCount at:
  ; PopPowerAggregatorTargetStateContexts+0xCDC
  ldr   w8, [x10, #0xCDC]
  sub   w8, w8, #1
  str   w8, [x10, #0xCDC]

  ; ETW tracing
  bl    PoTraceSystemSystemTimerResolution

  ; Reset to system default
  mov   x0, #0
  mov   x1, #0
  mov   x2, #0
  bl    ExpUpdateTimerResolution
  b     done

  ; === SET PATH (timeBeginPeriod) ===
set_path:
  ; Atomic set resolution bits in EPROCESS+0x1E4
  mov   w8, w23                    ; desired value
  ldsetal w8, w8, [x9]             ; atomic OR bits (exclusive)

  ; Increment ExpTimerResolutionCount
  ldr   w8, [x10, #0xCDC]
  add   w8, w8, #1
  str   w8, [x10, #0xCDC]

  ; ETW tracing
  bl    PoTraceSystemSystemTimerResolution

  ; Apply new resolution
  mov   x0, x23                    ; DesiredResolution
  mov   x1, #0
  mov   x2, #0
  bl    ExpUpdateTimerResolution

done:
  ; Output current resolution
  ldr   w8, [x20]
  str   w8, [x24]

  bl    ExReleaseTimeRefreshLock
  ret
```

### 3.2 Key Structures & Offsets

| Offset | Field | Description |
|--------|-------|-------------|
| `EPROCESS+0x1E4` | Timer resolution bits | Per-process resolution request (atomic ldclr/ldset) |
| `PopPowerAggregatorTargetStateContexts+0xCDC` | `ExpTimerResolutionCount` | Global count of active resolution requests |
| `PpmPolicyConfigTable+0xB4C` | Timer resolution setting | Current effective resolution |

### 3.3 Mechanism

1. Acquire `ExAcquireTimeRefreshLockExclusive` — global lock for timer resolution changes
2. If **setting**: atomic `ldsetal` (OR) on `EPROCESS+0x1E4` — per-process resolution bits
3. If **clearing**: atomic `ldclral` (AND-NOT) on `EPROCESS+0x1E4`
4. Update `ExpTimerResolutionCount` — tracks how many processes requested resolution changes
5. ETW trace via `PoTraceSystemSystemTimerResolution`
6. Call `ExpUpdateTimerResolution` — applies the effective resolution system-wide
7. Output current resolution to user buffer

### 3.4 ExpUpdateTimerResolution — Core Resolution Update

```asm
nt!ExpUpdateTimerResolution @ fffff800`6f56adc0
  ; Raises IRQL to DISPATCH_LEVEL
  mov   w0, #2
  bl    KfRaiseIrql

  ; Acquires spin lock via SWPA (swap atomic)
  ; Checks PpmPolicyConfigTable+0xB04 for timer coalescing enable flag
  ; Updates hardware timer interval
  ; Calls KiExitDispatcher
```

> **Вывод:** **Требует kernel-разработчика.** Сложная синхронизация: exclusive lock, atomic operations on EPROCESS, global counter management, IRQL manipulation, interaction with power manager (PpmPolicyConfigTable), ETW tracing. `timeBeginPeriod` is NOT a simple register write — it involves process-level state, global counter, and hardware timer reconfiguration.

---

## 4. NtQueryTimerResolution — Read Timer Resolution

**WinAPI path:** Direct syscall (no WinAPI wrapper, use `NtQueryTimerResolution` directly)

### 4.1 Disassembly

```asm
nt!NtQueryTimerResolution @ fffff800`6fb7b4b0
  ; Probe user pointers (3 outputs)
  ; x0 = *MaximumResolution, x1 = *MinimumResolution, x2 = *CurrentResolution

  ; Read from timer resolution globals at PpmPolicyConfigTable+0xAA0:
  adrp  x9, PpmPolicyConfigTable
  add   x9, x9, #0xAA0

  ldr   w8, [x9]            ; [+0xAA0] → MaxResolution
  str   w8, [x0]

  ldr   w8, [x9, #0x98]     ; [+0xB38] → MinResolution
  str   w8, [x1]

  ldr   w8, [x9, #0xE0]     ; [+0xB80] → CurrentResolution
  str   w8, [x2]

  ret
```

### 4.2 Timer Resolution Globals

| Address (relative to PpmPolicyConfigTable) | Value | Description |
|---------------------------------------------|-------|-------------|
| `+0xAA0` | MaxResolution | Maximum timer resolution (coarsest, largest interval in 100ns) |
| `+0xB38` | MinResolution | Minimum timer resolution (finest, smallest interval in 100ns) |
| `+0xB80` | CurrentResolution | Currently active timer resolution |

> **Вывод:** **Не требует kernel-разработчика.** Простое чтение 3 глобальных переменных. User-mode может вызывать напрямую.

---

## 5. KeSetTimerEx → KiSetTimerEx — Timer Wheel Insertion

**WinAPI path:** `SetWaitableTimer` / `SetWaitableTimerEx` → `NtSetTimerEx` → `KeSetTimerEx` → `KiSetTimerEx`

### 5.1 KeSetTimerEx (thin wrapper)

```asm
nt!KeSetTimerEx @ fffff800`6f46b0d0
  mov   x4, x3              ; DPC parameter
  mov   w3, #0              ; period = 0 (one-shot)
  bl    KiSetTimerEx         ; actual implementation
```

### 5.2 KiSetTimerEx — Timer Wheel Mechanics

The timer wheel is a hash-table of timer lists indexed by DueTime. Key operations:

1. **Calculate timer wheel bucket:** DueTime is hashed to determine which bucket list to insert into
2. **Insert into sorted list:** Timers within a bucket are sorted by DueTime ascending
3. **Set Inserted flag:** `DISPATCHER_HEADER+0x003 bit 6`
4. **Configure coalescing:** `EncodedTolerableDelay` in header byte +0x001

```
Timer Wheel Structure:
┌──────────────────────────────────┐
│ KiTimerTableListHead[]           │  ← array of LIST_ENTRY
│ [0] → timer → timer → ...       │
│ [1] → timer → ...               │
│ [2] → timer → timer → timer → . │
│ ...                              │
│ [N] → (empty)                    │
└──────────────────────────────────┘

Insertion: hash(DueTime) → bucket → sorted insert by DueTime
```

### 5.3 Key Fields

| Structure | Offset | Field | Purpose |
|-----------|--------|-------|---------|
| KTIMER | +0x018 | DueTime | Absolute expiration (100ns units since boot) |
| KTIMER | +0x020 | TimerListEntry | Linked list entry in timer wheel bucket |
| KTIMER | +0x030 | Dpc | Callback to execute on expiry |
| KTIMER | +0x038 | Processor | Target CPU for the DPC |
| KTIMER | +0x03c | Period | Periodic interval (0 = one-shot) |
| DISPATCHER_HEADER | +0x001 bit 2-7 | EncodedTolerableDelay | Coalescing tolerance |
| DISPATCHER_HEADER | +0x003 bit 6 | Inserted | Timer is in wheel |
| DISPATCHER_HEADER | +0x003 bit 7 | Expired | Timer has fired |

> **Вывод:** **Требует kernel-разработчика.** Timer wheel insertion involves hash-based bucket selection, sorted list insertion, coalescing delay encoding, cross-processor DPC scheduling. The timer coalescing mechanism (EncodedTolerableDelay) is especially relevant for optimization — it allows batching timer expirations.

---

## 6. KeCancelTimer

```asm
nt!KeCancelTimer @ fffff800`6f46a4f0
  ; Check if timer is Inserted
  ldrb  w8, [x0, #0x03]        ; DISPATCHER_HEADER+0x003
  tbz   w8, #6, not_inserted    ; bit 6 = Inserted

  ; Remove from timer wheel list
  ldr   x8, [x0, #0x20]        ; TimerListEntry.Flink
  ldr   x9, [x0, #0x28]        ; TimerListEntry.Blink
  str   x9, [x8]               ; Flink->Blink = Blink
  str   x8, [x9, #8]           ; Blink->Flink = Flink

  ; Clear Inserted flag, set Expired
  ; Return TRUE (was inserted)
not_inserted:
  ; Return FALSE (was not active)
```

> Standard doubly-linked list removal. Clears `Inserted` flag, signals any waiters.

---

## 7. QueryPerformanceCounter — High-Resolution Timer

**WinAPI path:** `QueryPerformanceCounter` → `KeQueryPerformanceCounter`

### 7.1 Disassembly

```asm
nt!KeQueryPerformanceCounter @ fffff800`6f404c50
  ; Load HAL timer table pointer
  ldr   x19, [HalpAllocationDescriptorStaticArray+0xC58]  ; timer table

  ; Read timer type
  ldr   w8, [x19, #0xE4]     ; timer type field
  cmp   w8, #5                ; type 5 = special processing path
  beq   special_type_path

  ; Read counter size
  ldr   w8, [x19, #0xDC]     ; counter size field
  cmp   w8, #0x40             ; 64-bit counter?

  ; Load frequency (for QueryPerformanceFrequency — same function)
  ldr   x20, [x19, #0xC0]    ; frequency value

  ; Read base counter value
  ldr   x0, [x19, #0x48]     ; base counter value

  ; Call hardware-specific read function
  ldr   x8, [x19, #0x70]     ; read function pointer
  blr   x8                    ; call it → returns delta in x0

  ; Compute current value
  add   x0, x21, x0           ; current = base + delta
  ret
```

### 7.2 HAL Timer Table Layout

| Offset | Field | Description |
|--------|-------|-------------|
| `+0x48` | BaseCounter | Base value (subtracted from hardware counter) |
| `+0x70` | ReadFunction | Pointer to hardware-specific counter read function |
| `+0xC0` | Frequency | Ticks per second (returned by QPF) |
| `+0xDC` | CounterSize | Counter width (0x40 = 64-bit) |
| `+0xE4` | TimerType | Timer hardware type (5 = special path) |

### 7.3 Mechanism

1. Load HAL timer table from global pointer (`HalpAllocationDescriptorStaticArray+0xC58`)
2. Check timer type — different hardware has different read paths
3. Read base counter value and hardware-specific read function pointer
4. Call read function → get delta from hardware
5. Return base + delta = current performance counter value
6. **Frequency** is stored at `+0xC0` — same function returns it via second output parameter

> **Вывод:** **Требует kernel-разработчика.** Complex multi-path function depending on HAL timer type. Reads hardware counter through function pointer table. Understanding timer type dispatch and counter read mechanisms is essential for optimization (e.g., choosing the right counter for benchmarking).

---

## 8. KeQueryUnbiasedInterruptTime — Tick Counter (without sleep bias)

**WinAPI path:** `QueryUnbiasedInterruptTime` → `KeQueryUnbiasedInterruptTime`

### 8.1 Disassembly

```asm
nt!KeQueryUnbiasedInterruptTime @ fffff800`6f459180
  ; Seqlock pattern — lock-free read with consistency check
retry:
  ldr   x8, [ptr1]             ; → InterruptTime structure base
  ldr   x12, [ptr2]            ; → UnbiasedTime value
  ldr   x9, [x8]               ; read sequence number 1
  dmb   ish                     ; memory barrier (inner shareable)
  ldr   x10, [x8]              ; read sequence number 2
  cmp   x9, x10                ; consistency check
  bne   retry                   ; if changed → retry (writer was active)

  sub   x0, x12, x9            ; result = UnbiasedTime - bias
  ret
```

### 8.2 Mechanism

This is a **seqlock pattern** — one of the most elegant lock-free read techniques in the kernel:

1. Read `UnbiasedTime` value
2. Read sequence number before and after
3. If sequence numbers differ → a writer was updating → retry
4. If sequence numbers match → consistent read → subtract bias

> `dmb ish` (Data Memory Barrier, Inner Shareable) ensures the second sequence read happens after the value read.

### 8.3 Why "Unbiased"?

`KeQueryInterruptTime` includes time spent in sleep/hibernate. `KeQueryUnbiasedInterruptTime` subtracts the "bias" — time the system spent in low-power states. Useful for measuring actual active time.

> **Вывод:** **Частично требует kernel-разработчика.** The function itself is simple (~10 instructions), but understanding the seqlock pattern and when to prefer unbiased vs biased interrupt time is important for optimization.

---

## 9. KeQueryInterruptTimePrecise — Precise Tick Counter

```asm
nt!KeQueryInterruptTimePrecise
  ; Uses ldp (load pair) for atomic 128-bit read
  ; Then dmb ish barrier
  ldr   x8, [InterruptTimePtr]
  ldr   x9, [x8]               ; read current tick count
  dmb   ish
  ; Additional precision via hardware counter refinement
```

> More precise than `KeQueryInterruptTime` because it refines the tick count using the hardware performance counter, interpolating between timer interrupts.

---

## 10. Timer Resolution Globals

| Global Symbol | Value (observed) | Description |
|---------------|-------------------|-------------|
| `KeTimeIncrement` | `0x43F8` (17396) | Default clock tick in 100ns units (~1.74ms) |
| `PpmPolicyConfigTable+0xAA0` | — | MaxResolution |
| `PpmPolicyConfigTable+0xB38` | — | MinResolution |
| `PpmPolicyConfigTable+0xB80` | — | CurrentResolution |
| `PpmPolicyConfigTable+0xB04` | — | Timer coalescing enable flag |
| `PopPowerAggregatorTargetStateContexts+0xCDC` | 0 (idle) | ExpTimerResolutionCount |

### KeTimeIncrement Interpretation

`0x43F8` = 17,396 × 100ns = 1,739,600ns ≈ **1.74ms**

This is the default system timer resolution on this ARM64 platform. When `timeBeginPeriod(1)` is called, the resolution changes to ~1ms (10,000 × 100ns).

---

## 11. Timer Coalescing

Timer coalescing is a power-saving feature that groups nearby timer expirations:

```
Without coalescing:          With coalescing:
  T1 ─┐                        T1,T2,T3 ─┐
  T2 ─┼─ separate               merged    ├─ single
  T3 ─┘  wakeups                          ┘  wakeup
```

### Implementation

- **EncodedTolerableDelay** in `DISPATCHER_HEADER+0x001` bits 2-7 (6 bits = 0-63)
- Controlled by `PpmPolicyConfigTable+0xB04` flag
- `ExpUpdateTimerResolution` checks this flag when applying resolution
- When enabled, timers within the tolerable delay window are batched together

### Relevance for Optimization

- Timer coalescing **reduces wakeups** → better power efficiency
- But increases **latency** for individual timer expiry
- For real-time/low-latency applications: coalescing should be disabled or minimized
- `SetWaitableTimerEx` allows specifying `TolerableDelay` per-timer
- `timeBeginPeriod(1)` effectively reduces coalescing window

---

## 12. CreateWaitableTimerEx

**NT path:** `CreateWaitableTimerEx` → `NtCreateTimerEx`

`NtCreateTimerEx` is NOT an exported symbol (not resolvable via `u nt!NtCreateTimerEx`). It creates a timer object via the Object Manager and initializes a KTIMER structure.

**Timer object attributes:**
- Can be manual-reset or auto-reset (synchronization timer vs notification timer)
- Supports `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` flag (Windows 10 1803+)
- High-resolution timers bypass the timer wheel and use a more precise mechanism

> **Вывод:** **Частично требует kernel-разработчика.** Object creation is standard Object Manager path, but high-resolution timer mode requires understanding of the underlying hardware timer programming.

---

## 13. Практические рекомендации для оптимизации

### 13.1 Timer Resolution

| Операция | API | Влияние |
|----------|-----|---------|
| Увеличить точность сна до 1ms | `timeBeginPeriod(1)` | Увеличивает `ExpTimerResolutionCount`, перенастраивает аппаратный таймер |
| Вернуть стандартную точность | `timeEndPeriod(1)` | Decrement counter, reset resolution если counter=0 |
| Узнать текущую точность | `NtQueryTimerResolution` | Чтение 3 глобальных переменных |

### 13.2 Для system optimizer — приоритет исследования

1. **HIGH PRIORITY:** `NtSetTimerResolution` — `timeBeginPeriod` используется многими приложениями для увеличения точности таймера. Optimizer должен управлять resolution-aware процессами.
2. **HIGH PRIORITY:** Timer coalescing —直接影响 power/performance баланс. `EncodedTolerableDelay` — уникальный per-timer knob.
3. **MEDIUM:** `KeQueryPerformanceCounter` — understanding HAL timer type dispatch для правильного выбора benchmarking counter.
4. **LOW:** `NtQueryTimerResolution` — простое чтение глобальных переменных, не требует kernel research.

### 13.3 Аппаратные зависимости

На ARM64 платформе (этот стенд):
- Default tick: ~1.74ms (KeTimeIncrement = 0x43F8)
- HAL timer table через `HalpAllocationDescriptorStaticArray+0xC58`
- Counter read через function pointer в timer table (+0x70)
- Timer wheel: hash(DueTime) → bucket → sorted list

---

## 14. Lessons Learned (WinDbg Research)

| # | Observation | Implication |
|---|------------|-------------|
| 1 | `NtSetTimerResolution` uses `ExAcquireTimeRefreshLockExclusive` | Global lock — concurrent resolution changes are serialized |
| 2 | Per-process resolution bits in `EPROCESS+0x1E4` (atomic ldclr/ldset) | Each process contributes independently to global resolution |
| 3 | `ExpTimerResolutionCount` tracks active requests | System resets resolution only when ALL processes have cleared |
| 4 | `KeQueryUnbiasedInterruptTime` uses seqlock (no lock acquisition) | Extremely fast read — suitable for hot paths |
| 5 | `KeQueryPerformanceCounter` dispatches by timer type | Different ARM64 SoCs may have different timer hardware |
| 6 | `NtCreateTimerEx` is not exported | Symbol not resolvable — only accessible via private symbols |
| 7 | Timer coalescing uses 6-bit encoded delay in DISPATCHER_HEADER | Maximum coalescing window = 63 units (interpretation depends on encoding) |
| 8 | `KiSetTimerEx` uses hash-based timer wheel | O(1) average insertion, sorted within bucket |
