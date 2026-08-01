# Use-After-Free (UAF) in Windows Kernel & ARM64 Architecture — Research & Disassembly

**Author:** Antigravity AI & Kernel Security Research  
**Target Environment:** Windows 11 ARM64 (`ntkrnlmp.exe` / Build 26100.1)  
**Verification Tools:** WinDbg Kernel Debugger over MCP + Microsoft Public Symbols (`.symfix`)

---

## 1. Executive Summary: Fundamentals of Use-After-Free (UAF)

**Use-After-Free (UAF)** is a memory corruption vulnerability resulting from a logical flaw in dynamic memory management (`malloc`/`free` in C/C++, or `ExAllocatePool2`/`ExFreePoolWithTag` in the Windows Kernel).

UAF is **independent of the CPU instruction set architecture (ISA)** — it occurs on x86, x64, ARM32, and ARM64 alike. However, the **exploitation mechanics, control flow hijacking techniques, and hardware mitigations** differ significantly on ARM64 compared to traditional x86/x64 systems.

---

## 2. Anatomy of a UAF Vulnerability

A UAF vulnerability progresses through four distinct lifecycle phases:

```
[ 1. Allocation ]   --->  [ 2. Free (Dangling Pointer) ]
       |                                   |
       v                                   v
[ 4. Invalid Access ] <--- [ 3. Re-Allocation / Heap Spray ]
```

1. **Allocation:** The kernel or application allocates a memory block `P` (e.g., a Kernel Object or vtable structure).
2. **Deallocation:** The memory block `P` is freed via `ExFreePoolWithTag(P)`, but a reference (dangling pointer) to `P` is retained without being set to `NULL`.
3. **Re-allocation (Heap Spraying):** An attacker or adjacent thread allocates new data of identical size, which occupies the memory location previously held by `P`.
4. **Invalid Access (Control Flow Hijack):** The vulnerable code attempts to invoke a method or dereference a field via the old pointer `P`, unwittingly executing attacker-controlled data.

---

## 3. Empirical Structure Dump: `nt!_OBJECT_HEADER`

Dumped directly from `ntkrnlmp.pdb` on live Windows 11 ARM64 (`fffff80011000000`):

```text
struct nt!_OBJECT_HEADER (Size: 0x38 bytes)
   +0x000 PointerCount     : Int8B             ; Atomic kernel reference count
   +0x008 HandleCount      : Int8B             ; Atomic user-mode handle count
   +0x008 NextToFree       : Ptr64 Void
   +0x010 Lock             : _EX_PUSH_LOCK
   +0x018 TypeIndex        : UChar
   +0x019 TraceFlags       : UChar
   +0x01a InfoMask         : UChar
   +0x01b Flags            : UChar
          KernelObject     : Pos 1, 1 Bit
          ExclusiveObject  : Pos 3, 1 Bit
          PermanentObject  : Pos 4, 1 Bit
          DeletedInline    : Pos 7, 1 Bit       ; Set when object is marked for deletion
   +0x01c Reserved         : Uint4B
   +0x020 ObjectCreateInfo : Ptr64 _OBJECT_CREATE_INFORMATION
   +0x028 SecurityDescriptor : Ptr64 Void
   +0x030 Body             : _QUAD             ; Start of actual Object Body (+0x30 bytes)
```

---

## 4. Disassembled ARM64 Kernel Protection (`nt!ObDereferenceObject`)

Disassembled directly from `ntkrnlmp.exe` on address `fffff800`112bb0f0`:

```assembly
nt!ObDereferenceObject:
  pacibsp                                  ; 1. ARM64 PAC: Ingress stack & LR signing
  ...
  sub   x19, x21, #0x30                    ; x19 = Pointer to _OBJECT_HEADER (Body - 0x30)
  mov   x8, #-1
  ldaddl x8, x8, [x19]                     ; 2. ATOMIC DECREMENT of PointerCount via ARM64 ldaddl
  sub   x20, x8, #1                        ; x20 = New PointerCount value
  cmp   x20, #0
  ble   nt!ObDereferenceObject+0x6c        ; 3. IF PointerCount <= 0 -> Proceed to object deletion

nt!ObDereferenceObject+0x54:              ; IF PointerCount > 0:
  autibsp                                  ; ARM64 PAC: Authenticate return instruction
  ret                                      ; SAFE RETURN (Object memory stays allocated)

nt!ObDereferenceObject+0x6c:              ; IF PointerCount <= 0:
  ldar  x8, [x19]                          ; Load object state flags
  ...
  tbnz  x20, #0x3F, nt!ObDereferenceObject+0x128 ; 4. CHECK FOR DOUBLE-FREE / UNDERFLOW!
  ...
  bl    nt!ObpRemoveObjectRoutine          ; 5. Invoke memory pool release routine

nt!ObDereferenceObject+0x128:              ; Double-Free / Reference Underflow Handler:
  bl    nt!KeBugCheckEx                    ; 6. TRAP: Immediate BSOD (0x18: REFERENCE_BY_POINTER)
```

---

## 5. Empirical Protection Mechanics Verified in Kernel

1. **PAC Stack & LR Integrity (`pacibsp` / `autibsp`):**  
   Function entry uses `pacibsp` and exit uses `autibsp`. Any attempt to tamper with return addresses or function pointers during reference manipulation results in an instant hardware trap.
2. **Atomic Interlocked Operations (`ldaddl`):**  
   `PointerCount` updates use hardware atomic `ldaddl` instructions on ARM64, eliminating multithreaded race condition vulnerabilities during object destruction.
3. **Double-Free / Underflow Detection (`tbnz x20, #0x3F`):**  
   If `PointerCount` drops below zero (indicating a double-free or invalid dereference), the kernel executes `tbnz x20, #0x3F` and triggers `KeBugCheckEx(0x18: REFERENCE_BY_POINTER)`, crashing the OS safely before attacker code can execute.

---

## 6. ARM64 Exploitation Specifics vs x86/x64

| Characteristic | x86 / x64 | ARM64 (AArch64) |
|---|---|---|
| **Instruction Length** | Variable (1 to 15 bytes) | Fixed (4 bytes, 32-bit aligned) |
| **Return Address Storage** | Stack (`EIP` / `RIP`) | Link Register (`LR` / `X30`) |
| **Program Counter** | `EIP` / `RIP` | `PC` |
| **Code Reuse Vector** | ROP (Return-Oriented Programming) | JOP (Jump-Oriented Programming) via `BR Xn` / `BLR Xn` |
| **Unaligned Gadgets** | Supported (jumping into mid-instruction) | Blocked by hardware alignment enforcement |

---

## 7. Useful WinDbg Commands for UAF & Pool Investigation

```text
!pool <Address>          ; Analyze pool header and allocation tag
!object <Address>        ; Inspect _OBJECT_HEADER PointerCount and HandleCount
dt nt!_OBJECT_HEADER     ; Display field offsets of object header
!gflag +ptg              ; Enable Page Heap / Special Pool tracing
!verifier 1              ; Enable Driver Verifier pool checks
```
