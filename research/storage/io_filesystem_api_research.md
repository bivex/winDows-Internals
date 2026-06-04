# Windows I/O & Filesystem API — Kernel Research Guide

> Цель: классификация WinAPI для I/O и файловых операций — какие требуют kernel-level исследования.
> Build: Windows 11 26100 ARM64 | WinDbg kernel debugging

---

## 1. Сводная таблица

| WinAPI | NT Syscall | Kernel Backend | Сложность | Kernel Dev? |
|--------|-----------|----------------|-----------|-------------|
| `CreateFile` (flags) | `NtCreateFile` | `IopCreateFile` → FILE_OBJECT setup | **HIGH** | **Да** |
| `ReadFileEx` / `WriteFileEx` | `NtReadFile` / `NtWriteFile` | IRP alloc → APC completion | **VERY HIGH** | **Да** |
| `SetFileIoOverlappedRange` | `NtSetInformationFile` (FileIoPriorityHintInfo) | `IopSetFileObjectIosbRange` | **HIGH** | **Да** |
| `CancelIo` | `NtCancelIoFile` | `IopCompleteIrpInFileObjectList` | **MEDIUM** | Частично |
| `CancelIoEx` | `NtCancelIoFileEx` | Same + IO_STATUS_BLOCK filtering | **MEDIUM** | Частично |
| `SetFileInformationByHandle` (FileIoPriorityHintInfo) | `NtSetInformationFile` (class=0x0D) | Giant switch — 84 classes | **MEDIUM** | Частично |
| `DeviceIoControl` | `NtDeviceIoControlFile` | `IopXxxControlFile` | **HIGH** | **Да** |
| `FSCTL_*` via `DeviceIoControl` | `NtFsControlFile` | `IopXxxControlFile` (shared!) | **HIGH** | **Да** |
| `GetOverlappedResult` | `NtWaitForSingleObject` | `ObWaitForSingleObject` | LOW | Нет |
| `GetOverlappedResultEx` | `NtWaitForSingleObject` + timeout | Same + alertable wait | LOW | Нет |

---

## 2. CreateFile — Flags Translation

### 2.1 NtCreateFile → IopCreateFile

```
nt!NtCreateFile @ fffff800`6fa37540
  → bl nt!IopCreateFile @ fffff800`6fa2e968
```

`NtCreateFile` — thin wrapper. Sets up 0x40 byte stack frame, calls `IopCreateFile` with all parameters.

```asm
; NtCreateFile sets w8 = 0x20 (CreateOptions flags slot)
mov   w8,#0x20
str   xzr,[sp,#0x38]         ; clear result
str   w8,[sp,#0x30]          ; CreateOptions
str   wzr,[sp,#0x28]         ; CreateDisposition = 0
str   xzr,[sp,#0x20]         ; EaBuffer = NULL
str   wzr,[sp,#0x18]         ; EaLength = 0
; ... copy remaining params from registers ...
bl    nt!IopCreateFile
```

### 2.2 FILE_FLAG_* → FILE_OBJECT Flags Mapping

| WinAPI Flag | FILE_OBJECT Flag | Offset | Bit |
|-------------|-----------------|--------|-----|
| `FILE_FLAG_NO_BUFFERING` | `FO_NO_INTERMEDIATE_BUFFERING` | +0x50 | 1 |
| `FILE_FLAG_WRITE_THROUGH` | `FO_WRITE_THROUGH` | +0x50 | 2 |
| `FILE_FLAG_OVERLAPPED` | `FO_SYNCHRONOUS_IO` **cleared** | +0x50 | 3 (inverted!) |
| `FILE_FLAG_SEQUENTIAL_SCAN` | `FO_SEQUENTIAL_ONLY` | +0x50 | 4 |
| `FILE_FLAG_RANDOM_ACCESS` | `FO_RANDOM_ACCESS` | +0x50 | ? |

> **Note:** `FILE_FLAG_OVERLAPPED` is inverted — it clears `FO_SYNCHRONOUS_IO`.
> Without the flag, synchronous I/O is assumed (FO_SYNCHRONOUS_IO set).

