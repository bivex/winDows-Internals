# Windows Memory Management API — Kernel Research Guide

> Цель: классификация WinAPI для управления памятью — какие требуют kernel-level исследования.
> Build: Windows 11 26100 ARM64 | WinDbg kernel debugging

---

## 1. Сводная таблица

| WinAPI | NT Syscall | Kernel Backend | Сложность | Kernel Dev? |
|--------|-----------|----------------|-----------|-------------|
| `VirtualAlloc` / `VirtualAllocEx` | `NtAllocateVirtualMemory` | `MmAllocateVirtualMemory` → VAD insertion | **HIGH** | **Да** |
| `VirtualAlloc` (MEM_LARGE_PAGES) | `NtAllocateVirtualMemory` (SEC_LARGE_PAGES) | `MiLargePagePromote` → `MiLargePageFault` | **VERY HIGH** | **Да** |
| `SetProcessWorkingSetSize` | `NtSetInformationProcess` (ProcessWorkingSetSize=0x1A) | `MmAdjustWorkingSetSizeEx` | **HIGH** | **Да** |
| `SetProcessWorkingSetSizeEx` | `NtSetInformationProcess` (0x1A + flags) | `MmAdjustWorkingSetSizeEx` | **HIGH** | **Да** |
| `EmptyWorkingSet` | `NtSetInformationProcess` (ProcessWorkingSetSize, -1) | `MiEmptyWorkingSet` → `MiEmptyWorkingSetInitiate` | **MEDIUM** | Частично |
| `PrefetchVirtualMemory` | `NtPrefetchVirtualMemory` (undocumented) | `MmPrefetchVirtualMemory` → `MmPrefetchVirtualAddresses` | **MEDIUM** | Частично |
| `OfferVirtualMemory` | `NtOfferVirtualMemory` (syscall stub) | **Нет kernel export** — user-mode only | LOW | Нет |
| `ReclaimVirtualMemory` | `NtReclaimVirtualMemory` (syscall stub) | **Нет kernel export** — user-mode only | LOW | Нет |
| `CreateFileMapping` | `NtCreateSection` | `MmCreateSection` → Section object | **HIGH** | **Да** |
| `MapViewOfFile` | `NtMapViewOfSection` | `MmMapViewOfSection` → VAD + PTE | **VERY HIGH** | **Да** |
| `GetSystemInfo` | `NtQuerySystemInformation` (SystemBasicInformation=0x00) | Trivial query | LOW | Нет |
| `GlobalMemoryStatusEx` | `NtQuerySystemInformation` (SystemPerformanceInformation=0x02) | `MmQuerySystemWorkingSetInformation` | MEDIUM | Частично |
| `NtQuerySystemInformation` (SystemMemoryListInformation) | Direct syscall | `MmQuerySystemWorkingSetInformation` | **MEDIUM** | Частично |
| `NtSetSystemInformation` (MemoryPurgeStandbyList) | Direct syscall | Inlined in `NtSetSystemInformation` | **HIGH** | **Да** |

---

## 2. Детальный разбор по API

### 2.1 VirtualAlloc / VirtualAllocEx

**NT-путь:** `VirtualAllocEx(hProcess, addr, size, MEM_COMMIT|MEM_RESERVE, prot)` → `NtAllocateVirtualMemory(ProcessHandle, &BaseAddress, 0, &RegionSize, AllocationType, Protect)`

**Kernel backend:** `NtAllocateVirtualMemory` @ `fffff800`ef1df070`

```asm
nt!NtAllocateVirtualMemory:                     ; fffff800`ef1df070
  stp   x29, x30, [sp, #-0x60]!
  mov   x29, sp
  stp   x19, x20, [sp, #0x10]
  ; ... validation of ProcessHandle ...
  ; ... access check via ObReferenceObjectByHandle ...
  ; ... check allocation type flags ...
  tst   w25, w8              ; w8 = 0x400000 (SEC_LARGE_PAGES / MEM_LARGE_PAGES)
  b.eq  skip_large_page      ; если не large pages — обычный путь
  ; → MiLargePagePromote для large page allocation
```

