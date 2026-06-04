# Windows Power Management API — Kernel Research Guide

> Цель: классификация WinAPI управления питанием — какие требуют kernel-level исследования.
> Build: Windows 11 26100 ARM64 | WinDbg kernel debugging

---

## 1. Сводная таблица

| WinAPI | NT Syscall / Kernel Func | Kernel Backend | Сложность | Kernel Dev? |
|--------|--------------------------|----------------|-----------|-------------|
| `PowerSetActiveScheme` | `NtPowerInformation` (level 0x08=SystemPowerPolicyCurrent) | `PopApplyPolicy` → `PopVerifySystemPowerPolicy` → policy update | **VERY HIGH** | **Да** |
| `PowerGetActiveScheme` | Registry read via powrprof.dll | User-mode registry, no kernel call | LOW | Нет |
| `PowerWriteACValueIndex` / `PowerWriteDCValueIndex` | Registry write via powrprof.dll | User-mode registry write | LOW | Нет |
| `CallNtPowerInformation` (ProcessorInformation) | `NtPowerInformation` (level 0x0B) | `PopProcessorInformation` → PPM state read | **HIGH** | **Да** |
| `CallNtPowerInformation` (SystemBatteryState) | `NtPowerInformation` (level 0x09) | `EtwpCoverageUserIsAdmin` check → battery state read | **MEDIUM** | Частично |
| `CallNtPowerInformation` (SystemPowerPolicyCurrent) | `NtPowerInformation` (level 0x08) | `PopApplyPolicy` → policy change dispatch | **VERY HIGH** | **Да** |
| `SetThreadCharacteristics` (MMCSS) | `NtSetInformationThread` | Kernel: thread priority + affinity boost | **HIGH** | **Да** |
| `AvSetMmThreadCharacteristics` | `AvSetMmThreadCharacteristics` (avrt.dll) | User-mode MMCSS service → `SetThreadPriority` + `SetThreadAffinityMask` | **MEDIUM** | Нет |

---

## 2. NtPowerInformation — Master Dispatcher

Все `CallNtPowerInformation` вызовы идут через единый syscall `NtPowerInformation`.

### 2.1 Function Signature

```
NtPowerInformation(
  InformationLevel,    // w0 → [sp+0x68] + [sp+0x14] — power info class
  InputBuffer,         // x1 → x24/x19/x25
  InputBufferLength,   // w2 → w27
  OutputBuffer,        // x3 → [sp+0x78]
  OutputBufferLength   // w4 → [sp+0x50]
)
```

### 2.2 Function Structure

```
NtPowerInformation @ fffff800`6fabb0d0
├─ Stack frame: 0x730 bytes (+ 0x60 saved regs)
├─ Zero 0x600 bytes at sp+0x10 for local state
├─ Security cookie check
├─ Access validation (multiple paths):
│  ├─ Level <= 0x22: bitmap check (allowed without admin)
│  ├─ Level 0x27-0x60: bitmap check (admin required)
│  ├─ Level 0x25-0x5E: SeIsAppContainerOrIdentifyLevelContext
│  ├─ Level 0x5C-0x5D: ExCheckFullProcessInformationAccess
│  ├─ Level 0x1C-0x4E: EtwpCoverageUserIsAdmin
│  ├─ Level 0x47: PopIsRunningAsLocalSystem
│  ├─ Level 0x0B: SeSinglePrivilegeCheck
│  └─ Level 0x09/0x26-0x5F: EtwpCoverageUserIsAdmin
├─ Policy lock acquisition (if needed):
│  └─ PopAcquirePolicyLock → ExAcquireResourceExclusiveLite
├─ Jump table dispatch @ fffff800`6fabcbc0
│  └─ 100+ entries for different power info levels
└─ Cleanup: ExFreePoolWithTag, PopReleasePolicyLock
```

### 2.3 Jump Table (partial — key levels)