---

## 3. FILE_OBJECT Structure (ARM64)

```
dt nt!_FILE_OBJECT (key fields):
  +0x000 Type             : Int2B
  +0x002 Size             : Int2B
  +0x008 DeviceObject     : Ptr64 _DEVICE_OBJECT
  +0x010 Flags            : Uint4B          ← FO_* flags + IO priority (bits 16-18)
  +0x018 FsContext        : Ptr64 Void
  +0x020 FsContext2       : Ptr64 Void
  +0x028 SectionObjectPointer : Ptr64
  +0x030 PrivateCacheMap  : Ptr64 Void
  +0x038 FinalStatus      : Int4B
  +0x040 RelatedFileObject : Ptr64
  +0x048 LockOperation    : UChar
  +0x049 DeletePending    : UChar
  +0x04A ReadAccess       : UChar
  +0x04B WriteAccess      : UChar
  +0x04C DeleteAccess     : UChar
  +0x04D SharedRead       : UChar
  +0x04E SharedWrite      : UChar
  +0x04F SharedDelete     : UChar
  +0x050 Flags2           : Uint4B          ← additional flags
  +0x058 CurrentByteOffset : _LARGE_INTEGER  ← file pointer (sync I/O)
  +0x060 Waiters          : Uint4B
  +0x068 Busy             : Uint4B
  +0x070 LastLock         : Ptr64 Void
  +0x078 Lock             : _KEVENT
  +0x090 Event            : _KEVENT
  +0x0A8 Completion       : _IO_COMPLETION_CONTEXT
  +0x0B0 IrpListLock      : Uint8B          ← spinlock for IRP list (KSPIN_LOCK)
  +0x0B8 IrpList          : _LIST_ENTRY     ← pending IRPs for this file
```

### Key Observations:
- **+0x10 Flags** — dual purpose: FO_* flags in lower bits, **IO priority in bits 16-18**
- **+0xB0 IrpListLock** — `KSPIN_LOCK`, acquired via `KeAcquireSpinLockRaiseToDpc`
- **+0xB8 IrpList** — linked list of pending IRPs queued to this file object

---

## 4. IRP Structure (ARM64)

```
dt nt!_IRP:
  +0x000 Type             : Int2B           ← IO_TYPE_IRP = 0x0F
  +0x002 Size             : Uint2B
  +0x004 AllocationProcessorNumber : Uint2B
  +0x006 Reserved1        : Uint2B
  +0x008 MdlAddress       : Ptr64 _MDL
  +0x010 Flags            : Uint4B
  +0x014 Reserved2        : Uint4B
  +0x018 AssociatedIrp    : union { SystemBuffer, IrpCount, Tail }
  +0x020 ThreadListEntry  : _LIST_ENTRY
  +0x030 IoStatus         : _IO_STATUS_BLOCK  ← completion status
  +0x040 RequestorMode    : Char             ← UserMode(1) / KernelMode(0)
  +0x041 PendingReturned  : UChar
  +0x042 StackCount       : Char             ← number of IO_STACK_LOCATIONs
  +0x043 CurrentLocation  : Char             ← current stack index
  +0x044 Cancel           : UChar            ← cancel flag
  +0x045 CancelIrql       : UChar
  +0x046 ApcEnvironment   : Char
  +0x047 AllocationFlags  : UChar
  +0x048 UserIosb         : Ptr64 _IO_STATUS_BLOCK  ← user-mode IOSB pointer
  +0x050 UserEvent        : Ptr64 _KEVENT            ← completion event
  +0x058 Overlay          : union { AsynchronousParameters, Thread }
  +0x068 CancelRoutine    : Ptr64 void               ← driver cancel callback
  +0x070 UserBuffer       : Ptr64 Void
  +0x078 Tail             : union { Apc, CompletionKey, Overlay }
```

### IO_STACK_LOCATION (0x48 bytes each, appended after IRP):

