# Windows Optimization API — Kernel Research Guide

> Цель: классификация WinAPI для оптимизации процессов/потоков — какие требуют kernel-level исследования.
> Build: Windows 11 26100 ARM64 | WinDbg kernel debugging

---

## 1. Сводная таблица

| WinAPI | NT Syscall | Kernel Backend | Сложность | Kernel Dev? |
|--------|-----------|----------------|-----------|-------------|
| `SetPriorityClass` | `NtSetInformationProcess` (ProcessPriorityClass=0x0C) | `PsSetProcessPriorityClass` | LOW | Нет |
| `SetThreadPriority` | `NtSetInformationThread` (ThreadPriority=0x02) | `KeSetBasePriorityThread` | **HIGH** | **Да** |
| `SetProcessAffinityMask` | `NtSetInformationProcess` (ProcessAffinityMask=0x04) | `KeSetAffinityProcess` | **MEDIUM** | **Да** |
| `SetThreadAffinityMask` | `NtSetInformationThread` (ThreadAffinityMask=0x04) | `KeSetSystemAffinityThread` → `KeSetSystemGroupAffinityThread` | **HIGH** | **Да** |
| `SetThreadIdealProcessor` | `NtSetInformationThread` (ThreadIdealProcessor=0x05) | `KeSetIdealProcessorThread` → `KeSetIdealProcessorThreadByNumber` | **HIGH** | **Да** |
| `SetThreadIdealProcessorEx` | `NtSetInformationThread` (ThreadIdealProcessorEx=0x2C) | `KeSetIdealProcessorThread` → `KeSetIdealProcessorThreadByNumber` | **HIGH** | **Да** |
| `NtSetInformationProcess` (ProcessIoPriority=0x29) | Direct syscall | Inlined in `NtSetInformationProcess` | **HIGH** | **Да** |
| `NtSetInformationThread` (ThreadPriority=0x02) | Direct syscall | `KeSetBasePriorityThread` | **HIGH** | **Да** |
| `SetProcessInformation` (ProcessMemoryPriority) | `NtSetInformationProcess` (0x60) | `MmSetMemoryPriorityProcess` | LOW | Нет |
| `SetProcessInformation` (ProcessPowerThrottling) | `NtSetInformationProcess` | `PspTrySetProcessPebThrottlingFlags` | MEDIUM | Частично |
| `SetThreadInformation` (ThreadMemoryPriority) | `NtSetInformationThread` (0x30) | Inlined | LOW | Нет |
| `SetThreadInformation` (ThreadPowerThrottling) | `NtSetInformationThread` | Inlined | MEDIUM | Частично |

---

## 2. Детальный разбор по API

### 2.1 SetPriorityClass / GetPriorityClass

**NT-путь:** `SetPriorityClass` → `NtSetInformationProcess(ProcessHandle, ProcessPriorityClass=0x0C, ...)`

**Kernel backend:** `PsSetProcessPriorityClass` — **2 инструкции:**

```asm
nt!PsSetProcessPriorityClass:
  strb  w1, [x0, #0x337]    ; EPROCESS.PriorityClass = value
  ret
```

**Структура:** `EPROCESS+0x337` (UChar) — поле `PriorityClass`

**Вывод:** Простейшая операция — запись 1 байта. **Не требует kernel-разработчика.** User-mode разработчик может вызывать напрямую через WinAPI.

---

### 2.2 SetThreadPriority / GetThreadPriority

**NT-путь:** `SetThreadPriority` → `NtSetInformationThread(ThreadHandle, ThreadPriority=0x02, &priority, 4)`

**Kernel backend:** `KeSetBasePriorityThread` — **сложная функция:**