**Механизм:**
1. Валидация `ProcessHandle` через `ObReferenceObjectByHandle`
2. Проверка флагов `AllocationType` (MEM_COMMIT=0x1000, MEM_RESERVE=0x2000, MEM_LARGE_PAGES=0x20000000)
3. Для MEM_LARGE_PAGES: проверка привилегии `SeLockMemoryPrivilege`
4. Вызов `MmAllocateVirtualMemory` → создание VAD (Virtual Address Descriptor)
5. Для committed страниц: заполнение PTE (Page Table Entries)

**Ключевые структуры:**
- **VAD** (Virtual Address Descriptor) — описывает регион виртуальной памяти
- **PTE** (Page Table Entry) — маппинг виртуальной → физической страницы
- `EPROCESS+0x400` — указатель на Working Set (косвенно через VadRoot)

---

### 2.2 VirtualAlloc (MEM_LARGE_PAGES) — Large Page Allocation

**Специальный путь для MEM_LARGE_PAGES (0x20000000):**

NT проверяет флаг через `tst w25, w8` где `w8 = 0x400000` (SEC_LARGE_PAGES — внутренний эквивалент).

**Kernel call chain:**
```
NtAllocateVirtualMemory
  → MmAllocateVirtualMemory
    → MiLargePagePromote          ; fffff800`eebfe600
      → MiLargePageFault          ; fffff800`eedf8430
        → MmBuildLargePages       ; fffff800`eefd4608
```

**`MiLargePagePromote`** @ `fffff800`eebfe600`:
- Продвигает обычные страницы в large pages (2MB на ARM64)
- Требует contiguous physical memory
- Использует TLB locking для сохранения маппинга
- SeLockMemoryPrivilege обязательна

**`MiLargePageFault`** @ `fffff800`eedf8430`:
- Обрабатывает page fault для large page region
- Создает large page PTE напрямую (без обычного WSLE)

**Large Pages на ARM64:**
- Размер: 2MB (по сравнению с 4KB standard pages)
- TLB pressure снижается в 512 раз
- Не pageable — остаются в физической памяти
- Требуют `SeLockMemoryPrivilege` (SeLockMemoryPrivilege SID: S-1-5-32-544)

---

### 2.3 SetProcessWorkingSetSize / SetProcessWorkingSetSizeEx

**NT-путь:** `SetProcessWorkingSetSize(hProcess, dwMin, dwMax)` → `NtSetInformationProcess(ProcessHandle, ProcessWorkingSetSize=0x1A, &wsInfo, sizeof(wsInfo))`

**Kernel backend:** `MmAdjustWorkingSetSizeEx` @ `fffff800`eeb96080`