```
dt nt!_IO_STACK_LOCATION:
  +0x000 MajorFunction    : UChar           ← IRP_MJ_READ(3), WRITE(4), DEVICE_CONTROL(14), etc.
  +0x001 MinorFunction    : UChar
  +0x002 Flags            : UChar
  +0x003 Control          : UChar
  +0x008 Parameters       : union (Read, Write, DeviceIoControl, etc.)
  +0x028 DeviceObject     : Ptr64 _DEVICE_OBJECT
  +0x030 FileObject       : Ptr64 _FILE_OBJECT
  +0x038 CompletionRoutine : Ptr64 long     ← driver completion callback
  +0x040 Context          : Ptr64 Void
```

> **IRP size calculation:** `sizeof(IRP) + StackCount * sizeof(IO_STACK_LOCATION)`
> = 0x80 + StackCount * 0x48

---

## 5. IRP Allocation — IoAllocateIrp

```
nt!IoAllocateIrp @ fffff800`6f444750
  → nt!IopAllocateIrpPrivate @ fffff800`6f0486e8  (normal path)
  → nt!IopAllocateIrpWithExtension @ fffff800`6f048a80  (if PpmPolicyConfigTable+0xCC4 == 2)
  → nt!IovAllocateIrp @ fffff800`6f8bb0a0  (if Driver Verifier active)
```

### IoAllocateIrp Logic:

```asm
; IoAllocateIrp(StackSize, ChargeQuota)
; x0 = StackSize (sxtb → signed byte)
; x1 = ChargeQuota (uxtb → unsigned byte)

uxtb  w2,w1                      ; ChargeQuota
ldr   w8,[PpmPolicyConfigTable+0xCC4]  ; check policy
cbnz  w8, extended_path          ; if non-zero, extended allocation
sxtb  w1,w0                      ; sign-extend StackSize
mov   x0,#0                      ; no owner object
bl    IopAllocateIrpPrivate      ; main allocator
ret
```

### IopAllocateIrpPrivate — Size Calculation:

```asm
; IRP size = 0x48 (base) + StackCount * 0x48 (stack locations)
uxth  x8,w21                     ; StackCount zero-extended
mov   x26,#0x48
mul   w8,w8,w26                  ; StackCount * 0x48

; Counter increment for IRP tracking (perf counters):
mov   x9,#0x2094                 ; counter index
add   x12,x9,w21,sxtw           ; per-stackcount counter
ldr   w9,[xpr+offset,x12 lsl #2]
add   w9,w9,#1
str   w9,[xpr+offset,x12 lsl #2]

; Limit check against MaxStackCount:
ldr   w9,[IopNotifyLastChanceShutdownQueueHead+0x80]
cmp   w10,w9                     ; if stack count > max → allocation failure
bhi   allocation_failed
```

---

## 6. Async I/O — ReadFileEx / WriteFileEx

### 6.1 NtReadFile

