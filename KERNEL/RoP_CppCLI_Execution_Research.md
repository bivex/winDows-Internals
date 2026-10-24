# C++/CLI Mixed-Mode Execution Security Surface Analysis

**Date:** 2026-10-24  
**Environment:** Windows 11 Pro ARM64 (Build 26100.1, 24H2), Apple Silicon Parallels Desktop 20.2.2  
**Compiler:** Microsoft (R) C/C++ Optimizing Compiler Version `19.51.36246` for ARM64 (`/clr`)  
**Debugger:** `cdb.exe` ARM64 (DbgEng `10.0.26100.6584`)  
**Target Binary:** `C:\test_cppcli\MixedLib.dll` (Mixed-Mode PE, ARM64)  
**Target File:** `/Volumes/External/Code/winDows-Internals/KERNEL/RoP_CppCLI_Execution_Research.md`  

---

## 1. Executive Summary: Why C++/CLI Expands the Attack Surface

C++/CLI mixed-mode assemblies introduce security-relevant architectural properties that pure native or pure managed binaries do not exhibit. This research empirically maps three distinct classes of writable function pointers and executable memory regions that the CLR provisions to support the IJW ("It Just Works") architecture:

1. **RWX (Read-Write-Execute) Heap for IJW Thunks:** The CLR allocates private memory with `PAGE_EXECUTE_READWRITE` protection to store dynamically generated transition thunks. This violates the W^X (Write-Xor-Execute) security policy.

2. **Writable VTableFixup Function Pointer Slots:** The `VTableFixups` directory points to function pointer arrays in the `.data` section (`PAGE_READWRITE`). Every managed-to-native crossover dereferences these writable slots and performs an indirect branch.

3. **Absence of Control Flow Guard (CFG):** MSVC `/clr` compilation does not emit `IMAGE_DLLCHARACTERISTICS_GUARD_CF` in the PE optional header. Indirect branch instructions (`br x9`) in mixed-mode assemblies are **not validated** by the kernel's CFG dispatch table.

```
SECURITY-RELEVANT MEMORY MAP OF C++/CLI PROCESS:

┌─────────────────────────────────────────────────────────────────────────┐
│  ADDRESS RANGE           │ PROTECTION            │ CONTENTS             │
├─────────────────────────────────────────────────────────────────────────┤
│  00007ff8`1a4b1000      │ PAGE_EXECUTE_READ     │ .text (Native code) │
│  00007ff8`1a4ba000      │ PAGE_READWRITE    ◄── │ .data (VTableFixup)│
│                         │                      │   Writable pointers │
│  00000277`be500000      │ PAGE_EXECUTE_     ◄── │ CLR Thunk Heap      │
│                         │   READWRITE           │   IJW stubs + data  │
│  0000010d`30ee0000      │ PAGE_READWRITE        │ CLR Loader Data     │
│  00007fff`8af10020      │ PAGE_EXECUTE_READWRITE│ JIT Code Heap       │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. RWX Memory: The CLR Executable Thunk Heap

### 2.1 Empirical Discovery via `!address`

Querying the memory region containing the IJW transition thunk pointer (stored in VTableFixup slot `MixedLib+0xA000`):

```text
0:000> !address poi(MixedLib+0xA000)

Usage:                  <unknown>
Base Address:           00000277`be500000
End Address:            00000277`be503000
Region Size:            00000000`00003000 (  12.000 kB)
State:                  00001000          MEM_COMMIT
Protect:                00000040          PAGE_EXECUTE_READWRITE  ◄── RWX!
Type:                   00020000          MEM_PRIVATE
Allocation Base:        00000277`be000000
Allocation Protect:     00000004          PAGE_READWRITE
```

### 2.2 Security Implications of RWX Thunk Memory:

| Property | Value | Security Impact |
|---|---|---|
| **Protection** | `PAGE_EXECUTE_READWRITE` (0x40) | Violates W^X. Same page is simultaneously writable and executable. |
| **Type** | `MEM_PRIVATE` | Allocated via `VirtualAlloc`, not mapped from a DLL file. |
| **Size** | 12 KB (0x3000) | Contains multiple thunk slots at 0x30-byte intervals. |
| **Allocation Base** | `00000277be000000` | Originally allocated as `PAGE_READWRITE`, then `VirtualProtect` to RWX. |

**Attack Surface:** If an attacker obtains an arbitrary write primitive targeting any address within this 12 KB region, they can:
- Overwrite the 16-byte machine code stub of any IJW thunk.
- Modify the embedded 32-byte descriptor packet (handler address, module base, MethodDef token).
- Redirect all subsequent managed-to-native crossovers through the compromised thunk.

### 2.3 Thunk Layout at RWX Addresses:

```asm
; RWX Region at 00000277`be502058:
00000277`be502058 10000010 adr         xip0, 00000277`be502058  ; Self-reference
00000277`be50205c 58000071 ldr         xip1, 00000277`be502068  ; Load handler addr
00000277`be502060 d61f0220 br          xip1                     ; Indirect branch!
00000277`be502064 d503201f nop

