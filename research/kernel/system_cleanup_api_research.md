# System Cleanup / SuperFetch / Prefetch APIs — Kernel Research

> WinDbg ARM64 disassembly | Windows 11 24H2 | ntoskrnl

---

## 1. NtSetSystemInformation — Memory List Commands

### 1.1 Entry & Dispatch

`NtSetSystemInformation` (`fffff800`6f977e10`) uses a jump-table dispatch for info class `SystemMemoryListInformation` (0x50 = 80).

**Jump-table mechanism** (at +0xF4):
```asm
; w10 = infoClass - 9
cmp   w10, #0xED                   ; max index = 237 (class 246)
bhi   <invalid_class>
adr   x9, <jump_table>             ; fffff800`6f979558
ldrsw x8, [x9, w10 uxtw #2]       ; load signed 32-bit offset
adr   x9, <code_base>              ; fffff800`6f978db4
add   x8, x9, x8 lsl #2           ; handler = base + offset*4
br    x8                           ; jump to handler
```

**SystemMemoryListInformation** (class 0x50, index 0x47):
- Jump table entry (raw bytes `5c ff ff ff`): offset = -0xA4 (signed 32-bit)
- Handler validates `InputBufferLength >= 4`, reads command DWORD from user buffer
- Calls `MmIssueMemoryListCommand(command, partition_object)`

### 1.2 MmIssueMemoryListCommand (`fffff800`6f9c10a8`)

```asm
; Command validation & privilege check
cmp   w19, #5                      ; command range: 0-5
bhi   <invalid>                    ; >5 → STATUS_INVALID_INFO_CLASS

cmp   w19, #3                      ; command 3 skips privilege check
beq   <skip_check>

ldr   x0, [x8+0xB70]              ; SeProfileSingleProcessPrivilege LUID
bl    SeSinglePrivilegeCheck
cbnz  w8, <proceed>                ; privileged → continue
; else → STATUS_ACCESS_DENIED

<proceed>:
; Convert partition object to partition via MiPartitionObjectToPartition
bl    MmPerformMemoryListCommand   ; actual work
```

**Key:** Commands 0-2 and 4-5 require `SeProfileSingleProcessPrivilege`. Command 3 skips the privilege check entirely.

### 1.3 MmPerformMemoryListCommand (`fffff800`6fca5d18`)

Complete command dispatch:

| Command | Action | Kernel Function |
|---------|--------|-----------------|
| 0 | Capture working set access bits (skip) | `MiCaptureAllWorkingSetAccessBits(partition, 0)` |
| 1 | Capture working set access bits (include) | `MiCaptureAllWorkingSetAccessBits(partition, 1)` |
| 2 | Empty all working sets | `MiEmptyAllWorkingSets(partition)` |
| 3 | Flush all modified pages | `MiFlushAllPages(partition, 0, 8)` |
| 4 | Purge standby list (low priority) | `MiPurgePartitionStandby(partition, 8)` |
| 5 | Purge standby list (all priorities) | `MiPurgePartitionStandby(partition, 1)` |
| >5 | Invalid | STATUS_INVALID_INFO_CLASS (0xC0000003) |

**Command-to-meaning mapping** (from SDK `SYSTEM_MEMORY_LIST_COMMAND` enum):
- 0 = `MemoryCaptureAccessBits` — snapshot WS access bits, don't reset
- 1 = `MemoryCaptureAndResetAccessBits` — snapshot + reset WS access bits
- 2 = `MemoryEmptyWorkingSets` — flush all working sets
- 3 = `MemoryFlushModifiedList` — write back all modified pages (no privilege needed!)
- 4 = `MemoryPurgeLowPriorityStandbyList` — purge low-priority standby only
- 5 = `MemoryPurgeStandbyList` — purge entire standby list

### 1.4 Deep Dive: MiPurgePartitionStandby (`fffff800`6f60bb40`)

```asm
mov   w0, #2                       ; IRQL = DISPATCH_LEVEL
bl    KfRaiseIrql                  ; raise IRQL for page list manipulation
mov   w2, #0x200                   ; PFN flags mask
bl    MiRemoveLowestPriorityStandbyPage
; Loop through PFN entries (0x30 stride = sizeof(MMPFN))
```

Raises IRQL to DISPATCH_LEVEL to safely manipulate the page frame database. Iterates standby list pages and removes them.