```asm
nt!MmAdjustWorkingSetSizeEx:                    ; fffff800`eeb96080
  stp   x29, x30, [sp, #-0xA0]!
  mov   x29, sp
  ; ... acquire working set locks ...
  ldr   x8, [x20, #0x400]     ; EPROCESS → WorkingSet (via ApcState)
  ; ... validate min/max bounds ...
  ; ... MiTrimWorkingSet if new max < current ...
```

**`MmAdjustWorkingSetSize`** @ `fffff800`eeb96050` — thin wrapper:
```asm
nt!MmAdjustWorkingSetSize:
  mov   x3, #0               ; flags = 0
  b     MmAdjustWorkingSetSizeEx
```

**Механизм:**
1. Acquire Working Set lock (exclusive)
2. Проверка min/max bounds против системных лимитов
3. Если new max < current: вызов `MiTrimWorkingSet` для освобождения страниц
4. Если new min > current: установка growth threshold
5. Обновление полей EPROCESS: WorkingSetMinimum, WorkingSetMaximum

**Working Set access chain:**
```
KTHREAD+0xB0 → EPROCESS → +0x400 → Working Set pointer
```

**Ex-версия** добавляет флаги:
- `QUOTA_LIMITS_HARDWS_MIN_ENABLE` (0x02)
- `QUOTA_LIMITS_HARDWS_MAX_ENABLE` (0x04)
- `QUOTA_LIMITS_WORKING_SET_THERMAL` (0x08)

**Связанные функции:**
- `MiTrimWorkingSet` @ `fffff800`eebc6f48` — физическое удаление страниц из WS
- `MiWorkingSetManager` @ `fffff800`eebc7c20` — системный поток, периодически обрезает WS

---

### 2.4 EmptyWorkingSet

**NT-путь:** `EmptyWorkingSet(hProcess)` → `SetProcessWorkingSetSize(hProcess, -1, -1)` → `NtSetInformationProcess(0x1A, {Min=-1, Max=-1})`

**Kernel backend:** `MiEmptyWorkingSet` @ `fffff800`eedd79d8` — **3 инструкции:**

```asm
nt!MiEmptyWorkingSet:                            ; fffff800`eedd79d8
  mov   x3, #-1              ; trim ALL pages (trimLimit = -1)
  mov   x2, #0               ; flags = 0
  b     MiEmptyWorkingSetInitiate
```

**`MiEmptyWorkingSetInitiate`** @ `fffff800`eeb8fe68` — сложная функция:
```asm
nt!MiEmptyWorkingSetInitiate:                     ; fffff800`eeb8fe68
  stp   x29, x30, [sp, #-0x70]!
  ; ... acquire MiLockWorkingSetShared ...
  ; ... iterate WSLE (Working Set List Entries) ...
  ; ... for each entry: remove from WS, update PTE to transition ...
  ; ... release WS lock ...
```

**Механизм:**
1. `MiEmptyWorkingSet` устанавливает trimLimit = -1 (trim all) и прыгает в initiate
2. `MiEmptyWorkingSetInitiate` захватывает Working Set lock (shared)
3. Итерирует по WSLE (Working Set List Entries)
4. Каждую страницу переводит в transition state (PTE modification)
5. Освобождает WS lock
6. Страницы перемещаются в standby/modified list

**Связанные функции:**
- `MmEmptyAllWorkingSets` @ `fffff800`eede9360` — итерирует по partition'ам через `PsGetNextPartition`
- WSLE — Working Set List Entry (индексированный массив в Working Set)

---

### 2.5 PrefetchVirtualMemory

**NT-путь:** `PrefetchVirtualMemory(hProcess, count, &addresses, flags)` → `NtPrefetchVirtualMemory`

**Kernel backend:** `MmPrefetchVirtualMemory` @ `fffff800`ef1ec120` → `MmPrefetchVirtualAddresses` @ `fffff800`ef1ec050`

```asm
nt!MmPrefetchVirtualAddresses:                    ; fffff800`ef1ec050
  stp   x29, x30, [sp, #-0x50]!
  ; ... validate version (must be 1) ...
  ldr   w8, [x1, #0x00]       ; version check (w8 == 1)
  cmp   w8, #1
  b.ne  reject
  ; ... priority validation ...
  bl    MiGetEffectivePagePriorityThread
  ; ... compute effective page priority ...
  ; → MiPrefetchVirtualMemory (internal)
```

**`MiPrefetchVirtualMemory`** @ `fffff800`eebc00f0` — внутренняя реализация:
- Обрабатывает массив `WIN32_MEMORY_RANGE_ENTRY`
- Для каждого диапазона: проверяет VAD, инициирует prefetch
- Страницы помещаются в standby list с заданным priority

**`VmpPrefetchVirtualAddresses`** @ `fffff800`eedab070` — внутренний helper для виртуального адресного пространства.

**Структура `WIN32_MEMORY_RANGE_ENTRY`:**
```c
typedef struct _WIN32_MEMORY_RANGE_ENTRY {
    PVOID VirtualAddress;    // начало диапазона
    SIZE_T NumberOfBytes;    // размер
} WIN32_MEMORY_RANGE_ENTRY;
```

**Механизм:**
1. Валидация version (должна быть 1)
2. Вычисление effective page priority через `MiGetEffectivePagePriorityThread`
3. Для каждого диапазона: lookup VAD
4. Prefetch страниц → placement в standby list
5. Не блокирует вызывающий поток (асинхронный I/O hint)

---

### 2.6 OfferVirtualMemory / ReclaimVirtualMemory

**ВАЖНО:** `NtOfferVirtualMemory` и `NtReclaimVirtualMemory` **НЕ являются kernel exports**.

Это user-mode only syscall stubs:
- Нет соответствующих функций в `ntoskrnl.exe`
- Обработчики живут в kernel dispatcher table, но не экспортируются
- Реализация: валидация в syscall entry → работа с VAD directly

**OfferVirtualMemory** помечает память как "discardable":
- `VmOfferedDecommit` (0) — можно полностью освободить
- `VmOfferedPagedOut` (1) — page out но не release
- `VmOfferedPriorityDecommit` (2) — priority-based discard

**ReclaimVirtualMemory** восстанавливает предложенную память:
- Проверяет что регион был ранее offered
- Заново резервирует/коммитит страницы

**Вывод:** Для kernel research эти функции не представляют интереса — вся логика user-mode/внутренняя.

---

### 2.7 CreateFileMapping / MapViewOfFile (Shared Memory)

**NT-путь:**
- `CreateFileMapping` → `NtCreateSection`
- `MapViewOfFile` → `NtMapViewOfSection`

**Kernel backend:**

```
CreateFileMapping
  → NtCreateSection @ fffff800`ef1e5320
    → MmCreateSection @ fffff800`ef1e4cf0
      → Section object creation (size, protection, pagefile/file backing)

MapViewOfFile
  → NtMapViewOfSection @ fffff800`ef1e9d20
    → MmMapViewOfSection @ fffff800`ef1e9810
      → VAD insertion + PTE setup
```

**`NtCreateSection`** @ `fffff800`ef1e5320`:
```asm
nt!NtCreateSection:                               ; fffff800`ef1e5320
  stp   x29, x30, [sp, #-0x60]!
  ; ... validate DesiredAccess ...
  ; ... validate object attributes ...
  ; ... call MmCreateSection ...
```

**`NtMapViewOfSection`** @ `fffff800`ef1e9d20`:
```asm
nt!NtMapViewOfSection:                             ; fffff800`ef1e9d20
  stp   x29, x30, [sp, #-0x70]!
  ; ... validate SectionInheritDisposition ...
  ; ... Wow64 handling (32-bit compat) ...
  ; ... validate commit size vs section size ...
  bl    MmMapViewOfSection
```

**Section Object** — ключевой kernel объект:
- `_SECTION` — описывает shared memory region
- Может быть backed by: pagefile (anonymous), file (mapped file), или image
- Содержит: размер, protection, reference count, prototype PTEs

**Механизм MapViewOfFile:**
1. `NtMapViewOfSection` валидирует параметры
2. `MmMapViewOfSection` находит свободный регион в VA space
3. Создает VAD типа `VadLargePage`, `VadImageMap`, или `VadWriteWatch`
4. Настраивает prototype PTEs (shared между всеми mappings)
5. Page fault при первом доступе → `MiResolveProtoPteFault`

**Performance-critical аспекты:**
- Section object — единственная(kernel) абстракция для shared memory
- Prototype PTEs позволяют share physical pages между процессами
- Large page sections (SEC_LARGE_PAGES) — 2MB alignment
- Copy-on-Write через `SECTION_MAP_COPY` access flag

---

### 2.8 GetSystemInfo / GlobalMemoryStatusEx

**`GetSystemInfo`:**
- NT-путь: `NtQuerySystemInformation(SystemBasicInformation=0x00)`
- Возвращает: `SYSTEM_BASIC_INFORMATION` — PageSize, NumberOfProcessors, etc.
- Kernel: trivial query из global variables
- **Сложность: LOW** — не требует kernel research

**`GlobalMemoryStatusEx`:**
- NT-путь: `NtQuerySystemInformation(SystemPerformanceInformation=0x02)`
- Возвращает: `MEMORYSTATUSEX` — total/available physical/virtual memory
- Kernel: `MmQuerySystemWorkingSetInformation` @ `fffff800`eeb96600`
- **Сложность: MEDIUM** — aggregate query из multiple counters

---

### 2.9 NtQuerySystemInformation (SystemMemoryListInformation)

**NT-путь:** `NtQuerySystemInformation(SystemMemoryListInformation=0x50, ...)`

**Kernel backend:** `MmQuerySystemWorkingSetInformation` @ `fffff800`eeb96600`

Возвращает детальную информацию о памяти:
- Standby list sizes (по priority levels)
- Modified page count
- Free page count
- Zero page count
- Working set sizes (system/process)

**Структура `SYSTEM_MEMORY_LIST_INFORMATION`:**
```c
typedef struct _SYSTEM_MEMORY_LIST_INFORMATION {
    ULONG   ZeroPageCount;          // страниц в zero list
    ULONG   FreePageCount;          // свободных страниц
    ULONG   ModifiedPageCount;      // modified pages
    ULONG   ModifiedNoWritePageCount;
    ULONG   BadPageCount;
    ULONG   PriorityZeroPageCount;  // standby priority 0
    ULONG   PriorityOnePageCount;   // standby priority 1
    // ... priority 2-7 ...
    ULONG   RepurposedPageCount;
    ULONG   StandbyPageCount;       // total standby
    // ...
} SYSTEM_MEMORY_LIST_INFORMATION;
```

---

### 2.10 NtSetSystemInformation (MemoryPurgeStandbyList)

**NT-путь:** `NtSetSystemInformation(SystemMemoryPurgeStandbyList=0x49, NULL, 0)`

**Kernel backend:** `NtSetSystemInformation` @ `fffff800`eef77e10` — гигантский switch:

```asm
nt!NtSetSystemInformation:                         ; fffff800`eef77e10
  ; ... validate information class ...
  ; ... privilege check (SeSystemProfilePrivilege or SeDebugPrivilege) ...
  ; ... giant switch on information class ...
  ; class 0x49 (MemoryPurgeStandbyList):
  ;   → purge standby list pages
  ; class 0x59: ...
  ; class 0x97: ...
  ; class 0xCC: ...
  ; class 0xD2: ...
  ; class 0xD4: ...
```

**Information Classes для memory management:**

| Class | Имя | Описание |
|-------|-----|----------|
| 0x49 | `SystemMemoryPurgeStandbyList` | Очистка standby list |
| 0x50 | `SystemMemoryListInformation` | Query memory lists |
| 0x59 | `SystemRangeStartInformation` | System VA range |
| 0x97 | `SystemSingleModuleInfo` | Single module information |
| 0xCC | (undocumented) | Memory list control |
| 0xD2 | (undocumented) | Memory priority |
| 0xD4 | (undocumented) | Page priority |

**Привилегии:** Требуется `SeSystemProfilePrivilege` или `SeDebugPrivilege` для большинства memory information classes.

**Механизм purge standby:**
1. Проверка привилегий
2. Итерация по standby list (priority 0 → 7)
3. Перемещение страниц из standby → free list
4. Обновление счетчиков доступной памяти

---

## 3. Ключевые структуры и смещения

### Working Set Structure
```
EPROCESS
  ├── +0x400  → Working Set pointer (через ApcState/KTHREAD chain)
  ├── +0x8E0  → WorkingSet-related fields (from PspRefreshProcessUserPresencePpmPolicyCallback analysis)
  │
  ├── WorkingSetMinimum (DWORD) — минимальный размер WS
  ├── WorkingSetMaximum (DWORD) — максимальный размер WS
  ├── WorkingSetPeak (DWORD)    — пиковый размер WS
  └── WorkingSetSize  (DWORD)   — текущий размер WS
```

### Access Chain (Working Set)
```
KTHREAD+0xB0  → EPROCESS pointer
  EPROCESS+0x400 → Working Set structure
    Working Set → WSLE array (Working Set List Entries)
    Working Set → WS lock (MiLockWorkingSetExclusive/Shared)
```

### VAD (Virtual Address Descriptor)
```
VAD (недокументированная структура)
  ├── StartingVpn     — стартовый Virtual Page Number
  ├── EndingVpn       — конечный Virtual Page Number
  ├── Protection      — PAGE_READWRITE, etc.
  ├── VadType         — VadLargePage, VadImageMap, VadWriteWatch
  ├── CommitCharge    — committed страниц
  └── LeftChild / RightChild — BST pointers (VadRoot)
```

### Section Object
```
_SECTION
  ├── SizeOfSection (LARGE_INTEGER)
  ├── Segment → _SEGMENT
  │     ├── PrototypePtes → array of prototype PTEs
  │     ├── NumberOfCommittedPages
  │     └── ImageBase (для image sections)
  ├── ControlArea → _CONTROL_AREA
  │     ├── FilePointer (для file-backed)
  │     ├── NumberOfSectionReferences
  │     └── NumberOfMappedViews
  └── Flags (SEC_COMMIT, SEC_RESERVE, SEC_LARGE_PAGES, etc.)
```

---

## 4. Адреса функций (Build 26100 ARM64)

| Функция | Адрес |
|---------|-------|
| `NtAllocateVirtualMemory` | `fffff800`ef1df070` |
| `MmAdjustWorkingSetSize` | `fffff800`eeb96050` |
| `MmAdjustWorkingSetSizeEx` | `fffff800`eeb96080` |
| `MiEmptyWorkingSet` | `fffff800`eedd79d8` |
| `MiEmptyWorkingSetInitiate` | `fffff800`eeb8fe68` |
| `MmEmptyAllWorkingSets` | `fffff800`eede9360` |
| `MmPrefetchVirtualMemory` | `fffff800`ef1ec120` |
| `MmPrefetchVirtualAddresses` | `fffff800`ef1ec050` |
| `MiPrefetchVirtualMemory` | `fffff800`eebc00f0` |
| `VmpPrefetchVirtualAddresses` | `fffff800`eedab070` |
| `NtMapViewOfSection` | `fffff800`ef1e9d20` |
| `NtCreateSection` | `fffff800`ef1e5320` |
| `MmCreateSection` | `fffff800`ef1e4cf0` |
| `MmMapViewOfSection` | `fffff800`ef1e9810` |
| `NtSetSystemInformation` | `fffff800`eef77e10` |
| `NtQuerySystemInformation` | `fffff800`ef17ac70` |
| `MiLargePagePromote` | `fffff800`eebfe600` |
| `MiLargePageFault` | `fffff800`eedf8430` |
| `MmBuildLargePages` | `fffff800`eefd4608` |
| `MmQuerySystemWorkingSetInformation` | `fffff800`eeb96600` |
| `MiTrimWorkingSet` | `fffff800`eebc6f48` |
| `MiWorkingSetManager` | `fffff800`eebc7c20` |

---

## 5. Рекомендации для kernel-разработчика

#### Высший приоритет:

1. **NtAllocateVirtualMemory + Large Pages** — самый сложный путь:
   - VAD creation и insertion в VadRoot (red-black tree)
   - Large page promotion (`MiLargePagePromote`) — contiguous physical memory allocation
   - `MiLargePageFault` — special fault handling
   - `MmBuildLargePages` — TLB management
   - Влияние SeLockMemoryPrivilege на процесс allocation

2. **NtMapViewOfSection / MmMapViewOfSection** — shared memory internals:
   - Prototype PTEs и CoW (Copy-on-Write) механизм
   - Section object lifecycle (create → map → unmap → close)
   - Cross-process address space synchronization
   - Image section vs pagefile section vs file-backed section

3. **MmAdjustWorkingSetSizeEx** — working set management:
   - WSLE (Working Set List Entry) iteration
   - MiTrimWorkingSet — page eviction policy
   - Working Set lock hierarchy (MiLockWorkingSetExclusive vs Shared)
   - Interaction с MiWorkingSetManager (system thread)

#### Средний приоритет:

4. **MiEmptyWorkingSetInitiate** — WS flushing:
   - WSLE → transition PTE conversion
   - Standby/Modified list insertion
   - Async vs sync flush behavior

5. **MmPrefetchVirtualAddresses** — prefetch internals:
   - `MiGetEffectivePagePriorityThread` — priority computation
   - Standby list priority placement
   - Interaction с SuperFetch/SysMain

6. **NtSetSystemInformation** (MemoryPurgeStandbyList):
   - Giant switch statement analysis (0x49, 0xCC, 0xD2, 0xD4)
   - Privilege checks per information class
   - Standby list purge → free list transition

#### Низкий приоритет (не требует kernel research):

7. **GetSystemInfo** — trivial query из globals
8. **GlobalMemoryStatusEx** — aggregate counter query
9. **OfferVirtualMemory / ReclaimVirtualMemory** — user-mode only, нет kernel exports

---

## 6. Call Chain Summary (ASCII)

```
┌──────────────────────────────────────────────────────────────────────┐
│                        USER MODE                                     │
├──────────────────────────────────────────────────────────────────────┤
│ VirtualAllocEx ──► NtAllocateVirtualMemory                           │
│     ├──► MmAllocateVirtualMemory → VAD insertion                     │
│     └──► (MEM_LARGE_PAGES) MiLargePagePromote                        │
│            └──► MiLargePageFault → MmBuildLargePages                 │
│                                                                      │
│ SetProcessWorkingSetSize ──► NtSetInformationProcess(0x1A)           │
│     └──► MmAdjustWorkingSetSize → MmAdjustWorkingSetSizeEx          │
│            └──► MiTrimWorkingSet [if shrinking]                      │
│                                                                      │
│ EmptyWorkingSet ──► SetProcessWorkingSetSize(-1,-1)                  │
│     └──► NtSetInformationProcess(0x1A)                               │
│          └──► MiEmptyWorkingSet (3 instr: trimAll=-1)                │
│               └──► MiEmptyWorkingSetInitiate [WSLE iteration]        │
│                                                                      │
│ PrefetchVirtualMemory ──► NtPrefetchVirtualMemory                    │
│     └──► MmPrefetchVirtualMemory                                     │
│          └──► MmPrefetchVirtualAddresses [version+priority check]    │
│               └──► MiPrefetchVirtualMemory [standby placement]       │
│                                                                      │
│ CreateFileMapping ──► NtCreateSection                                │
│     └──► MmCreateSection → Section object                            │
│                                                                      │
│ MapViewOfFile ──► NtMapViewOfSection                                 │
│     └──► MmMapViewOfSection → VAD + PrototypePTEs                    │
│                                                                      │
│ NtSetSystemInformation(PurgeStandby) ──► inlined purge               │
│     └──► Standby[0..7] → Free list transition                       │
│                                                                      │
│ OfferVirtualMemory ──► NtOfferVirtualMemory [user-mode syscall]      │
│ ReclaimVirtualMemory ──► NtReclaimVirtualMemory [user-mode syscall]  │
│     ⚠ No kernel exports — internal syscall handling only             │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 7. Quick Reference — что поручить kernel-разработчику

| Задача | API | Что исследовать |
|--------|-----|-----------------|
| Large page allocation | `MiLargePagePromote` | Contiguous memory, TLB locking, SeLockMemoryPrivilege |
| Shared memory internals | `MmMapViewOfSection` | Prototype PTEs, CoW, cross-process sync |
| Working set management | `MmAdjustWorkingSetSizeEx` | WSLE iteration, trim policy, lock hierarchy |
| Working set flush | `MiEmptyWorkingSetInitiate` | WSLE → transition PTE, standby insertion |
| Memory prefetch | `MmPrefetchVirtualAddresses` | Priority computation, standby placement |
| Standby list purge | `NtSetSystemInformation(0x49)` | Privilege checks, list manipulation |
| VAD tree management | `MmAllocateVirtualMemory` | VadRoot insertion, VAD lifecycle |

**Не требует kernel research:** GetSystemInfo, GlobalMemoryStatusEx, OfferVirtualMemory, ReclaimVirtualMemory

---

## 8. Примечания по инфраструктуре

**Окружение WinDbg:**
- Debugger VM: `10.211.55.5` (Windows 11 Pro ARM64)
- Target VM: `10.211.55.6` (Windows 11 ARM64, Build 26100)
- MCP Agent: `http://10.211.55.5:44444/mcp` (SSE transport)
- socat bridge: UNIX socket → TCP relay для serial debugging
- Требуется `iphlpsvc` (IP Helper Service) для netsh portproxy

**Архитектурные особенности ARM64:**
- Large page size: 2MB (standard: 4KB)
- Все адреса в дизассемблированном коде — ARM64 instructions
- Регистры: x0-x30 (64-bit), w0-w30 (32-bit), sp, x29=fp, x30=lr
- Атомарные операции: SWPA (Store Word Pair Atomic), CAS (Compare-And-Swap)