| Level | Name | Target Offset |
|-------|------|---------------|
| 0x00 | SystemPowerPolicyAc | -0x418 → read AC policy |
| 0x01 | SystemPowerPolicyDc | -0x418 → read DC policy |
| 0x02 | VerifySystemPowerPolicyAc | -0x3F0 → validate + apply |
| 0x03 | VerifySystemPowerPolicyDc | -0x3DD → validate + apply |
| 0x04 | SystemPowerCapabilities | -0x382 → read caps |
| 0x05 | SystemBatteryState | -0x355 → battery query |
| 0x09 | (internal) | Admin check → battery state |
| 0x0A | ProcessorInformation (admin) | `SeSinglePrivilegeCheck` |
| 0x0B | ProcessorInformation | `PopProcessorInformation` |
| 0x0C | SystemPowerInformation | `PopCurrentPowerState` |
| 0x0D | ProcessorPowerPolicyAc | read AC processor policy |
| 0x0E | ProcessorPowerPolicyDc | read DC processor policy |
| 0x1F+ | Extended levels | Various policy/device queries |

> Jump table is at `NtPowerInformation+0x1AF0` (fffff800'6fabcbc0), 100+ DWORD offsets, indexed by InformationLevel.

---

## 3. PopAcquirePolicyLock / PopReleasePolicyLock

All power policy writes go through an exclusive ERESOURCE lock:

### 3.1 Acquire

```asm
nt!PopAcquirePolicyLock:
  ; Boost current thread IO priority
  ldr   x0, [xpr, #0x988]        ; EPROCESS of current thread
  mov   w1, #0                     ; boost IO priority
  bl    PsBoostThreadIo

  ; Decrement EPROCESS+0x1DC (power refcount, signed halfword)
  ldr   x9, [xpr, #0x988]
  mov   w1, #1
  ldrsh w8, [x9, #0x1DC]          ; current refcount
  sub   w8, w8, #1
  strh  w8, [x9, #0x1DC]          ; EPROCESS+0x1DC -= 1

  ; Acquire exclusive ERESOURCE lock
  adrp  x8, PopSleepStats+0x5E0   ; PopPolicyLock (ERESOURCE)
  bl    ExAcquireResourceExclusiveLite

  ; Store owning thread pointer
  ldr   x9, [xpr, #0x988]
  adrp  x8, PopWdiScenarioStopEventData+0x120
  str   x9, [x8, #0x898]          ; store EPROCESS for lock tracking
  ret
```

### 3.2 Release

```asm
nt!PopReleasePolicyLock:
  ; Clear owning thread pointer
  adrp  x8, PopWdiScenarioStopEventData+0x120
  str   xzr, [x8, #0x898]

  ; Release ERESOURCE lock
  adrp  x8, PopSleepStats+0x5E0
  bl    ExReleaseResourceLite

  ; Check for pending power work
  bl    PopCheckForWork
  bl    KeLeaveCriticalRegion

  ; Restore thread IO priority
  ldr   x0, [xpr, #0x988]
  mov   w1, #1
  bl    PsBoostThreadIo
  ret
```

### 3.2 Key Structures

| Offset | Field | Description |
|--------|-------|-------------|
| `EPROCESS+0x1DC` | Power refcount (SHalf) | Decremented on lock acquire, tracked per-process |
| `PopSleepStats+0x5E0` | `PopPolicyLock` | ERESOURCE exclusive lock for all policy changes |
| `PopWdiScenarioStopEventData+0x898` | Lock owner | EPROCESS pointer of current lock holder |
| `PopSleepStats+0x404` | Active AC policy | SYSTEM_POWER_POLICY (AC) |
| `PopSleepStats+0x578` | Cached policy | memcmp target for change detection |

> **Вывод:** `PopAcquirePolicyLock` uses `ERESOURCE` (not spinlock) — can be held across page faults. Boosts calling thread's IO priority to prevent priority inversion during policy changes.

---

## 4. PopApplyPolicy — Core Policy Change Engine

Called when `NtPowerInformation` receives a write request for power policy (levels 0x02, 0x03, etc.)

### 4.1 Disassembly

```asm
nt!PopApplyPolicy @ fffff800`6fabcdd0
  ; Parameters: w0=flags, w1=policy_type, x2=policy_data, w3=data_size
  ; Stack: 0x1F0 bytes local + 0x50 saved regs

  ; Zero local policy buffer (0xE0 bytes = SYSTEM_POWER_POLICY size)
  movi  v16.16b, #0
  ; ... zero fill loop ...

  ; Validate size
  cmp   w3, #0xE8                 ; SYSTEM_POWER_POLICY = 0xE8 bytes
  bhs   size_ok
  ldr   w0, [error_code]           ; STATUS_BUFFER_TOO_SMALL

size_ok:
  ; Copy caller's policy to local buffer
  ; ... 0xE0 bytes memcpy ...

  ; Validate policy structure
  bl    PopVerifySystemPowerPolicy  ; returns STATUS

  ; Compare with current policy (memcmp)
  adrp  x24, PopSleepStats+0x5E0
  ldr   x23, [x24, #0x578]         ; current cached policy
  mov   x2, #0xE8                   ; size
  bl    _memcmp_spec_unaligned

  ; If identical AND no force flag → skip (no change needed)
  cbnz  w0, policy_changed
  cbnz  w19, policy_changed          ; force flag?
  b     done                         ; no change, return success

policy_changed:
  ; Check each of 4 discharge policies for changes
  add   x25, x23, #0x60             ; DischargePolicy array
  mov   w23, #0                      ; index
  mov   w22, #0                      ; change flags
check_loop:
  cmp   w23, #4
  bhs   apply_changes

  ; memcmp each 0x18-byte discharge policy entry
  umaddl x1, w23, #0x18, x25        ; old policy offset
  umaddl x0, w23, #0x18, sp+0x80    ; new policy offset
  mov   x2, #0x18
  bl    _memcmp_spec_unaligned
  ; ... if different, set change flag ...
  add   w23, w23, #1
  b     check_loop
```

### 4.2 Mechanism

1. Copy caller's `SYSTEM_POWER_POLICY` to local buffer
2. **Validate** via `PopVerifySystemPowerPolicy` — checks all fields for correctness
3. **Compare** with current policy via `memcmp` — skip if identical
4. **Diff discharge policies** — iterate 4 entries, track which changed
5. **Apply changes** — update globals, notify devices, ETW trace
6. **Release lock** — `PopReleasePolicyLock`

---

## 5. PopVerifySystemPowerPolicy — Policy Validation

```asm
nt!PopVerifySystemPowerPolicy @ fffff800`6fabf920
  ; Copy policy from source to local buffer (0xE0 bytes)
  ; ... memcpy loop ...

  ; Check Revision field
  ldr   w8, [x19]               ; Revision
  cmp   w8, #1
  beq   revision_ok
  ldr   w0, [STATUS_INVALID_PARAMETER]
  b     return

revision_ok:
  ; Load power features flags
  adrp  x21, PopSleepStats+0x780
  ldrb  w8, [x21, #5]            ; feature byte
  cbz   w8, no_special
  str   w22, [x19, #0x48]        ; override field
  b     continue

no_special:
  ldrb  w8, [x21, #4]
  cbz   w8, use_default
  mov   w8, #3
  str   w8, [x19, #0x48]
  ; ... continue validation ...
```

### 5.1 SYSTEM_POWER_POLICY Structure

```
dt nt!_SYSTEM_POWER_POLICY (0xE8 bytes):
  +0x000 Revision              : Uint4B
  +0x004 PowerButton           : POWER_ACTION_POLICY    (0x0C bytes each)
  +0x010 SleepButton           : POWER_ACTION_POLICY
  +0x01C LidClose              : POWER_ACTION_POLICY
  +0x028 LidOpenWake           : _SYSTEM_POWER_STATE
  +0x02C Reserved              : Uint4B
  +0x030 Idle                  : POWER_ACTION_POLICY
  +0x03C IdleTimeout           : Uint4B
  +0x040 IdleSensitivity       : UChar
  +0x041 DynamicThrottle       : UChar       ← CPU throttling policy
  +0x044 MinSleep              : _SYSTEM_POWER_STATE
  +0x048 MaxSleep              : _SYSTEM_POWER_STATE
  +0x04C ReducedLatencySleep   : _SYSTEM_POWER_STATE
  +0x060 DischargePolicy[4]    : SYSTEM_POWER_LEVEL  (0x18 each = 0x60)
  +0x0C0 VideoTimeout          : Uint4B
  +0x0C4 VideoDimDisplay       : UChar
  +0x0D4 SpindownTimeout       : Uint4B
  +0x0D8 OptimizeForPower      : UChar
  +0x0D9 FanThrottleTolerance  : UChar
  +0x0DA ForcedThrottle        : UChar
  +0x0DB MinThrottle           : UChar
  +0x0DC OverThrottled         : POWER_ACTION_POLICY
```

> `DynamicThrottle` at +0x041 is the CPU throttle policy: `PO_THROTTLE_NONE`, `PO_THROTTLE_CONSTANT`, `PO_THROTTLE_DEGRADE`, `PO_THROTTLE_ADAPTIVE`.

---

## 6. PowerSetActiveScheme / PowerGetActiveScheme

### 6.1 Kernel Path

```
PowerSetActiveScheme (powrprof.dll, user-mode)
  → Registry write: HKLM\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes\ActiveOverlayScheme
  → CallNtPowerInformation(SystemPowerPolicyCurrent, ...)
    → NtPowerInformation (level 0x08)
      → PopAcquirePolicyLock
      → PopApplyPolicy(0, 1, policy_data, 0xE8)
        → PopVerifySystemPowerPolicy
        → memcmp vs current policy
        → Update globals, notify devices
      → PopReleasePolicyLock
```

### 6.2 Key Power Globals

| Symbol | Address | Description |
|--------|---------|-------------|
| `PopPolicy` | `fffff800'6ff81578` | Current active power policy (SYSTEM_POWER_POLICY) |
| `PopAdminPolicy` | `fffff800'6ff81560` | Admin override policy |
| `PopFlushPolicy` | `fffff800'70009f04` | Flush state (0 = normal) |
| `PopPolicyLock` | `fffff800'6ff815e0` | ERESOURCE for policy serialization |
| `PopPolicyDeviceLock` | `fffff800'6ff80880` | Device notification lock |
| `PopPolicyWorker` | `fffff800'6ff816a0` | Worker thread for async policy changes |
| `PopEsBgActivityPolicy` | `fffff800'6fe57ef4` | Background activity policy |

### 6.3 Power Scheme Overlay Mapping

Power schemes in modern Windows use "overlays" on top of base GUIDs:
- `381b4222-f694-4111-a2b7-06c18db6b684` — Balanced (default)
- `8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c` — High Performance
- `a1841308-3541-4fab-bc81-f71558f79b6a` — Power Saver
- Overlays: `961cc777-2547-4479-b318-e4f38e8b0d0a` (Better Performance), etc.

---

## 7. PowerWriteACValueIndex / PowerWriteDCValueIndex

### 7.1 Kernel Path

```
PowerWriteACValueIndex / PowerWriteDCValueIndex (powrprof.dll, user-mode)
  → Registry write: HKLM\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes\
    {SchemeGuid}\{SubGroupGuid}\{SettingGuid}\ACValueIndex (or DCValueIndex)
  → NO kernel call for the write itself
  → Changes take effect when:
    1. Power scheme is re-applied (PowerSetActiveScheme)
    2. Power setting callback fires (PopDeviceIdlePolicySettingCallback, etc.)
    3. WNF notification triggers policy refresh
```

### 7.2 Registry → Kernel Flow

```
Registry Write
  → WNF Notification (PopWnfEffectivePowerModeCallback)
    → PopApplyAdminPolicy
      → PopSetNewPolicyValue
        → PpmSetProfilePolicySetting  (for processor GUIDs)
        → PopNotifyPolicyDevice
          → PopAcquirePolicyLock
          → Device notification dispatch
          → PopReleasePolicyLock
```

### 7.3 PopSetNewPolicyValue — GUID-Based Policy Routing

```asm
nt!PopSetNewPolicyValue @ fffff800`6fac2b38
  ; Convert GUID to string for registry lookup
  bl    RtlStringFromGUIDEx

  ; Check if this is a session-specific setting
  bl    PopStateIsSessionSpecific

  ; Compare with GUID_PROCESSOR_SHORT_THREAD_ARCH_CLASS_UPPER_THRESHOLD
  ; to route to PPM (processor power management)
  bl    _memcmp_spec_unaligned
  cbnz  w0, not_processor_guid

  ; Route to processor power management
  bl    PpmSetProfilePolicySetting

not_processor_guid:
  ; Compare with EnableProcessTracingCallbacks GUID
  ; Check result, route to appropriate handler
```

> **Key insight:** `PopSetNewPolicyValue` routes power setting changes by GUID comparison. Processor-related GUIDs go to `PpmSetProfilePolicySetting`; others go to device-specific handlers.

---

## 8. CallNtPowerInformation — ProcessorInformation

### 8.1 NtPowerInformation Level 0x0B (ProcessorInformation)

```asm
; Entry in NtPowerInformation for level 0x0B:
cmp   w8, #0xB                    ; level == 0x0B?
beq   PopProcessorInformation_call ; yes → dispatch

; Level 0x0A requires SeSinglePrivilegeCheck
cmp   w8, #0xA
; Load privilege from PopPowerAggregatorTargetStateContexts+0xBA8
ldr   x9, [x8, #0x200]           ; privilege value
ldr   x8, [x8]                    ; alternative privilege
cseleq x0, x9, x8                ; pick based on level
bl    SeSinglePrivilegeCheck
cbnz  w8, access_granted
; Access denied → STATUS_ACCESS_DENIED
```

### 8.2 PopProcessorInformation — Per-CPU Power State

```asm
nt!PopProcessorInformation @ fffff800`6fac8d00
  ; Query group affinity for processor enumeration
  bl    KeQueryGroupAffinity        ; get processor mask for group

  ; Count active processors (popcount of affinity mask)
  ins   v16.d[0], x0
  cnt   v16.8b, v16.8b             ; byte count = popcount
  uaddlv h16, v16.8b               ; horizontal sum
  umov  w20, v16.h[0]              ; processor count

  ; Size check: count * 24 bytes per processor (0x18)
  add   w8, w20, w20, lsl #1      ; count * 3
  lsl   w24, w8, #3               ; count * 24
  cmp   w24, #0x600                ; max 64 processors * 24 = 0x600
  bhi   buffer_too_small

  ; Acquire shared push lock for PPM state
  adrp  x25, PopSleepStats+0x390
  bl    FsRtlAcquirePushLockShared

  ; For each processor in mask:
  ;   rbit + clz → find set bit index
  ;   Map processor index to PPM domain
  ;   Read current PPM state via PpmPerfGetCurrentState
  ;   Store: processor number, current frequency, max frequency, etc.

  ; Per-processor entry (0x18 = 24 bytes):
  ;   +0x00: Processor number
  ;   +0x08: Current MHz (PpmPerfGetCurrentState)
  ;   +0x0C: Max MHz
  ;   +0x10: Mhz limit
  ;   +0x14: Maximum Mhz since boot
```

### 8.3 PROCESSOR_POWER_INFORMATION (user-mode structure, 24 bytes)

```c
typedef struct {
  ULONG Number;             // +0x00: processor index
  ULONG MaxMhz;             // +0x04: max frequency
  ULONG CurrentMhz;         // +0x08: current frequency
  ULONG MhzLimit;           // +0x0C: frequency limit
  ULONG MaxIdleState;       // +0x10: max idle state
  ULONG CurrentIdleState;   // +0x14: current idle state
} PROCESSOR_POWER_INFORMATION;
```

### 8.4 PPM Data Sources

| Source | Offset | Description |
|--------|--------|-------------|
| `KiIdleProcessExtension+0xBF8` | processor map | Index → PPM domain mapping |
| `PpmPolicyConfigTable+0xA80` | domain count | Number of PPM domains |
| `PPM domain+0x789` | throttle state | Current throttle byte |
| `PPM domain+0x1550` | perf state | Extended performance info |
| `PopSleepStats+0x780` | feature flags | Power feature enable bits |

> **Вывод:** `PopProcessorInformation` iterates all processors, reads per-CPU PPM state via shared push lock. Complex interaction with PPM (Processor Power Management) domains. **Требует kernel-разработчика** для понимания PPM domain mapping и throttle state.

---

## 9. CallNtPowerInformation — SystemBatteryState

### 9.1 Level 0x09 Path

```asm
; NtPowerInformation validation for level 0x09:
cmp   w8, #9                     ; SystemBatteryState
beq   admin_check_path

admin_check_path:
  bl    EtwpCoverageUserIsAdmin   ; check if caller is admin
  uxtb  w8, w0
  cbnz  w8, proceed               ; admin → continue
  ; Non-admin → STATUS_ACCESS_DENIED
```

### 9.2 Battery State Read

After passing admin check, battery state is read from the power state structure:

```asm
; In NtPowerInformation dispatch for battery queries:
  ; Read battery string from caller
  bl    RtlStringCbLengthW        ; validate string length

  ; Call battery state reader
  bl    PopBatteryDeviceState      ; read battery state from device

  ; Output: 0x34 bytes (SYSTEM_BATTERY_STATE)
```

### 9.3 PopBatteryDeviceState — Battery Query Engine

```asm
nt!PopBatteryDeviceState @ fffff800`6facd758
  ; Parse device string, walk character by character
  mov   x8, #0x7FFF               ; max string length
  ldrh  w10, [x9], #2             ; read WCHAR

  ; Validate string length and format
  ; Look up battery device by string match

  ; Check if caller has access to battery data
  bl    PsGetCurrentServerSiloGlobals

  ; Compare with known battery device strings
  bl    _memcmp_spec_unaligned

  ; Read battery tag, status, rates, capacity
  ; → fills SYSTEM_BATTERY_STATE output
```

> **Вывод:** Battery state requires admin privileges. The actual read involves battery device string matching and silo globals. **Частично требует kernel-разработчика** — data reading is straightforward but access control is complex.

---

## 10. MMCSS — Multimedia Class Scheduler Service

### 10.1 Architecture

```
User Application
  │
  ├─ AvSetMmThreadCharacteristics("Pro Audio", &taskIndex)
  │   └─ avrt.dll → MMCSS service (svchost)
  │       ├─ Lookup task profile in registry
  │       ├─ SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL or similar)
  │       ├─ SetThreadAffinityMask (to specific cores)
  │       └─ Configure latency-sensitive scheduling
  │
  └─ SetThreadCharacteristics (lower-level)
      └─ NtSetInformationThread(ThreadInformationClass)
          ├─ ThreadPriority (0x02) → KeSetBasePriorityThread
          ├─ ThreadAffinityMask (0x04) → KeSetSystemAffinityThread
          └─ ThreadIdealProcessor (0x05) → KeSetIdealProcessorThread
```

### 10.2 Kernel-Level Effects

MMCSS is primarily a **user-mode service** (running in svchost as "LocalSystem"). It uses standard WinAPI calls that map to these kernel functions:

| MMCSS Action | Kernel Function | Complexity |
|-------------|-----------------|------------|
| Priority boost | `KeSetBasePriorityThread` | **HIGH** — priority arithmetic, IRQL management |
| Core affinity | `KeSetSystemAffinityThread` | **HIGH** — KAFFINITY_EX, group-aware |
| Ideal processor | `KeSetIdealProcessorThread` | **HIGH** — scheduler interaction |
| IO priority | `PsBoostThreadIo` | **MEDIUM** — used in PopAcquirePolicyLock too |

### 10.3 AvSetMmMaxThreadCharacteristics

Extended version of `AvSetMmThreadCharacteristics` that allows specifying:
- Priority (instead of using profile default)
- Affinity (instead of using profile default)
- Both audio and GPU task characteristics simultaneously

> **Вывод:** MMCSS itself is **user-mode only** (avrt.dll + svchost service). Kernel effects are through standard `NtSetInformationThread` calls. No special kernel path for MMCSS. **Не требует kernel-разработчика** — the kernel functions it uses (priority, affinity) are already documented in `optimization_api_research.md`.

---

## 11. PopCurrentPowerState / PopCurrentPowerStatePrecise

### 11.1 PopCurrentPowerState — Simple Read

```asm
nt!PopCurrentPowerState @ fffff800`6facdb70
  ; Load power state structure base
  adrp  x19, PopWdiScenarioStopEventData+0xC80

  ; Acquire shared push lock
  add   x0, x19, #0x210
  bl    FsRtlAcquirePushLockShared

  ; Read 0x20 bytes (two QP pairs) from shared state
  ldp   q17, q16, [x19, #0x220]   ; power state data
  stp   q17, q16, [x20]            ; copy to output

  ; Release lock
  bl    PopReleaseRwLock
  ret
```

### 11.2 PopCurrentPowerStatePrecise — Synchronized Read

```asm
nt!PopCurrentPowerStatePrecise @ fffff800`6facdbb8
  ; Load battery update generation counter
  ldr   x8, [ptr_generation_counter]
  ldr   x24, [x8]                  ; current generation

  ; Load power state base
  adrp  x22, PopWdiScenarioStopEventData+0xC80

retry:
  ; Read current snapshot
  ldr   x8, [x22, #0x200]          ; timestamp/generation
  str   x8, [sp, #0x10]            ; save for comparison
  add   x8, x8, x23                ; add timeout value
  cmp   x8, x24                    ; compare with current gen
  bhi   wait_for_update

  ; Wait for battery work with push lock
  mov   w0, #0x20
  bl    PopBatteryQueueWork

  ; Block on address push lock (wait for update)
  bl    ExBlockOnAddressPushLock
  cmp   w19, #0x102                ; STATUS_TIMEOUT
  bne   retry                       ; if not timeout → retry

wait_for_update:
  ; Fallback to non-precise read
  bl    PopCurrentPowerState
```

> `PopCurrentPowerStatePrecise` uses a generation counter + push lock wait to ensure the battery state is fresh. If the wait times out, falls back to the cached value from `PopCurrentPowerState`.

---

## 12. Power Policy Device Notification

### 12.1 PopNotifyPolicyDevice

```asm
nt!PopNotifyPolicyDevice @ fffff800`6f8fb5c0
  ; Compare device GUID with known GUIDs
  bl    _memcmp_spec_unaligned      ; compare with wake alarm GUID

  ; Dispatch based on notification type:
  cmp   w20, #8                     ; WakeAlarmNotification
  beq   handle_wake_alarm
  cmp   w20, #3                     ; HiberFile policy change
  bne   other

  ; HiberFile policy change:
  bl    PopAcquireTransitionLock
  bl    PopAcquirePolicyLock
  ldrb  w19, [PpmPolicyConfigTable+0xAFB]  ; hiberfile policy flag
  bl    PopEnableHiberFile(0)        ; disable first
  cbnz  w19, enable_hiber
  bl    PopEnableHiberFile(1)        ; re-enable if policy says so
  bl    PopReleasePolicyLock
```

### 12.2 Notification Types

| Type | Description |
|------|-------------|
| 3 | Hibernate file policy change |
| 8 | Wake alarm notification |
| Others | Device-specific power state changes |

---

## 13. PpmSetProfilePolicySetting — Processor Policy Engine

```asm
nt!PpmSetProfilePolicySetting @ fffff800`6fac9840
  ; Parameters: x0=policy_object, x1=AC/DC flag, w2=policy_index, x3=data, w4=size

  ; Check for special GUID (arch class threshold)
  adrp  x1, string+0x278           ; GUID_PROCESSOR_SHORT_THREAD_ARCH_CLASS...
  bl    _memcmp_spec_unaligned
  cbnz  w0, not_special_guid
  mov   w0, #0                      ; return success without action
  ret

not_special_guid:
  ; Load PPM configuration
  adrp  x8, KeNumberNodes
  add   x27, x8, #0xEC0            ; PPM config table

  ; Iterate over all NUMA nodes
  ; For each node, apply the policy setting
  ; Calls into platform-specific PPM handler
```

> Routes processor power settings (frequency limits, idle states, throttling) to PPM infrastructure. Complex interaction with NUMA topology and per-domain PPM state.

---

## 14. Практические рекомендации для оптимизации

### 14.1 Power Scheme Management

| Операция | API | Kernel involvement |
|----------|-----|-------------------|
| Read active scheme | `PowerGetActiveScheme` | Registry only (user-mode) |
| Set active scheme | `PowerSetActiveScheme` | **Full kernel path**: `PopApplyPolicy` |
| Write AC/DC value | `PowerWriteACValueIndex` | Registry write → WNF callback → kernel |
| Read processor state | `CallNtPowerInformation(ProcessorInformation)` | `PopProcessorInformation` → PPM read |
| Read battery state | `CallNtPowerInformation(SystemBatteryState)` | Admin check → battery device query |

### 14.2 Priority Research Matrix

1. **HIGH PRIORITY:** `NtPowerInformation` dispatch — massive function, 100+ info levels, complex access control, policy serialization through ERESOURCE lock
2. **HIGH PRIORITY:** `PopApplyPolicy` — core policy change engine, validates via `PopVerifySystemPowerPolicy`, diffs discharge policies, notifies devices
3. **HIGH PRIORITY:** `PopProcessorInformation` — per-CPU power state reading, PPM domain mapping, shared push lock pattern
4. **MEDIUM:** `PopSetNewPolicyValue` — GUID-based routing of power setting changes to PPM or device handlers
5. **LOW:** `PowerWriteACValueIndex/DCValueIndex` — pure registry writes, no direct kernel call
6. **LOW:** MMCSS (`AvSetMmThreadCharacteristics`) — user-mode service using standard thread priority/affinity APIs

### 14.3 Key Locking Hierarchy

```
PopTransitionLock        (for state transitions)
  └─ PopPolicyLock       (ERESOURCE, exclusive for writes, shared for reads)
       └─ PushLock (PPM)  (per-domain, shared for reads)
```

> **Warning:** Policy lock is ERESOURCE (can be held across page faults). Never acquire spinlocks while holding PopPolicyLock.

---

## 15. Lessons Learned (WinDbg Research)

| # | Observation | Implication |
|---|------------|-------------|
| 1 | `NtPowerInformation` has 100+ switch cases via jump table | Complex function — new levels added per Windows build |
| 2 | Multiple access control paths: admin check, LocalSystem check, AppContainer, privilege check | Different levels have different security requirements |
| 3 | `PopAcquirePolicyLock` boosts thread IO priority (`PsBoostThreadIo`) | Prevents priority inversion during policy changes |
| 4 | `EPROCESS+0x1DC` tracks power lock refcount (signed halfword) | Per-process power state tracking |
| 5 | `PopApplyPolicy` uses `memcmp` for change detection | No change = no action (optimization) |
| 6 | `PopProcessorInformation` uses push lock (not ERESOURCE) | Concurrent reads allowed, low contention |
| 7 | MMCSS is entirely user-mode (avrt.dll + svchost service) | No kernel-specific MMCSS path exists |
| 8 | `PowerWriteACValueIndex` is registry-only | Changes propagate via WNF → kernel callbacks |
| 9 | `PpmSetProfilePolicySetting` routes by GUID comparison | New processor GUIDs need kernel updates |
| 10 | Battery state requires admin (`EtwpCoverageUserIsAdmin`) | Non-admin apps cannot read battery state via this API |
