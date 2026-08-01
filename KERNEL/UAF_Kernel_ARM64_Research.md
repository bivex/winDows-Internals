# Use-After-Free (UAF) in Windows Kernel & ARM64 Architecture — Research & Analysis

**Author:** Antigravity AI & Kernel Security Research  
**Target Environment:** Windows 11 ARM64 (`ntkrnlmp.exe` / Build 26100.1)  
**Scope:** Object Lifetime Management, Kernel Pool Allocator, ARM64 Hardware Security (PAC & MTE)

---

## 1. Executive Summary: What is Use-After-Free (UAF)?

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

## 3. Windows Kernel Object Lifetime Management (`_OBJECT_HEADER`)

In the Windows 11 kernel (`ntoskrnl.exe`), object lifetimes are managed by the Object Manager (`nt!Ob`) via reference counting inside `_OBJECT_HEADER`:

```text
struct nt!_OBJECT_HEADER
   +0x000 PointerCount     : Int8B   (Active kernel pointer count)
   +0x008 HandleCount      : Int8B   (Active user-mode handle count)
   +0x010 Lock             : _EX_PUSH_LOCK
   +0x018 TypeIndex        : UChar   (Object Type ID: Job, Process, File)
```

### Reference Counting Mechanics
* **`ObReferenceObjectByHandle` / `ObReferenceObjectByPointer`:** Increments `PointerCount`.
* **`ObDereferenceObject`:** Decrements `PointerCount`.
* **Safe Release:** When a process closes its handle (`CloseHandle`), `HandleCount` reaches zero, but the physical memory is **NOT freed** until `PointerCount` reaches zero.
* **Kernel UAF Root Cause:** Occurs when a custom kernel driver accesses an object after `PointerCount == 0` without acquiring a valid reference, leading to a BugCheck `0x50` (`PAGE_FAULT_IN_NONPAGED_AREA`) or arbitrary code execution.

---

## 4. Kernel Pool Allocator & Modern Mitigations

### A. Kernel Segment Heap (`nt!ExAllocatePool2`)
Windows 11 replaces legacy lookaside lists with the **Kernel Segment Heap**:
* **Randomized Chunk Allocation:** Free memory blocks are not immediately re-allocated to the same size bucket, neutralizing predictable heap spraying.
* **Header Guard Telemetry:** Encrypted pool headers prevent header overwrites.

### B. Special Pool & Driver Verifier
* When Driver Verifier (`!verifier 1`) or Special Pool (`!pool`) is enabled, allocations are placed on individual page boundaries bounded by unmapped guard pages.
* Any dangling access after `ExFreePoolWithTag` instantly triggers an unhandled page fault (`BugCheck 0x50`), halting execution before exploitation can occur.

---

## 5. ARM64 Exploitation Specifics vs x86/x64

| Characteristic | x86 / x64 | ARM64 (AArch64) |
|---|---|---|
| **Instruction Length** | Variable (1 to 15 bytes) | Fixed (4 bytes, 32-bit aligned) |
| **Return Address Storage** | Stack (`EIP` / `RIP`) | Link Register (`LR` / `X30`) |
| **Program Counter** | `EIP` / `RIP` | `PC` |
| **Code Reuse Vector** | ROP (Return-Oriented Programming) | JOP (Jump-Oriented Programming) via `BR Xn` / `BLR Xn` |
| **Unaligned Gadgets** | Supported (jumping into mid-instruction) | Blocked by hardware alignment enforcement |

On ARM64, fixed 4-byte instruction alignment prevents jumping into the middle of instructions. Attackers rely instead on **Jump-Oriented Programming (JOP)** using indirect branch instructions (`BLR X19`, `BR X20`).

---

## 6. Hardware-Backed ARM64 Security Features

Modern ARM64 processors (Apple Silicon, Snapdragon 8 Gen 2/3, ARMv8.3+ / ARMv9) incorporate hardware-level UAF mitigations directly on silicon:

```
+-------------------------------------------------------------------+
|               ARM64 Silicon Security Layer                        |
+-------------------------------------------------------------------+
|  1. PAC (Pointer Authentication Code) -> Validates FunctionPtrs   |
|  2. MTE (Memory Tagging Extension)    -> Traps Stale Heap Tags    |
+-------------------------------------------------------------------+
```

### A. Pointer Authentication Code (PAC — ARMv8.3+)
* **Mechanism:** PAC uses upper unused bits of 64-bit virtual addresses to embed a cryptographic signature generated via hardware keys (`APIA`, `APIB`, `APDA`, `APDB`).
* **UAF Impact:** When a function pointer in a `vtable` or object header is signed (`PACIA`), any attempt by an attacker to overwrite the pointer with forged data invalidates the signature.
* **Enforcement:** Before branching (`BLRAA` / `AUTIA`), the CPU verifies the signature. Mismatched signatures trigger an immediate hardware exception trap (`Kernel Panic` / Crash).

### B. Memory Tagging Extension (MTE — ARMv8.5+ / ARMv9)
* **Mechanism:** The allocator assigns a 4-bit "tag" (key) to every allocated memory block and embeds the matching tag into bits `[59:56]` of the pointer.
* **On `free()`:** The allocator changes the memory block tag to a new random value.
* **UAF Defense:** The dangling pointer retains the *old* 4-bit tag. Upon any attempt to dereference the dangling pointer, the CPU hardware compares the pointer tag against the memory tag. Mismatches instantly trigger a hardware memory safety fault.

---

## 7. Useful WinDbg Commands for UAF & Pool Investigation

```text
!pool <Address>          ; Analyze pool header and allocation tag
!object <Address>        ; Inspect _OBJECT_HEADER PointerCount and HandleCount
dt nt!_OBJECT_HEADER     ; Display field offsets of object header
!gflag +ptg              ; Enable Page Heap / Special Pool tracing
!verifier 1              ; Enable Driver Verifier pool checks
```