### 1.5 MiEmptyAllWorkingSets (`fffff800`6f7e8890`)

```asm
; Checks global flag at fffff800`6fe3e980+0xB78C
ldrb  w8, [x9, x8]                ; read working set management flag
cbz   w8, <skip>                   ; if disabled, skip
ldr   x10, [x0, #0x4600]          ; partition → WS request queue
add   w8, w8, #1                  ; increment counter
str   w8, [x10, #0x1C]
bl    MiQueueWorkingSetRequest     ; async WS flush via queue
```

Uses an asynchronous work queue mechanism rather than doing a synchronous flush. Queues a working set request that processes in a system worker thread.

### 1.6 MiFlushAllPages (`fffff800`6f7eb688`)

```asm
mov   w0, #1                      ; parameter
uxtbf w19, w1                     ; priority level from caller
bl    KiQueryUnbiasedInterruptTime ; get current time for timeout
; Uses modified page writer to flush dirty pages
```

Queries unbiased interrupt time and initiates a flush of all modified (dirty) pages through the modified page writer subsystem.

### 1.7 Optimization Implications

| Operation | Cost | Risk | Optimization Use |
|-----------|------|------|-----------------|
| CaptureAccessBits | Low | None | Profiling page access patterns |
| EmptyWorkingSets | High | Performance degradation | Force memory release, anti-forensics |
| FlushModifiedList | Medium | I/O spike | Ensure data written to disk |
| PurgeStandbyList | Medium | Cache loss | Free physical memory |
| PurgeLowPriorityStandby | Low-Medium | Minor cache loss | Selective cleanup |

**Privilege model:** Only `MemoryFlushModifiedList` (cmd 3) doesn't require `SeProfileSingleProcessPrivilege`. All other commands need it. This privilege has LUID `{LowPart=0x0D}`.

---

## 2. SuperFetch (SysMain) Subsystem

### 2.1 Architecture

On Windows 11 24H2, SuperFetch is built **into ntoskrnl** — not a separate `sysmain.sys` driver. Key symbols:

| Symbol | Address | Purpose |
|--------|---------|---------|
| `PfInitializeSuperfetch` | `fffff800`6fd4db20` | Init during boot |
| `PfSetSuperfetchInformation` | `fffff800`6faacc40` | Set/query SuperFetch parameters |
| `PfQuerySuperfetchInformation` | `fffff800`6faac9c8` | Query SuperFetch state |
| `PfGlobals` | `fffff800`6fe57430` | Global state (prefetch path: `\SystemRoot\Prefetch`) |

**Access path:** In this build, SuperFetch is NOT dispatched via `NtSetSystemInformation` (class 0x87 maps to invalid). Instead it's accessed through internal kernel calls (`PfSetSuperfetchInformation` / `PfQuerySuperfetchInformation`) which are called from the Prefetch/SuperFetch scenario subsystem.

### 2.2 PfSetSuperfetchInformation (`fffff800`6faacc40`)

```asm
; 1. Privilege check
ldr   x0, [x8+0xB70]             ; SeProfileSingleProcessPrivilege
bl    SeSinglePrivilegeCheck
cbnz  w8, <proceed>               ; must have privilege
; else → STATUS_ACCESS_DENIED (0xC0000022)

; 2. Size validation
cmp   w19, #0x20                   ; input must be exactly 0x20 bytes
bne   <invalid_param>             ; → STATUS_INVALID_PARAMETER (0xC0000004)

; 3. Copy input structure (0x20 bytes from user buffer)

; 4. Magic value check
ldr   w8, [input+0]              ; offset 0: must be 0x2D
cmp   w8, #0x2D
bne   <invalid_param>

; 5. Session ID validation
ldr   w9, [constant]             ; expected session ID
ldr   w8, [input+4]              ; offset 4: session ID from caller
cmp   w8, w9
bne   <invalid_param>

; 6. Command dispatch
ldr   w8, [input+8]              ; offset 8: command type
sub   w10, w8, #3                 ; commands 3-29 (range 0x1A = 26 commands)
cmp   w10, #0x1A
bhi   <invalid>

; Jump table dispatch to PfSn* functions
adr   x9, <subcmd_table>
ldrsw x8, [x9, w10 uxtw #2]
adr   x9, <dispatch_base>
add   x8, x9, x8 lsl #2
br    x8
```

**Input structure** (0x20 bytes):
```
Offset 0x00: DWORD  Magic (0x2D)
Offset 0x04: DWORD  Session ID
Offset 0x08: DWORD  Command (3-29)
Offset 0x0C: ...    Command-specific data
```

**Sub-command dispatch table** (26 entries, commands 3-28):
- Commands dispatch to various `PfSn*` (Prefetch Scenario) functions
- Notable targets include:
  - `PfpDeprioritizeOldPagesInWs` — de-prioritize old pages in working sets
  - `PfpPrefetchRequest` — initiate a prefetch operation
  - `PfSnPrefetchCacheEntryUpdate` — update prefetch cache entry

### 2.3 PfQuerySuperfetchInformation (`fffff800`6faac9c8`)

```asm
; Validates input size = 0x20
; Checks magic value 0x2D at offset 0
; Validates session ID at offset 4
; Returns query results to user buffer
```

Same input validation as `PfSetSuperfetchInformation`: 0x20-byte structure, magic 0x2D, session ID check.

### 2.4 PfpRpControlRequest (`fffff800`6f8debe8`) — IOCTL Control

This function handles SuperFetch runtime control requests:

```asm
; 1. Check initialized flag
ldrb  w8, [x21+0x330]             ; global initialized flag
tbnz  w8, #0, <proceed>           ; must be initialized
; else → STATUS_DEVICE_NOT_READY (0xC0000080)

; 2. Validate input size
ldr   w1, [x0+0x18]               ; input buffer length
cmp   w1, #0x18                   ; minimum 0x18 bytes
bhs   <proceed>
; else → STATUS_INVALID_BUFFER_SIZE (0xC0000206)

; 3. Copy & verify request
bl    PfpRpControlRequestCopy
bl    PfpRpControlRequestVerify

; 4. Acquire rundown protection
bl    ExAcquireRundownProtection

; 5. Command dispatch (halfword at offset 2)
ldrh  w8, [x20+2]
;   0: PfpRpControlRequestUpdate  — update RP parameters
;   1: PfpRpControlRequestReset   — reset RP state
;  >1: STATUS_INVALID_INFO_CLASS
```

**Three sub-commands:**
- `PfpRpControlRequestUpdate` — Update ReadyBoost/prefetch parameters
- `PfpRpControlRequestReset` — Reset the runtime prefetch state
- Uses `ExAcquireRundownProtection` / `ExReleaseRundownProtection` for safe concurrent access

---

## 3. Prefetch Scenario Subsystem

### 3.1 PfSnSetPrefetcherInformation (`fffff800`6fab7780`)

```asm
; 1. Input validation
cmp   w2, #0x20                    ; size must be 0x20
bne   <invalid_param>

; 2. Copy input structure

; 3. Magic check
ldr   w8, [input+0]               ; magic must be 1 (not 0x2D!)
cmp   w8, #1
bne   <invalid_param>

; 4. Session ID check
ldr   w9, [expected_session]
ldr   w8, [input+4]
cmp   w8, w9
bne   <invalid_param>

; 5. Command dispatch
ldr   w20, [input+8]              ; command number
cmp   w20, #8                     ; max command = 8
bhi   <invalid>

; Bitmask validation (0x128 >> command, check bit 0)
mov   w8, #0x128                   ; valid commands bitmask
lsr   w8, w8, w20
tbz   w8, #0, <invalid>           ; bit not set → invalid command

; Valid commands: 0, 3, 5, 6, 8 (from bitmask 0x128 = 0b100101000)

; 6. Privilege check (for non-special commands)
ldr   x0, [x8+0xB70]             ; SeProfileSingleProcessPrivilege
bl    SeSinglePrivilegeCheck
cbnz  w8, <proceed>
; else → STATUS_ACCESS_DENIED

; 7. Dispatch by command:
; cmd 5: 12-byte sub-structure at input+0x10, calls PfSn* functions
; cmd 6: 0x48-byte prefetch cache entry, validates alignment,
;        calls PfSnPrefetchCacheEntryUpdate
; cmd 8: similar pattern with 0x48-byte entries
```

**Valid Prefetch commands** (from bitmask 0x128):
- Bit positions with 1: 3, 5, 6, 8 (0-indexed from right of `0b100101000`)
- Actually: 0x128 = binary `100101000` → bits 3, 5, 6 are set (plus bit 8)
- Command 6: Update prefetch cache entry (0x48-byte structure)
- Command 5: Set prefetch parameters (0xC-byte sub-structure)

### 3.2 Key Prefetch Symbols

| Symbol | Purpose |
|--------|---------|
| `PfSnInitializePrefetcher` | Boot-time initialization |
| `PfSnSetPrefetcherInformation` | Configure prefetch behavior |
| `PfSnQueryPrefetcherInformation` | Query prefetch state |
| `PfSnPrefetchSections` | Prefetch file sections into memory |
| `PfSnAsyncPrefetchWorker` | Async prefetch worker thread |
| `PfSnBeginAppLaunch` | Begin app launch trace scenario |
| `PfSnEndTrace` | End trace scenario |
| `PfSnPrefetchCacheEntryUpdate` | Update cache entry |
| `PfSnAppLaunchScenarioControl` | Control app launch prefetching |
| `PfSnLogPageFault` | Log page fault for prefetch learning |
| `MiPrefetchVirtualMemory` | Kernel memory prefetch |
| `MmPrefetchPagesEx` | Prefetch pages (extended) |
| `MmPrefetchVirtualMemory` | Top-level memory prefetch API |
| `ExpPrefetchPushLock` | Global synchronization push lock |

---

## 4. RtlAcquirePrivilege / RtlAdjustPrivilege

### 4.1 RtlAcquirePrivilege (`fffff800`6fb11550`) — Kernel Implementation

This is the kernel-mode equivalent of the user-mode `RtlAdjustPrivilege`. It's used by system APIs that need to temporarily enable privileges.

```asm
; 1. Allocate privilege context
; Pool tag: 0x41, size = (privilege_count - 1 + 0x5A) * 0xC
bl    ExAllocatePool2              ; allocate from pool
cbz   x19, <no_memory>            ; → STATUS_INSUFFICIENT_RESOURCES

; 2. Check process flags
ldr   x8, [xpr+0x988]             ; current process/thread
ldr   w8, [x8+0x580]              ; EPROCESS flag
tbz   w8, #3, <normal_path>       ; bit 3 = special token handling needed

; 3. Token manipulation
; If special process: open thread token
bl    RtlpOpenThreadToken          ; open current thread token
; Set Thread Cpu Sets (InformationClass 5) via ZwSetInformationThread
mov   w0, #-2                      ; current thread handle
mov   w1, #5                       ; ThreadCpuSets
bl    ZwSetInformationThread

; 4. Impersonation (if no existing token)
cbnz  x8, <has_token>
bl    RtlImpersonateSelfEx         ; impersonate self with new token

; 5. Build privilege array
; For each privilege LUID from input array:
mov   w10, #2                      ; SE_PRIVILEGE_ENABLED attribute
stp   w11, wzr, [sp+0x18]         ; LUID + attributes
str   w10, [offset+0xC]            ; SE_PRIVILEGE_ENABLED = 2

; 6. Adjust token privileges
mov   w8, #0x400                   ; flags
bl    ZwAdjustPrivilegesToken      ; enable the privileges

; 7. Check result
cmp   w0, <STATUS_SUCCESS>         ; 0
beq   <success>
cmp   w0, #0x106                   ; STATUS_NOT_ALL_ASSIGNED (partial success)
beq   <partial>
; else → error, cleanup
```

**Pool allocation formula:**
```
size = (privilege_count - 1 + 0x5A) * 0xC
tag  = 0x41
```

### 4.2 RtlAdjustPrivilege (User-Mode ntdll.dll)

`RtlAdjustPrivilege` is a user-mode ntdll export (not present in kernel exports). Its kernel equivalent is `RtlAcquirePrivilege` / `RtlReleasePrivilege`.

**User-mode flow:**
1. Opens current process token (`NtOpenProcessToken`)
2. Calls `NtAdjustPrivilegesToken` to enable/disable the specified privilege
3. Returns previous state for restoration

### 4.3 Privilege LUIDs

| Privilege | LUID (LowPart) | Address | Usage |
|-----------|---------------|---------|-------|
| `SeProfileSingleProcessPrivilege` | 0x0D (13) | `fffff800`70023b70` | Memory list commands, SuperFetch, Prefetch |
| `SeIncreaseBasePriorityPrivilege` | 0x0E (14) | `fffff800`70023b50` | Priority boosting, SuperFetch priority |
| `SeDebugPrivilege` | Various | — | Cross-process access |

**Verified via memory read:**
```
dq nt!SeProfileSingleProcessPrivilege → LowPart = 0x0D
dq nt!SeIncreaseBasePriorityPrivilege → LowPart = 0x0E
```

### 4.4 SeSinglePrivilegeCheck Pattern

All SuperFetch/Prefetch and memory list APIs use the same pattern:

```asm
adrp  x8, <page>                  ; fffff800`70023000
ldr   x0, [x8+0xB70]             ; load LUID from privilege global
bl    SeSinglePrivilegeCheck       ; check if current token has it
uxtb  w8, w0                      ; get boolean result
cbnz  w8, <proceed>               ; privileged → continue
; else → STATUS_ACCESS_DENIED (0xC0000022)
```

The privilege global at `[x8+0xB70]` resolves to `SeProfileSingleProcessPrivilege` in all cases.

---

## 5. Data Flow Summary

### 5.1 Memory Cleanup (SystemMemoryListInformation)

```
User mode:  NtSetSystemInformation(SystemMemoryListInformation, &command, 4)
            ↓
Kernel:     NtSetSystemInformation
            → Jump table dispatch (class 0x50)
            → Handler reads command DWORD
            → MmIssueMemoryListCommand(command, partition)
              ├─ Privilege check (SeProfileSingleProcessPrivilege) for cmds 0-2,4-5
              └─ MmPerformMemoryListCommand(command, partition)
                   ├─ 0: MiCaptureAllWorkingSetAccessBits(part, 0)
                   ├─ 1: MiCaptureAllWorkingSetAccessBits(part, 1)
                   ├─ 2: MiEmptyAllWorkingSets(part)
                   ├─ 3: MiFlushAllPages(part, 0, 8)
                   ├─ 4: MiPurgePartitionStandby(part, 8)
                   └─ 5: MiPurgePartitionStandby(part, 1)
```

### 5.2 SuperFetch Control

```
User mode:  NtSetSystemInformation(SystemSuperfetchInformation, &input, 0x20)
            OR internal kernel call
            ↓
Kernel:     PfSetSuperfetchInformation
            ├─ SeProfileSingleProcessPrivilege check
            ├─ Size = 0x20 validation
            ├─ Magic 0x2D check at offset 0
            ├─ Session ID validation at offset 4
            └─ Sub-command dispatch (commands 3-29)
                 ├─ PfpDeprioritizeOldPagesInWs
                 ├─ PfpPrefetchRequest
                 ├─ PfSnPrefetchCacheEntryUpdate
                 └─ ... (26 sub-commands total)

Runtime:    PfpRpControlRequest
            ├─ Initialized flag check
            ├─ Min input 0x18 bytes
            ├─ PfpRpControlRequestCopy + Verify
            ├─ ExAcquireRundownProtection
            └─ Command dispatch:
                 0: PfpRpControlRequestUpdate
                 1: PfpRpControlRequestReset
```

### 5.3 Prefetch Control

```
Kernel:     PfSnSetPrefetcherInformation
            ├─ Size = 0x20 validation
            ├─ Magic = 1 check (different from SuperFetch!)
            ├─ Session ID validation
            ├─ Command bitmask (0x128 → valid: 3, 5, 6, 8)
            ├─ SeProfileSingleProcessPrivilege check
            └─ Dispatch:
                 5: Set prefetch parameters (0xC-byte sub-struct)
                 6: Update prefetch cache entry (0x48-byte struct)
                    → PfSnPrefetchCacheEntryUpdate
```

### 5.4 Privilege Acquisition

```
User mode:  RtlAdjustPrivilege(LUID, Enable, CurrentThread, &Previous)
            → Opens process/thread token
            → NtAdjustPrivilegesToken
            → Returns previous state

Kernel:     RtlAcquirePrivilege(LUID_array, count, flags, &handle)
            → ExAllocatePool2 (tag 0x41)
            → Check EPROCESS+0x580 flags
            → RtlpOpenThreadToken / RtlImpersonateSelfEx
            → Build LUID_AND_ATTRIBUTES array
            → ZwAdjustPrivilegesToken (enable SE_PRIVILEGE_ENABLED)
            → Returns context handle for RtlReleasePrivilege
```

---

## 6. Locking & Synchronization

| Mechanism | Context | Purpose |
|-----------|---------|---------|
| `KfRaiseIrql(DISPATCH_LEVEL)` | `MiPurgePartitionStandby` | PFN database safe manipulation |
| `ExAcquireRundownProtection` | `PfpRpControlRequest` | Safe concurrent access to RP state |
| `ExpPrefetchPushLock` | Global prefetch subsystem | Synchronize prefetch operations |
| `MiQueueWorkingSetRequest` | `MiEmptyAllWorkingSets` | Async WS flush via work queue |
| Token impersonation | `RtlAcquirePrivilege` | Thread-safe privilege enable |

---

## 7. Optimization Implications

### 7.1 Memory Cleanup Strategy

For an optimization tool, the most useful commands are:

| Priority | Command | Effect | Use Case |
|----------|---------|--------|----------|
| 1 | `MemoryPurgeStandbyList` (5) | Free all standby pages | Emergency memory release |
| 2 | `MemoryPurgeLowPriorityStandbyList` (4) | Free low-priority standby only | Selective cleanup |
| 3 | `MemoryFlushModifiedList` (3) | Write dirty pages to disk | Pre-cleanup flush |
| 4 | `MemoryEmptyWorkingSets` (2) | Flush all WS | Aggressive cleanup (performance cost) |

**Recommended sequence:** Flush modified → Purge standby → (optionally empty WS)

### 7.2 SuperFetch/Prefetch Interaction

- SuperFetch uses `SeProfileSingleProcessPrivilege` (not commonly held)
- Prefetch is always active for learning; SuperFetch can be controlled per-app
- The magic value 0x2D in SuperFetch vs 1 in Prefetch distinguishes the two subsystems
- Commands 3-29 give fine-grained control over prefetch behavior

### 7.3 Required Privileges

| Operation | Required Privilege |
|-----------|-------------------|
| Memory list commands 0-2,4-5 | `SeProfileSingleProcessPrivilege` (LUID 0x0D) |
| MemoryFlushModifiedList (3) | None (no privilege needed!) |
| SuperFetch set/query | `SeProfileSingleProcessPrivilege` |
| Prefetch set/query | `SeProfileSingleProcessPrivilege` |
| Priority boosting | `SeIncreaseBasePriorityPrivilege` (LUID 0x0E) |

---

## 8. Key Addresses (ARM64, This Build)

| Symbol | Address |
|--------|---------|
| `NtSetSystemInformation` | `fffff800`6f977e10` |
| `MmIssueMemoryListCommand` | `fffff800`6f9c10a8` |
| `MmPerformMemoryListCommand` | `fffff800`6fca5d18` |
| `MiPurgePartitionStandby` | `fffff800`6f60bb40` |
| `MiRemoveLowestPriorityStandbyPage` | `fffff800`6f60c480` |
| `MiEmptyAllWorkingSets` | `fffff800`6f7e8890` |
| `MiFlushAllPages` | `fffff800`6f7eb688` |
| `MiCaptureAllWorkingSetAccessBits` | `fffff800`6f7e8878` |
| `PfSetSuperfetchInformation` | `fffff800`6faacc40` |
| `PfQuerySuperfetchInformation` | `fffff800`6faac9c8` |
| `PfSnSetPrefetcherInformation` | `fffff800`6fab7780` |
| `PfSnQueryPrefetcherInformation` | `fffff800`6fab7348` |
| `PfpRpControlRequest` | `fffff800`6f8debe8` |
| `PfInitializeSuperfetch` | `fffff800`6fd4db20` |
| `RtlAcquirePrivilege` | `fffff800`6fb11550` |
| `RtlReleasePrivilege` | `fffff800`6fb12b30` |
| `SeProfileSingleProcessPrivilege` | `fffff800`70023b70` (LUID LowPart=0x0D) |
| `SeIncreaseBasePriorityPrivilege` | `fffff800`70023b50` (LUID LowPart=0x0E) |
| `PfGlobals` | `fffff800`6fe57430` |
| `PfSnGlobals` | `fffff800`6fe577d0` |
| Jump table (NtSetSystemInformation) | `fffff800`6f979558` |
| Jump table code base | `fffff800`6f978db4` |