; Embedded Descriptor (also writable):
00000277`be502068: 00007ff8`0ecf10c0  ; Handler: mscoreei!VTableBootstrapThunkInitHelperStub
00000277`be502070: 00007ff8`1a4b0000  ; Module Base Address
00000277`be502078: 00007ff8`1a4ba000  ; VTableFixup slot to back-patch
00000277`be502080: 00000000`06000002  ; MethodDef token #2
```

---

## 3. VTableFixup Slots: Writable Indirect Branch Targets

### 3.1 Memory Protection of the Fixup Table

```text
0:000> !address MixedLib+0xA000

Usage:                  Image
Base Address:           00007ff8`1a4ba000
End Address:            00007ff8`1a4bb000
Region Size:            00000000`00001000 (   4.000 kB)
State:                  00001000          MEM_COMMIT
Protect:                00000004          PAGE_READWRITE  ◄── WRITABLE!
Type:                   01000000          MEM_IMAGE
Allocation Base:        00007ff8`1a4b0000
Allocation Protect:     00000080          PAGE_EXECUTE_WRITECOPY
Image Path:             C:\test_cppcli\MixedLib.dll
```

The VTableFixup slot resides in the module's `.data` section. Although the overall image allocation uses `PAGE_EXECUTE_WRITECOPY`, the `.data` sub-region is explicitly `PAGE_READWRITE` because the CLR **must** write the JIT-resolved function addresses into these slots during back-patching.

### 3.2 Unprotected Indirect Branch Path

The native export `NativeCallsManaged` performs:
```asm
; MixedLib!NativeCallsManaged:
00007ff8`1a4b1238 b0000049 adrp    x9, MixedLib+0xA000   ; Compute slot address
00007ff8`1a4b123c f9400129 ldr     x9, [x9]              ; Read pointer from WRITABLE memory
00007ff8`1a4b1240 d61f0120 br      x9                    ; Indirect branch - NO VALIDATION!
```

**Key Observation:** The `br x9` instruction performs an **unvalidated indirect branch**. The target address is loaded from a `PAGE_READWRITE` page. If any write primitive modifies the slot value, the next call to `NativeCallsManaged` will redirect execution to the attacker-controlled address.

### 3.3 Control Flow Hijack Scenario (Theoretical):

```
Normal Flow:
  NativeCallsManaged() 
    → [0x1a4ba000] = 0x00000277be502058 (IJW Thunk)
    → br 0x00000277be502058 
    → mscoreei!VTableBootstrapThunkInitHelperStub
    → clr!IJWNOADThunk::FindThunkTarget
    → JIT Compile ManagedClass::Compute
    → Back-patch [0x1a4ba000] = JIT address
    → Execute managed method

Hijacked Flow (requires arbitrary write to 0x1a4ba000):
  NativeCallsManaged()
    → [0x1a4ba000] = ATTACKER_CONTROLLED_ADDRESS (written by exploit)
    → br ATTACKER_CONTROLLED_ADDRESS
    → Execution redirected without any CFG check
```

---

## 4. Control Flow Guard (CFG) Absence in Mixed-Mode PE

### 4.1 Empirical DllCharacteristics Extraction:

```text
DllCharacteristics: 0x0160
  DYNAMIC_BASE (ASLR):     True   (bit 0x0040 SET)
  NX_COMPAT (DEP):         True   (bit 0x0100 SET)
  HIGH_ENTROPY_VA:         True   (bit 0x0020 SET)
  CONTROL_FLOW_GUARD:      False  (bit 0x4000 NOT SET)  ◄── MISSING!
  GUARD_CF:                False  (bit 0x8000 NOT SET)  ◄── MISSING!
```

### 4.2 Implication of Missing CFG:

| CFG Mechanism | Pure Native (`cl /EHsc`) | Mixed-Mode (`cl /clr`) |
|---|---|---|
| Indirect call validation (`_guard_dispatch_icall`) | Present (if `/guard:cf`) | **ABSENT** |
| Valid call target bitmap (`GFIDS table`) | Present | **NOT EMITTED** |
| Kernel `ProcessMitigationPolicy` enforcement | Applicable | **Ineffective for this module** |
| Stack pivot detection | Present | **Ineffective** |

Without CFG, the `br x9` instruction at `0x1a4b1240` will branch to **any address** without kernel or user-mode dispatch verification. This includes:
- Stack addresses (enabling stack pivot techniques).
- Heap addresses containing ROP chains.
- Addresses within the RWX thunk heap.
- Addresses pointing to unaligned gadget sequences in the middle of legitimate functions.

---

## 5. ARM64 Gadget Surface in Mixed-Mode Native Sections

### 5.1 Section Layout:

```text
Section .text: RVA 0x00001000 - 0x000032DC  (8,924 bytes of native ARM64 code)
Section .rdata: RVA 0x00004000 - 0x0000924A (Read-only data)
Section .data: RVA 0x0000A000 - 0x0000A2E0 (Writable data + VTableFixup)
Section .pdata: RVA 0x0000B000 - 0x0000B158 (Exception unwind info)
Section .reloc: RVA 0x0000C000 - 0x0000C02C (Relocations)
```