```
nt!NtReadFile @ fffff800`6fa3b590
```

Key flow:
```asm
; NtReadFile(FileHandle, EVENT, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key)
mov   x24,x3                     ; ApcRoutine
mov   x25,x2                     ; EVENT (NULL for async APC)
mov   x26,x1                     ; EVENT param
mov   x23,x4                     ; ApcContext
mov   x22,x5                     ; Buffer
mov   x20,x7                     ; Key
mov   w1,#1                      ; DesiredAccess = FILE_READ_DATA
mov   x5,#0                      ; HandleType = 0
add   x4,sp,#0x40                ; &object (output)
bl    ObReferenceObjectByHandle  ; resolve handle → FILE_OBJECT
ldr   x19,[sp,#0x40]             ; x19 = FILE_OBJECT

; Check if file has associated completion:
ldr   x8,[x19,#0xD0]             ; FILE_OBJECT+0xD0 (CompletionInfo)
cbz   x8, no_completion          ; if NULL, skip
ldr   w8,[x8]
tbnz  w8,#2, completion_path     ; if bit 2 set → completion path
```

### 6.2 NtWriteFile

```
nt!NtWriteFile @ fffff800`6fa3b8c0
```

Similar to NtReadFile but:
- Uses different counter indices for write tracking
- Checks `FILE_OBJECT+0xB0` → `+0x2F0` for write-specific state
- Checks bit 0x1A at `+0x1E4` (possibly write-through flag)

### 6.3 Async Completion Path

For `ReadFileEx`/`WriteFileEx`, the APC mechanism:
1. IRP allocated with `IoAllocateIrp`
2. `IRP+0x048 UserIosb` → user's IO_STATUS_BLOCK
3. `IRP+0x058 Overlay.AsynchronousParameters` stores:
   - User APC routine (from `ReadFileEx` callback)
   - User APC context
4. On I/O completion, kernel queues user-mode APC
5. APC fires in user mode → calls the `ReadFileEx`/`WriteFileEx` callback

---

## 7. IRP Queuing — IopQueueIrpToFileObject

```
nt!IopQueueIrpToFileObject @ fffff800`6f4434c8
```

```asm
; IopQueueIrpToFileObject(Irp, FileObject, Synchronous)
; x0 = IRP, x1 = FILE_OBJECT, x2 = synchronous flag

; Acquire spinlock at FILE_OBJECT+0xB0
add   x0,x19,#0xB8          ; FILE_OBJECT+0xB8 = IrpListLock (NOTE: +0xB8 in this build)
uxtb  w22,w2                 ; synchronous flag
bl    KeAcquireSpinLockRaiseToDpc

; Check FILE_OBJECT+0x50 flags
ldr   w8,[x19,#0x50]         ; FILE_OBJECT+0x50 = Flags
tbnz  w8,#0xA,...            ; check bit 10 (FO_HANDLE_CREATED?)

; Insert IRP into FILE_OBJECT list
; List head at FILE_OBJECT+0xC0 (IrpList)
; IRP queue tag: 0x6F49, 0x7043 ("IoIp" / "CpIo")

; Release spinlock
bl    KeReleaseSpinLock
```

> **Key:** IRP list is protected by `KSPIN_LOCK` at `FILE_OBJECT+0xB0/0xB8`.
> DPC-level IRQL required. This is why `CancelIo` must raise IRQL.

---

## 8. CancelIo / CancelIoEx

### 8.1 CancelIo → NtCancelIoFile

```
nt!NtCancelIoFile @ fffff800`6fa30f80
```

```asm
; NtCancelIoFile(FileHandle, IoStatusBlock)
mov   x20,x1                  ; IoStatusBlock
mov   w21,#0                  ; cancel all IRPs flag
; ...
bl    IopReferenceFileObject  ; resolve handle → FILE_OBJECT

; Increment I/O operation counter:
ldr   x8,[x9,#0x3B0]
add   x8,x8,#1
str   x8,[x9,#0x3B0]

; Raise IRQL to DISPATCH_LEVEL:
mov   w0,#1                   ; DISPATCH_LEVEL
bl    KfRaiseIrql
uxtb  w23,w0                  ; save old IRQL

; Call IopCompleteIrpInFileObjectList to cancel all IRPs:
add   x25,x19,#0x520          ; thread IRP list
ldr   x22,[x19,#0x520]        ; first IRP
```

### 8.2 CancelIoEx → NtCancelIoFileEx

```
nt!NtCancelIoFileEx @ fffff800`6fa31120
```

```asm
; NtCancelIoFileEx(FileHandle, IoStatusBlockToCancel)
; Additional: can cancel specific IRP by its IOSB pointer

; Validate IOSB pointer is in user range:
mov   x8,#0x7FFFFFFF0000
cmp   x21,x8
csel  x9,x21,x8,lo           ; clamp to user-space range
ldr   w8,[x9]                 ; probe read
str   w8,[x9]                 ; probe write (verify writable)
```

> **Difference:** `CancelIo` cancels ALL pending I/O for the file.
> `CancelIoEx` can target a specific IOSB, canceling only one operation.

### 8.3 IopCompleteIrpInFileObjectList

```
nt!IopCompleteIrpInFileObjectList @ fffff800`6f441640
```

```asm
; Walk FILE_OBJECT IrpList and complete each IRP
ldrb  w8,[x19,#0x41]          ; FILE_OBJECT+0x41 byte flag
cbz   w8, skip

ldr   x8,[x1,#0xB0]           ; FILE_OBJECT+0xB0
ldr   w9,[x19,#0x10]          ; FILE_OBJECT+0x10 (IO priority bits)
and   w8,w9,#0x50             ; mask priority bits
cmp   w8,#0x50
beq   high_priority_path

; Complete each IRP:
bl    IopCompleteRequest @ fffff800`6f4419a0
```

---

## 9. I/O Priority — SetFileInformationByHandle

### 9.1 IoSetIoPriorityHint

```
nt!IoSetIoPriorityHint @ fffff800`6f447920
```

```asm
; IoSetIoPriorityHint(FileObject, PriorityHint)
; x0 = FILE_OBJECT, w1 = IO_PRIORITY_HINT (0-4)

cmp   w1,#5                   ; validate: must be < 5
bhs   invalid_parameter

ldr   w8,[x0,#0x10]           ; FILE_OBJECT+0x10 (Flags + priority)
add   w9,w1,#1                ; PriorityHint + 1 (0→1, 1→2, ..., 4→5)
and   w8,w8,#0xFFF1FFFF       ; clear bits 16-18 (mask priority field)
orr   w8,w8,w9,lsl #0x11      ; set new priority in bits 16-18
str   w8,[x0,#0x10]           ; write back

mov   w0,#0                   ; STATUS_SUCCESS
ret
```

> **Priority stored as (PriorityHint + 1) in bits 16-18 of FILE_OBJECT+0x10**
> IO_PRIORITY_HINT values: VeryLow(0), Low(1), Normal(2), High(3), Critical(4)
> Stored: 1=VeryLow, 2=Low, 3=Normal, 4=High, 5=Critical
> 0 in bits 16-18 = no priority set → defaults to Normal

### 9.2 IoGetIoPriorityHint

```
nt!IoGetIoPriorityHint @ fffff800`6f445fb0
```

```asm
; x0 = FILE_OBJECT
mov   x8,x0
ldr   w9,[x8,#0x10]           ; FILE_OBJECT+0x10
ubfx  w9,w9,#0x11,#3          ; extract bits 16-18 (3 bits)
cbnz  w9, has_priority
mov   w0,#2                   ; default = IoPriorityNormal (2)
ret

has_priority:
sub   w0,w9,#1                ; stored_value - 1 = PriorityHint
cmp   w0,#2                   ; if >= 2 (Normal or higher)
bge   return_normal           ; return Normal

; Check if lower device has override priority:
ldr   x8,[x8,#0x98]           ; FILE_OBJECT+0x98 (lower device?)
cbz   x8, return_normal
ldr   w8,[x8,#0x5C0]          ; device priority override
cbz   w8, return_normal
b     return_normal           ; (original path continues)
```

> **Default is Normal (2)** if no priority explicitly set.

### 9.3 NtSetInformationFile — FileIoPriorityHintInfo

```
nt!NtSetInformationFile @ fffff800`6f0509c0
```

```asm
; Giant switch on FileInformationClass:
; w22 = FileInformationClass
cmp   w22,#0x54               ; 84 classes total
bhs   reject                  ; if class >= 0x54, reject

; FileIoPriorityHintInfo = class 0x0D (13)
; Dispatches to appropriate handler which calls IoSetIoPriorityHint
```

---

## 10. DeviceIoControl / FSCTL — IopXxxControlFile

### 10.1 Shared Dispatcher

Both `NtDeviceIoControlFile` and `NtFsControlFile` call the **same** function:

```
NtDeviceIoControlFile → IopXxxControlFile(flag=1)
NtFsControlFile       → IopXxxControlFile(flag=0)
```

### 10.2 NtFsControlFile

```
nt!NtFsControlFile @ fffff800`6fa376e0
```

```asm
; Thin wrapper — sets flag byte to 0 (FSCTL)
strb  wzr,[sp,#0x10]          ; flag byte = 0 → FSCTL path
str   w8,[sp,#8]              ; FsControlCode
str   x8,[sp]                 ; InputBuffer
bl    nt!IopXxxControlFile    ; shared dispatcher
```

### 10.3 IopXxxControlFile

```
nt!IopXxxControlFile @ fffff800`6fa2b530
```

```asm
; Very large function (~0x260 bytes stack frame)
sub   sp,sp,#0x260            ; 608 bytes of local storage

; Store all parameters:
str   x2,[x26,#0x88]          ; OutputBuffer
stp   x1,x3,[x26,#0xA8]      ; ApcRoutine, ApcContext
str   x3,[x26,#0x110]         ; ApcContext (again)
str   w5,[x26,#4]             ; IoControlCode
str   w5,[x26,#0xC8]          ; IoControlCode (copy)
str   w7,[x26,#0xC]           ; OutputBufferLength

; Read flag byte (0=FSCTL, 1=IOCTL):
ldrb  w8,[x26,#0x2D0]         ; flag from NtFsControlFile/NtDeviceIoControlFile
strb  w8,[x26,#2]             ; store as local flag
strb  w8,[x26,#0x1A]          ; store as local flag (copy)

; Allocate IRP, setup IO_STACK_LOCATION with:
;   MajorFunction = IRP_MJ_DEVICE_CONTROL (0x0E) or IRP_MJ_FILE_SYSTEM_CONTROL (0x0D)
;   Parameters.DeviceIoControl.IoControlCode = control code
;   Parameters.DeviceIoControl.InputBufferLength
;   Parameters.DeviceIoControl.OutputBufferLength
```

> **IOCTL vs FSCTL** distinguished only by the flag byte:
> - Flag=1 → `IRP_MJ_DEVICE_CONTROL` (0x0E)
> - Flag=0 → `IRP_MJ_FILE_SYSTEM_CONTROL` (0x0D)

---

## 11. SetFileIoOverlappedRange — IopSetFileObjectIosbRange

```
nt!IopSetFileObjectIosbRange @ fffff800`6ec89f28
```

```asm
; Validates range parameters for overlapped I/O tracking
; Uses FastMutex for synchronization (ExAcquireFastMutex)
; AVL table (RtlLookupElementGenericTableAvl) for range storage

; FILE_OBJECT+0x18 → range count
; FILE_OBJECT+0x10 → range pointer
```

### IopCleanupFileObjectIosbRange

```
nt!IopCleanupFileObjectIosbRange @ fffff800`6ec87f88
```

```asm
; Called when file handle is closed
; Uses ExAcquireFastMutex (not spinlock — can sleep)
; RtlLookupElementGenericTableAvl for range lookup
; FILE_OBJECT+0x20 dereferenced via ObfDereferenceObjectWithTag
; FILE_OBJECT+0x10 compared against range entries
```

> **Key:** Overlapped range tracking uses **AVL tree + FastMutex**, not spinlock.
> This is because cleanup can happen at PASSIVE_LEVEL (handle close).

---

## 12. Synchronous I/O Completion — IopSynchronousServiceTail

```
nt!IopSynchronousServiceTail @ fffff800`6fa2a840
```

```asm
; Called by NtReadFile/NtWriteFile after IRP is submitted
; Handles synchronous vs asynchronous completion

; Parameters:
;   x0 = IRP
;   x1 = FILE_OBJECT (stored as x19)
;   x2 = some object (x20)
;   w3 = synchronous flag (w27)
;   w4 = flags byte (stored [sp+0x10])
;   w5 = deferred flag (w21)
;   w6 = additional flags (w25)

; Check FILE_OBJECT flags:
ldr   x8,[x19,#0x58]          ; Flags from RelatedFileObject?
strb  w4,[sp,#0x10]
str   xzr,[sp,#0x14]
str   x20,[sp,#0x20]
add   x10,x19,#0x47           ; FILE_OBJECT+0x47 (AllocationFlags)
uxtb  w27,w3                  ; synchronous flag
uxtb  w21,w5                  ; deferred flag

; If pending I/O, queue IRP to file object:
tbz   w8,#0, check_sync       ; bit 0 = FO_SYNCHRONOUS_IO
ldrb  w9,[x19,#0x47]
and   x8,x8,#-2               ; clear bit
str   x8,[x19,#0x58]
orr   w9,w9,#0x10             ; set pending flag
strb  w9,[x19,#0x47]

; Check IO priority hint:
ldr   w9,[x19,#0x10]          ; FILE_OBJECT+0x10
ldr   x26,[x20,#0xB0]         ; related object
and   w24,w9,#0x200000        ; mask for FO_WAIT_MODE bit

; Queue IRP to file object if conditions met:
cbz   w21, skip_queue
cbnz  w8, queue_irp
cbz   w24, async_path

; Queue IRP for synchronous tracking:
mov   w2,#1                   ; synchronous = true
mov   x1,x20                  ; file object
mov   x0,x19                  ; IRP
bl    IopQueueIrpToFileObject ; queue IRP
uxtb  w8,w0                   ; check result
cbz   w8, queue_thread
b     wait_path

; Queue to thread IRP list:
mov   x0,x19                  ; IRP
bl    IopQueueThreadIrp       ; @ fffff800`6f443670
```

> **Flow:** Synchronous I/O → queue IRP to FILE_OBJECT → queue to Thread → wait for completion
> Async I/O → IRP not queued, APC fires on completion

---

## 13. GetOverlappedResult → NtWaitForSingleObject

```
nt!NtWaitForSingleObject @ fffff800`6faa37d0
```

```asm
; NtWaitForSingleObject(Handle, Alertable, Timeout)
uxtb  w3,w1                   ; Alertable
str   xzr,[fp,#0x10]          ; local IOSB
ldr   x8,[xpr,#0x988]
ldrsb w1,[x8,#0x252]          ; PreviousMode

; If timeout == NULL, skip timeout validation:
cbnz  x2, validate_timeout
mov   x4,x2                   ; Timeout = NULL
mov   w2,w1                   ; PreviousMode
bl    ObWaitForSingleObject   ; @ fffff800`6faa3f30
```

> `GetOverlappedResult` calls `NtWaitForSingleObject` on the event handle
> from the OVERLAPPED structure. The kernel calls `ObWaitForSingleObject`.
> This is a simple wait — no special I/O kernel paths involved.

---

## 14. NtLockFile

```
nt!NtLockFile @ fffff800`6fa37720
```

Minor API — also thin wrapper through IopXxxControlFile with IRP_MJ_LOCK_CONTROL.

---

## 15. Key Findings Summary

1. **NtDeviceIoControlFile and NtFsControlFile share IopXxxControlFile** — distinguished by a single flag byte (0=FSCTL, 1=IOCTL)

2. **IO Priority stored in FILE_OBJECT+0x10 bits 16-18** — as (PriorityHint + 1), default 0 → Normal(2)

3. **IRP queuing uses KSPIN_LOCK at FILE_OBJECT+0xB0/0xB8** — DPC-level IRQL required

4. **Overlapped range tracking uses AVL tree + FastMutex** — not spinlock, because cleanup is PASSIVE_LEVEL

5. **IRP size = 0x80 + StackCount * 0x48** — IopAllocateIrpPrivate calculates this

6. **IopSynchronousServiceTail** decides sync vs async: queues IRP to FILE_OBJECT for sync, skips for async (APC path)

7. **CancelIo cancels ALL IRPs** for a file; CancelIoEx can target specific IOSB

8. **GetOverlappedResult is trivial** — just NtWaitForSingleObject on the event handle

9. **NtSetInformationFile has 84 information classes** (< 0x54) — giant switch dispatch

10. **FILE_FLAG_OVERLAPPED is inverted** — it CLEARS FO_SYNCHRONOUS_IO, not sets a flag