```asm
nt!KeSetBasePriorityThread:           ; fffff800`eea65dc0
  ldr   x22, [x20, #0x240]           ; KTHREAD.ApcStateFill (Process)
  adrp  x8, PspHostSiloGlobals+0x80
  add   x8, x8, #0x580
  cmp   x22, x8                      ; проверка на System process
  KfRaiseIrql(DISPATCH_LEVEL)        ; повышение IRQL
  swpa  x23, x8, [x19]               ; ThreadLock (KTHREAD+0x40)
  ldrsb w8, [x22, #0x98]             ; KPROCESS.BasePriority
  ldrsb w27, [x20, #0x253]           ; KTHREAD.BasePriority (текущий)
  sub   w23, w27, w8                  ; delta = thread_base - process_base
  ldrsb w8, [x20, #0x2A5]            ; KTHREAD.PriorityDecrement (+0x2A5)
  ; ... вычисление нового приоритета с bounds checking ...
  strb  w8, [x20, #0x2A5]            ; запись PriorityDecrement
```

**Ключевые структуры и смещения:**
- `KTHREAD+0x0BB` — `Priority` (Char) — текущий приоритет потока
- `KTHREAD+0x253` — `BasePriority` (Char) — базовый приоритет
- `KTHREAD+0x2A5` — `PriorityDecrement` (Byte) — декремент приоритета
- `KTHREAD+0x240` — `ApcStateFill` → `KPROCESS` указатель
- `KPROCESS+0x098` — `BasePriority` (Char) — базовый приоритет процесса
- `KTHREAD+0x040` — `ThreadLock` (SWPA для синхронизации)

**Механизм:**
1. IRQL поднимается до `DISPATCH_LEVEL`
2. Acquire `ThreadLock` через atomic SWPA
3. Считывается `KPROCESS.BasePriority` (базовый приоритет процесса)
4. Вычисляется delta = thread_base - process_base
5. Новый приоритет clamped к диапазону [-15, +15] относительно process base
6. Записывается `PriorityDecrement` для negative boost
7. Release lock → `KiExitDispatcher`

**Вывод:** **Требует kernel-разработчика.** Сложная арифметика приоритетов, IRQL management, ThreadLock synchronization, взаимодействие с scheduler через `KiExitDispatcher`.

---

### 2.3 SetProcessAffinityMask

**NT-путь:** `SetProcessAffinityMask` → `NtSetInformationProcess(ProcessHandle, ProcessAffinityMask=0x04, &mask, 4)`

**Kernel backend:** `KeSetAffinityProcess` — ~0x130 байт, итерирует все потоки процесса:

```asm
nt!KeSetAffinityProcess:              ; fffff800`eed03510
  ; sub sp, #0x130 — большой stack frame для KAFFINITY_EX
  ; инициализация KAFFINITY_EX (256 байт, zeroed)
  ; копирование affinity mask в KAFFINITY_EX
  ; итерация по потокам процесса, вызов KeSetAffinityThread для каждого
```

**Структура:** `KPROCESS+0x058` — `Affinity` (Ptr64 `_KAFFINITY_EX`)

**Вывод:** **Требует kernel-разработчика.** Работает с `KAFFINITY_EX` (group-aware affinity), итерирует все потоки процесса, вызывает `KeSetAffinityThread`/`KiSetLegacyAffinityThread` для каждого потока.

---

### 2.4 SetThreadAffinityMask

**NT-путь:** `SetThreadAffinityMask` → `NtSetInformationThread(ThreadHandle, ThreadAffinityMask=0x04, &mask, 4)`

**Kernel backend:**
```
KeSetSystemAffinityThread → KeSetSystemAffinityThreadEx → KeSetSystemGroupAffinityThread
KeSetAffinityThread → KiSetLegacyAffinityThread
```

**Ключевые функции:**
- `KeSetSystemAffinityThread` (fffff800`eed06720) — строит `_GROUP_AFFINITY`, вызывает `KeSetSystemGroupAffinityThread`
- `KiSetLegacyAffinityThread` (fffff800`eea67e00) — legacy path для non-group affinity
- `KeSetSystemGroupAffinityThread` (fffff800`eea664f0) — основная реализация

**Структура:** `KTHREAD+0x260` — `Affinity` (Ptr64 `_KAFFINITY_EX`)

**Вывод:** **Требует kernel-разработчика.** Group-aware affinity через `KAFFINITY_EX`, взаимодействие с scheduler, изменение dispatcher database.

---

### 2.5 SetThreadIdealProcessor / SetThreadIdealProcessorEx

**NT-путь:**
- `SetThreadIdealProcessor` → `NtSetInformationThread(ThreadHandle, ThreadIdealProcessor=0x05, &proc, 4)`
- `SetThreadIdealProcessorEx` → `NtSetInformationThread(ThreadHandle, ThreadIdealProcessorEx=0x2C, &proc_ex, 0x10)`

**Kernel backend:** `KeSetIdealProcessorThread` → `KeSetIdealProcessorThreadByNumber`

```asm
nt!KeSetIdealProcessorThread:         ; fffff800`eed0d8e0
  ldr   x8, [xpr, #0x988]            ; текущий KTHREAD
  cmp   x0, x8                       ; если current thread
  ldrh  w8, [x0, #0x268]             ;   KTHREAD+0x268 (IdealProcessor для текущего)
  ldrh  w8, [x0, #0x250]             ;   иначе KTHREAD+0x250 (IdealProcessorecb)
  strb  w1, [sp, #0x12]              ; номер процессора
  bl    KeSetIdealProcessorThreadByNumber  ; основная логика
```

**Ключевые смещения:**
- `KTHREAD+0x26C` — `IdealProcessor` (Uint4B) — идеальный процессор
- `KTHREAD+0x250` — `IdealProcessorecb` — для non-current threads
- `KTHREAD+0x268` — для current thread

**Вывод:** **Требует kernel-разработчика.** `KeSetIdealProcessorThreadByNumber` работает с scheduler internals, NUMA topology, может вызвать rebalancing.

---

### 2.6 NtSetInformationProcess — ProcessIoPriority (0x29)

**Direct syscall.** Обработчик inlined в `NtSetInformationProcess`:

```asm
; NtSetInformationProcess, ProcessInformationClass == 0x29 (ProcessIoPriority)
cmp   w24, #0x29                     ; проверка класса
beq   handler_0x29
; handler_0x29:
movi  v16, #0                         ; zero context
stp   q16, q16, [x26, #0x2D0]        ; инициализация
str   d16, [x26, #0x2F0]
cmn   x22, #1                         ; Handle == -1 (CurrentProcess)?
mov   x22, #0                         ; обнуление handle
cmp   w20, #0x28                      ; BufferLength == 0x28?
cbnz  w19, wow64_path
ldr   w3, [x21]                       ; IO priority value
cmp   w3, #0x40                       ; bounds check (max IoPriority)
bls   valid_priority
```

**Структура:** `EPROCESS+0x1E4` (bits 27-29) — `DefaultIoPriority` (3 Bits)

Установка IO priority для процесса — сложная логика с проверкой привилегий, job limits, propagation на все потоки процесса через `KeAbProcessBaseIoPriorityChangeInternal` / `KeAbProcessEffectiveIoPriorityChange`.

**Вывод:** **Требует kernel-разработчика.** Сложная валидация, ETW трейсинг, propagation на потоки, interaction с Activity Broker (KeAb*).

---

### 2.7 NtSetInformationThread — ThreadPriority (0x02)

Тот же путь что и `SetThreadPriority` — через `KeSetBasePriorityThread`. См. раздел 2.2.

**Вывод:** **Требует kernel-разработчика.**

---

### 2.8 SetProcessInformation — ProcessMemoryPriority

**NT-путь:** `SetProcessInformation(ProcessMemoryPriority)` → `NtSetInformationProcess(ProcessHandle, 0x60, &MEMORY_PRIORITY_INFORMATION, 0x10)`

**Kernel backend:** `MmSetMemoryPriorityProcess` — простая функция:

```asm
nt!MmSetMemoryPriorityProcess:        ; fffff800`eeb8ac58
  adrp  x8, WheapIpmiLogEntry+0x30f0
  add   x20, x8, #0x980              ; spinlock address
  mov   x19, #0xB680                 ; spinlock offset
  add   x0, x20, x19
  ExAcquireSpinLockExclusive
  strb  w21, [x22, #0x4BA]           ; EPROCESS+0x4BA = MemoryPriority
  MiReleaseSpinLockExclusive
  ret
```

**Структура:** `EPROCESS+0x4BA` (UChar) — `MemoryPriority`

Значения: 0=Lowest, 1=VeryLow, 2=Low, 3=Medium, 4=BelowNormal, 5=Normal

**Вывод:** **Не требует kernel-разработчика.** Простая запись 1 байта под spinlock'ом. User-mode вызов достаточен.

---

### 2.9 SetProcessInformation — ProcessPowerThrottling

**NT-путь:** обработчик внутри `NtSetInformationProcess`

**Kernel backend:** `PspTrySetProcessPebThrottlingFlags`

```asm
nt!PspTrySetProcessPebThrottlingFlags: ; fffff800`eef1e370
  KiStackAttachProcess                ; аттачится к address space процесса
  ldr   x9, [x19, #0x2D0]            ; EPROCESS+0x2D0 (PEB pointer)
  ldr   x8, [x19, #0x300]            ; EPROCESS+0x300 (Wow64 PEB)
  add   x10, x9, #0x50               ; PEB+0x50 (ThrottlingFlags)
  ldsetal w9, w8, [x10]              ; atomic set в PEB
  ; аналогично для Wow64 PEB
  KiStackDetachProcess
```

**Механизм:** Модифицирует `PEB+0x50` (ThrottlingFlags) — user-mode память процесса. Также устанавливает kernel-side flags через `ldsetal` (interlocked).

**Вывод:** **Частично требует kernel-разработчика.** Сама запись в PEB простая, но нужно понимать какие throttling state machines затрагиваются (EcoQoS, PowerThrottling). Для базового использования достаточно WinAPI.

---

### 2.10 SetThreadInformation — ThreadMemoryPriority

**NT-путь:** обработчик внутри `NtSetInformationThread` (class 0x30)

```asm
; NtSetInformationThread, class == 0x30 (ThreadMemoryPriority)
cmp   w20, #0x10                      ; BufferLength == 0x10?
bne   error
cmn   x22, #2                         ; Handle validation
ldr   q16, [x19]                      ; load MEMORY_PRIORITY_INFORMATION
str   q16, [x26, #0x130]             ; save to local
```

Затем вызывается inline-логика для установки memory priority потока — аналогично процессу, но через ETHREAD структуру.

**Вывод:** **Не требует kernel-разработчика.** Аналогично ProcessMemoryPriority — простая установка приоритета.

---

### 2.11 SetThreadInformation — ThreadPowerThrottling

**NT-путь:** обработчик внутри `NtSetInformationThread`

**Вывод:** **Частично требует kernel-разработчика.** Аналогично ProcessPowerThrottling.

---

## 3. Ключевые структуры и смещения

### EPROCESS (релевантные поля)

| Offset | Size | Field | Описание |
|--------|------|-------|----------|
| +0x000 | | Pcb (KPROCESS) | Embedded KPROCESS |
| +0x1E4 | 3b | DefaultIoPriority | IO приоритет процесса (bits 27-29) |
| +0x337 | 1b | PriorityClass | Класс приоритета процесса |
| +0x4BA | 1b | MemoryPriority | Приоритет памяти |

### KPROCESS (релевантные поля)

| Offset | Size | Field | Описание |
|--------|------|-------|----------|
| +0x058 | 8 | Affinity | Ptr64 KAFFINITY_EX |
| +0x098 | 1b | BasePriority | Базовый приоритет процесса |

### KTHREAD (релевантные поля)

| Offset | Size | Field | Описание |
|--------|------|-------|----------|
| +0x040 | 8 | ThreadLock | Spin lock для синхронизации (SWPA) |
| +0x0BB | 1b | Priority | Текущий приоритет потока |
| +0x240 | 8 | ApcStateFill | → KPROCESS pointer |
| +0x253 | 1b | BasePriority | Базовый приоритет потока |
| +0x260 | 8 | Affinity | Ptr64 KAFFINITY_EX |
| +0x2A5 | 1b | PriorityDecrement | Декремент приоритета |
| +0x26C | 4 | IdealProcessor | Идеальный процессор |

---

## 4. Ключевые функции — адреса (Build 26100 ARM64)

| Функция | Адрес | Размер | Сложность |
|---------|-------|--------|-----------|
| `NtSetInformationProcess` | fffff800`ef0e1e80 | ~0x1900 | HIGH (switch по 0x71 классам) |
| `NtSetInformationThread` | fffff800`ef0e37a0 | ~0x1200 | HIGH (switch по 0x38 классам) |
| `PsSetProcessPriorityClass` | fffff800`eed398f0 | 8 bytes | LOW (1 strb) |
| `KeSetBasePriorityThread` | fffff800`eea65dc0 | ~0x290 | HIGH (IRTL + ThreadLock + scheduler) |
| `KeSetAffinityProcess` | fffff800`eed03510 | ~0x298 | MEDIUM (итерирует потоки) |
| `KeSetAffinityThread` | fffff800`eed066f0 | → jmp | → KiSetLegacyAffinityThread |
| `KiSetLegacyAffinityThread` | fffff800`eea67e00 | — | HIGH |
| `KeSetSystemAffinityThread` | fffff800`eed06710 | → jmp | → KeSetSystemAffinityThreadEx |
| `KeSetSystemAffinityThreadEx` | fffff800`eed06720 | ~0x60 | MEDIUM |
| `KeSetSystemGroupAffinityThread` | fffff800`eea664f0 | — | HIGH |
| `KeSetIdealProcessorThread` | fffff800`eed0d8e0 | ~0x50 | MEDIUM (wrapper) |
| `KeSetIdealProcessorThreadByNumber` | fffff800`eea7a348 | — | HIGH |
| `MmSetMemoryPriorityProcess` | fffff800`eeb8ac58 | ~0x50 | LOW (spinlock + strb) |
| `PsSetIoPriorityThread` | fffff800`eeaf2560 | ~0x60 | MEDIUM (CAS + ETW) |
| `PspTrySetProcessPebThrottlingFlags` | fffff800`eef1e370 | ~0xA0 | MEDIUM (PEB attach) |

---

## 5. Рекомендации для kernel-разработчика

### Приоритет исследования (от высокого к низкому):

#### Критический приоритет (обязательно kernel research):

1. **KeSetBasePriorityThread** — ядро системы приоритетов. Нужно понять:
   - Формулу вычисления нового приоритета (delta + base + decrement)
   - Как `KiExitDispatcher` применяет изменения к scheduler
   - Bounds checking (clamping к [-15, +15])
   - Взаимодействие с priority boost механизмом

2. **KeSetSystemGroupAffinityThread** — групповая аффинность:
   - Как `KAFFINITY_EX` маппится на NUMA nodes
   - Scheduler rebalancing при смене affinity
   - Как system affinity отличается от user affinity
   - Interaction с `KeSetIdealProcessorThreadByNumber`

3. **KeSetIdealProcessorThreadByNumber** — ideal processor:
   - NUMA-aware selection
   - Rebalancing timer callback (`KiInitializeIdealProcessorRebalancer`)
   - Влияние на scheduler decisions

4. **IoPriority propagation** (внутри NtSetInformationProcess class 0x29):
   - Как `KeAbProcessBaseIoPriorityChangeInternal` распространяет IO priority
   - ETW events при изменении
   - Job hierarchy interaction

#### Средний приоритет:

5. **KeSetAffinityProcess** — понять как итерация по потокам работает:
   - Thread list traversal
   - Per-thread affinity update
   - Race conditions при concurrent thread creation

6. **PspTrySetProcessPebThrottlingFlags** — power throttling:
   - Какие bits в PEB+0x50 за что отвечают
   - Как kernel-side throttling state machine реагирует

#### Низкий приоритет (не требует kernel research):

7. **PsSetProcessPriorityClass** — 2 инструкции, тривиально
8. **MmSetMemoryPriorityProcess** — 1 spinlock + 1 byte write
9. **PsSetIoPriorityThread** — CAS (compare-and-swap) + ETW, но прямолинейно

---

## 6. Call Chain Summary (ASCII)

```
┌─────────────────────────────────────────────────────────────┐
│                    USER MODE                                 │
├─────────────────────────────────────────────────────────────┤
│ SetPriorityClass ──► NtSetInformationProcess(0x0C)          │
│     └──► PsSetProcessPriorityClass  [EPROCESS+0x337 = val]  │
│                                                              │
│ SetThreadPriority ──► NtSetInformationThread(0x02)           │
│     └──► KeSetBasePriorityThread  [KTHREAD priority math]   │
│                                                              │
│ SetProcessAffinityMask ─► NtSetInformationProcess(0x04)      │
│     └──► KeSetAffinityProcess  [iterate threads]            │
│                                                              │
│ SetThreadAffinityMask ─► NtSetInformationThread(0x04)        │
│     └──► KeSetSystemAffinityThreadEx                        │
│          └──► KeSetSystemGroupAffinityThread                │
│                                                              │
│ SetThreadIdealProcessor ─► NtSetInformationThread(0x05)      │
│     └──► KeSetIdealProcessorThread                          │
│          └──► KeSetIdealProcessorThreadByNumber             │
│                                                              │
│ SetProcessInformation(MemPriority) ─► NtSetInformationProc   │
│     └──► MmSetMemoryPriorityProcess [EPROCESS+0x4BA]       │
│                                                              │
│ SetProcessInformation(PowerThrot) ─► NtSetInformationProc    │
│     └──► PspTrySetProcessPebThrottlingFlags [PEB+0x50]      │
└─────────────────────────────────────────────────────────────┘
```

---

## 7. Quick Reference — что поручить kernel-разработчику

| Задача | API | Что исследовать |
|--------|-----|-----------------|
| Priority boost/deboost | `KeSetBasePriorityThread` | Формула приоритетов, ThreadLock, KiExitDispatcher |
| CPU affinity (groups) | `KeSetSystemGroupAffinityThread` | KAFFINITY_EX, NUMA, scheduler rebalance |
| Ideal processor | `KeSetIdealProcessorThreadByNumber` | NUMA topology, rebalancing timer |
| IO Priority | NtSetInformationProcess(0x29) | KeAb* propagation, ETW, job limits |
| Process affinity iter | `KeSetAffinityProcess` | Thread enumeration, race conditions |
| Power throttling | `PspTrySetProcessPebThrottlingFlags` | PEB bits, kernel state machine |

**Не требует kernel research:** SetPriorityClass, SetProcessInformation(MemoryPriority), SetThreadInformation(MemoryPriority)