### 5.2 Function Pointer Indirection Gadgets:

The compiled native code within `MixedLib.dll` contains multiple instances of the `adrp + ldr + br` sequence that read targets from writable memory:

```asm
; Gadget Pattern 1: VTableFixup Crossover (at NativeCallsManaged entry):
adrp  x9, [writable .data address]
ldr   x9, [x9]
br    x9

; Gadget Pattern 2: IAT Indirection:
adrp  x8, [_imp_GetCurrentThreadId]
ldr   x8, [x8]
br    x8
```

### 5.3 CIL-Level Indirect Calls:

At the managed CIL level, every interface method invocation uses an indirect dispatch through the `MethodTable` vtable slot or interface dispatch map. If the `MethodTable` pointer in an object header can be corrupted, the subsequent `ldrb`/`ldr`/`blr` sequence will branch through attacker-controlled data.

---

## 6. Mitigation Landscape for C++/CLI

### 6.1 Currently Available Defenses:

| Mitigation | Status | Effectiveness for Mixed-Mode |
|---|---|---|
| **DEP / NX** | Enabled (`NX_COMPAT`) | Prevents execution from stack/heap data pages, but does not protect against the RWX thunk heap or code reuse. |
| **ASLR** | Enabled (`DYNAMIC_BASE` + `HIGH_ENTROPY_VA`) | Randomizes module bases, but infoleak of any pointer defeats it. |
| **CFG** | **NOT ENABLED** | Indirect branches in the mixed assembly are not validated. |
| **XFG** | **NOT ENABLED** | Type-based CFG is not compiled in. |
| **CET / BTI (ARM64 BTI)** | Unknown | ARM64 Branch Target Identification requires hardware and compiler support. |

### 6.2 Architectural Recommendations:

1. **Enable `/guard:cf` in C++/CLI builds:** Even though the CLR's internal thunks bypass CFG, the native code within the mixed assembly can still be compiled with valid call target enumeration.
2. **Reduce writable function pointer surface:** Consider runtime patching of VTableFixup slots to `PAGE_READONLY` after JIT resolution.
3. **Monitor RWX allocations:** Security tools should flag the CLR's thunk heap allocation (`PAGE_EXECUTE_READWRITE`) as anomalous in processes that should not generate dynamic code.
4. **Validate thunk descriptors:** Integrity checking of the embedded descriptor packets could detect tampering with handler addresses or MethodDef tokens.

---

## 7. Reproducibility & Falsification Matrix

| Claim ID | Technical Claim | Status | Target Configuration | Verification Command | Raw Empirical Observation | Falsification Condition |
|---|---|---|---|---|---|---|
| **ROP-01** | **IJW Transition Thunks reside in RWX (`PAGE_EXECUTE_READWRITE`) private memory** | `[EXPERIMENTALLY CONFIRMED]` | `MixedExe.exe` under `cdb.exe` ARM64 | `!address poi(MixedLib+0xA000)` | `Protect: 00000040 PAGE_EXECUTE_READWRITE, Type: MEM_PRIVATE, Size: 12KB` | Thunk memory allocated as `PAGE_EXECUTE_READ` (non-writable) after creation. |
| **ROP-02** | **VTableFixup slots reside in `PAGE_READWRITE` image section (.data)** | `[EXPERIMENTALLY CONFIRMED]` | `MixedLib.dll` under `cdb.exe` | `!address MixedLib+0xA000` | `Protect: 00000004 PAGE_READWRITE, Type: MEM_IMAGE, Image Path: MixedLib.dll` | Fixup table mapped as read-only after JIT resolution. |
| **ROP-03** | **C++/CLI PE does not set `IMAGE_DLLCHARACTERISTICS_GUARD_CF`** | `[EXPERIMENTALLY CONFIRMED]` | `MixedLib.dll` binary header | PowerShell PE parser for `DllCharacteristics` | `DllCharacteristics: 0x0160` (no bit `0x4000` or `0x8000`) | CFG flag present in mixed-mode compilation. |
| **ROP-04** | **The `br x9` indirect branch reads from writable memory without validation** | `[EXPERIMENTALLY CONFIRMED]` | Disassembly of `MixedLib!NativeCallsManaged` | `u MixedLib!NativeCallsManaged L3` | `adrp x9, MixedLib+0xA000; ldr x9, [x9]; br x9` preceded by no `__guard_dispatch_icall` check | Insertion of CFG validation call before indirect branch. |
| **ROP-05** | **IJW Thunk contains embedded descriptor with handler address in RWX memory** | `[EXPERIMENTALLY CONFIRMED]` | Dump of RWX region at thunk address | `dp poi(MixedLib+0xA000) L8` | `00007ff8'0ecf10c0` (handler), `00007ff8'1a4b0000` (base), `00007ff8'1a4ba000` (slot), `06000002` (token) | Descriptor stored in read-only memory or encrypted. |
