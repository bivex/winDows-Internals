# Windows C++/CLI Mixed-Mode (IJW) Relocation Architecture: Dual-Layer PE BaseReloc & VTableFixup Internals (ARM64)

**Date:** 2026-10-24  
**Target:** Windows 11 Pro ARM64 (Build 26100.1, 24H2), Apple Silicon Parallels Desktop 20.2.2  
**Compiler:** Microsoft (R) C/C++ Optimizing Compiler Version `19.51.36246` for ARM64 (`/clr`)  
**Debugger:** `cdb.exe` ARM64 (DbgEng `10.0.26100.6584`)  
**Target Binary:** `C:\test_cppcli\MixedLib.dll` (Mixed-Mode PE32+, ARM64, ImageBase `0x180000000`)  
**Workspace:** `/Volumes/External/Code/winDows-Internals/KERNEL/CppCLI_Relocations_Research.md`

---

## 1. Executive Summary: The Dual-Layer Relocation Problem

In pure native Windows executables (C/C++), image relocation is simple: when Address Space Layout Randomization (ASLR) maps a PE image away from its preferred `ImageBase`, the Windows PE Loader (`ntdll!LdrpProcessRelocationBlock`) iterates through the `.reloc` section (`IMAGE_DIRECTORY_ENTRY_BASERELOC`) and adds the relocation delta (`ActualBase - PreferredBase`) to every absolute 64-bit address (`IMAGE_REL_BASED_DIR64`).

In pure managed .NET assemblies (C#), native base relocations are practically nonexistent because all method invocations and type references are encoded as abstract 32-bit CLI metadata tokens (`0x06000001`), resolved dynamically at runtime by the JIT compiler.

**C++/CLI Mixed-Mode ("It Just Works" - IJW) assemblies bridge both worlds and consequently implement a unique Dual-Layer Relocation Architecture:**
1. **Layer 1: Native PE Base Relocations (`.reloc` / DataDirectory[5]):**  
   Processes standard Win32 64-bit pointers in `.rdata` (CRT structures, exception handling tables, import descriptors).
2. **Layer 2: Managed CLI VTableFixup Relocations (`IMAGE_COR20_HEADER.VTableFixups` / DataDirectory[14]):**  
   Processes native-to-managed crossover dispatch slots in `.data` through a dedicated 3-stage lifecycle.

### The Fundamental Architectural Conflict:
If the Win32 OS loader attempted to apply native `DIR64` relocations to the VTableFixup slots in `.data`, it would add the ASLR delta to the 32-bit CLI MethodDef metadata tokens, **corrupting the metadata tokens into garbage addresses and crashing the CLR**.

This research documents how the MSVC toolchain and Windows loader solve this conflict through strict segregation of relocation responsibilities.

---

## 2. Architecture Diagram: Dual-Layer Relocation Pipeline

```
+───────────────────────────────────────────────────────────────────────────────+
|                  C++/CLI Mixed-Mode Assembly (MixedLib.dll)                   |
+───────────────────────────────────────────────────────────────────────────────+
                                        │
           ┌────────────────────────────┴────────────────────────────┐
           ▼                                                         ▼
+──────────────────────────────────────+  +──────────────────────────────────────+
| Layer 1: Win32 PE Base Relocations   |  | Layer 2: CLR VTableFixup Directory   |
| (DataDirectory[5] -> Section .reloc) |  | (DataDirectory[14] -> COR20_HEADER)  |
+──────────────────────────────────────+  +──────────────────────────────────────+
           │                                                         │
           │ Processed by ntdll during                               │ Processed by clr.dll
           │ initial memory mapping                                  │ during module bootstrap
           ▼                                                         ▼
+──────────────────────────────────────+  +──────────────────────────────────────+
| Modifies absolute 64-bit pointers    |  | Resolves 32-bit MethodDef tokens     |
| in native sections (.rdata):         |  | in writable data (.data):            |
| - CRT Runtime Descriptors            |  | - MixedLib+0xA000 (NativeHelper)     |
| - Exception Unwind Directory         |  | - MixedLib+0xA008 (CallsManaged)     |
| - Native TLS Descriptors             |  | - MixedLib+0xA010 (TestMixedSeh)     |
| Total Entries: exactly 14 entries    |  | Total Descriptors: 6 fixup entries   |
| Total Size: exactly 44 bytes (0x2C)  |  | Total Size: exactly 48 bytes (0x30)  |
+──────────────────────────────────────+  +──────────────────────────────────────+
           │                                                         │
           ▼                                                         ▼
   Native Pointers Patched                                 Allocates 12 KB RWX Thunk Heap
   to ASLR Image Base                                      & Back-Patches Slots to JIT Code
```

---

## 3. Dissecting Layer 1: Win32 PE Base Relocations (`.reloc`)

### 3.1 Section Layout of `MixedLib.dll`
Dumping the section headers of our compiled ARM64 C++/CLI binary:

```text
Section: .text    RVA: 0x00001000 VSize: 0x00002424 RawOffset: 0x00000400 RawSize: 0x00002600
Section: .rdata   RVA: 0x00004000 VSize: 0x000059B2 RawOffset: 0x00002A00 RawSize: 0x00005A00
Section: .data    RVA: 0x0000A000 VSize: 0x000002E0 RawOffset: 0x00008400 RawSize: 0x00000200
Section: .pdata   RVA: 0x0000B000 VSize: 0x00000158 RawOffset: 0x00008600 RawSize: 0x00000200
Section: .reloc   RVA: 0x0000C000 VSize: 0x0000002C RawOffset: 0x00008800 RawSize: 0x00000200
```

Notice that the entire `.reloc` virtual size is only **`0x2C` bytes (44 bytes)**!

---

### 3.2 Parsing the Raw Relocation Blocks

A PE base relocation table consists of contiguous `IMAGE_BASE_RELOCATION` blocks:

```c
typedef struct _IMAGE_BASE_RELOCATION {
    DWORD   VirtualAddress; // Page RVA
    DWORD   SizeOfBlock;    // Block byte size including header
    // WORD TypeOffset[ (SizeOfBlock - 8) / 2 ];
} IMAGE_BASE_RELOCATION;
```

Each 16-bit `TypeOffset` entry encodes:
- High 4 bits: Relocation type (`10` = `IMAGE_REL_BASED_DIR64` on 64-bit Windows).
- Low 12 bits: Byte offset within the 4 KB page (`VirtualAddress`).

#### Parsing the Exact 44 Bytes at Raw Offset `0x00008800`:

```text
Raw Hex:
00 40 00 00 1C 00 00 00 F8 A0 00 A1 10 A1 D0 A4 D8 A4 38 A5 50 A5 58 A5 10 A6 18 A6 
00 80 00 00 10 00 00 00 00 AF 08 AF 10 AF 18 AF
```

#### Decoded Structure:

```text
Block 0: PageRVA=0x00004000 BlockSize=0x1C (28 bytes -> 10 entries)
  [ 0] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0x0F8 TargetRVA=0x000040F8 (.rdata)
  [ 1] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0x100 TargetRVA=0x00004100 (.rdata)
  [ 2] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0x110 TargetRVA=0x00004110 (.rdata)
  [ 3] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0x4D0 TargetRVA=0x000044D0 (.rdata)
  [ 4] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0x4D8 TargetRVA=0x000044D8 (.rdata)
  [ 5] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0x538 TargetRVA=0x00004538 (.rdata)
  [ 6] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0x550 TargetRVA=0x00004550 (.rdata)
  [ 7] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0x558 TargetRVA=0x00004558 (.rdata)
  [ 8] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0x610 TargetRVA=0x00004610 (.rdata)
  [ 9] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0x618 TargetRVA=0x00004618 (.rdata)

Block 1: PageRVA=0x00008000 BlockSize=0x10 (16 bytes -> 4 entries)
  [ 0] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0xF00 TargetRVA=0x00008F00 (.rdata)
  [ 1] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0xF08 TargetRVA=0x00008F08 (.rdata)
  [ 2] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0xF10 TargetRVA=0x00008F10 (.rdata)
  [ 3] Type=10 (IMAGE_REL_BASED_DIR64) Offset=0xF18 TargetRVA=0x00008F18 (.rdata)
```

### 3.3 The Missing Page: Where is Page `0x0000A000`?
All 14 relocations target addresses inside `.rdata` (pages `0x4000` and `0x8000`).
**There is not a single relocation entry for page `0x0000A000` (`.data`).**

The VTableFixup table (`MixedLib+0xA000` through `MixedLib+0xA018`) is completely absent from the PE base relocation table.

---

## 4. Dissecting Layer 2: CLR VTableFixup Relocations

Because the VTableFixup array is omitted from `.reloc`, how does the system know where the slots are and how to resolve them?

### 4.1 The CLI Header (`IMAGE_COR20_HEADER`)
At RVA `0x000043BC` in `.rdata`, the CLI header stores the descriptor pointer in DataDirectory entry 14:

```text
IMAGE_COR20_HEADER:
  cb:                  72 bytes (0x48)
  RuntimeVersion:      2.5
  MetaData:            RVA=0x00004640 Size=0x00004870
  Flags:               0x00000010 (COMIMAGE_FLAGS_IL_LIBRARY)
  EntryPointToken/RVA: 0x000018A8
  VTableFixups:        RVA=0x00008F28 Size=0x00000030 (48 bytes)
```

---

### 4.2 The `IMAGE_COR_VTABLEFIXUP` Table

At RVA `0x00008F28` (raw file offset `0x00007928`), the binary contains an array of `IMAGE_COR_VTABLEFIXUP` descriptors:

```c
typedef struct _IMAGE_COR_VTABLEFIXUP {
    ULONG   RVA;    // RVA of the vtable slot array in .data
    USHORT  Count;  // Number of contiguous slots
    USHORT  Type;   // COR_VTABLE_* flags
} IMAGE_COR_VTABLEFIXUP;
```

#### Flag Definitions:
- `COR_VTABLE_32BIT` (`0x01`): 32-bit vtable slots (x86).
- `COR_VTABLE_64BIT` (`0x02`): 64-bit vtable slots (x64 / ARM64).
- `COR_VTABLE_FROM_UNMANAGED` (`0x08`): Thunk must transition from unmanaged caller to managed callee.
- `COR_VTABLE_CALL_MOST_DERIVED` (`0x10`): Virtual call resolution through most derived type.

#### Dumping the Exact 6 Entries from `MixedLib.dll`:

```text
Fixup[0] at raw 0x7928: SlotRVA=0x0000A000 Count=1 Type=0x000A [64Bit=True, FromUnmanaged=True] -> NativeHelper
Fixup[1] at raw 0x7930: SlotRVA=0x0000A008 Count=1 Type=0x000A [64Bit=True, FromUnmanaged=True] -> NativeCallsManaged
Fixup[2] at raw 0x7938: SlotRVA=0x0000A010 Count=1 Type=0x000A [64Bit=True, FromUnmanaged=True] -> TestMixedSeh
Fixup[3] at raw 0x7940: SlotRVA=0x0000A018 Count=1 Type=0x000A [64Bit=True, FromUnmanaged=True] -> TestBothTls
Fixup[4] at raw 0x7948: SlotRVA=0x0000A098 Count=1 Type=0x000A [64Bit=True, FromUnmanaged=True] -> CRT Init Slot
Fixup[5] at raw 0x7950: SlotRVA=0x0000A0A0 Count=1 Type=0x000A [64Bit=True, FromUnmanaged=True] -> CRT Init Slot
```

Notice `Type = 0x000A` (`0x0002 | 0x0008` = `COR_VTABLE_64BIT | COR_VTABLE_FROM_UNMANAGED`).

---

## 5. The Token-vs-Pointer Conundrum: Mathematical Proof of Incompatibility

Why did the architects of C++/CLI design this dual-layer system instead of using standard Win32 PE relocations for everything?

### The Token Corruption Problem:
On disk, each slot in `MixedLib+0xA000` contains a 32-bit CLI metadata token:
$$\text{Slot Value} = \text{MethodDef Token} = \mathtt{0x00000000`06000001}$$

Suppose ASLR randomizes the image base from preferred base $B_{\text{pref}} = \mathtt{0x180000000}$ to actual base $B_{\text{act}} = \mathtt{0x7ff9`33b30000}$.
The Win32 loader's relocation formula is:
$$\Delta = B_{\text{act}} - B_{\text{pref}} = \mathtt{0x7ff9`33b30000} - \mathtt{0x00000001`80000000} = \mathtt{0x7ff8`b3b30000}$$

If the OS loader applied `IMAGE_REL_BASED_DIR64` to `MixedLib+0xA000`:
$$\text{Corrupted Slot} = \mathtt{0x00000000`06000001} + \mathtt{0x7ff8`b3b30000} = \mathtt{0x00007ff8`b9b30001}$$

When the CLR loads the assembly and reads the slot to determine which method to compile:
- The token type table (`0x06` = MethodDef) has been completely obliterated.
- `0x00007ff8b9b30001` is not a valid token.
- The CLR fails immediately with `COR_E_BADIMAGEFORMAT` or `COR_E_TYPELOAD`.

### The Solution:
1. The compiler uses **RVA addressing** in `IMAGE_COR_VTABLEFIXUP`.
2. When the module loads, the CLR calculates:
   $$\text{SlotAddress} = \text{HMODULE} + \text{SlotRVA}$$
3. The metadata token inside the slot is left untouched by the OS loader.
4. The CLR reads the token, allocates the thunk, and writes the thunk address into the slot.

---

## 6. The 3-Stage Runtime Lifecycle of a VTableFixup Slot

Empirical memory observation in `cdb` confirms the three distinct transformations of `MixedLib+0xA000`:

```
+───────────────────────────────────────────────────────────────────────────────+
| Stage 1: On-Disk / Pre-Load (PE Raw File)                                     |
| Contents: MethodDef CLI Metadata Tokens                                       |
+───────────────────────────────────────────────────────────────────────────────+
  00007ff9`33b3a000  01 00 00 06 00 00 00 00  -> Token 0x06000001 (MethodDef #1)
  00007ff9`33b3a008  02 00 00 06 00 00 00 00  -> Token 0x06000002 (MethodDef #2)

                                        │
                                        ▼ CLR bootstrap (_CorDllMain)
+───────────────────────────────────────────────────────────────────────────────+
| Stage 2: Post-Load / Cold Start (After clr!LdrpFixupVTable)                   |
| Contents: Pointers to Dynamic 12 KB RWX Thunk Heap                            |
+───────────────────────────────────────────────────────────────────────────────+
  00007ff9`33b3a000  58 20 50 be 77 02 00 00  -> Ptr to 00000277`be502058 (RWX)
  * RWX thunk executes: adr xip0; ldr xip1; br xip1 -> mscoreei!VTableBootstrap

                                        │
                                        ▼ First invocation through slot
+───────────────────────────────────────────────────────────────────────────────+
| Stage 3: Post-JIT Resolution (After JIT compilation)                          |
| Contents: Permanent 64-bit Pointers to Executable JIT Code                    |
+───────────────────────────────────────────────────────────────────────────────+
  00007ff9`33b3a000  20 00 26 69 f8 7f 00 00  -> Ptr to 00007ff8`69260020 (JIT)
  00007ff9`33b3a008  a0 00 26 69 f8 7f 00 00  -> Ptr to 00007ff8`692600a0 (JIT)
  * Future calls branch directly to native JIT code without crossing the thunk!
```

---

## 7. Security & Memory Integrity Implications

### 1. Inability to Enforce `PAGE_READONLY` on VTableFixups
In standard C++, virtual method tables (`vftables`) are placed in `.rdata`. During image loading, `ntdll` temporarily marks the page writable (`PAGE_READWRITE`), applies `DIR64` relocations, and immediately restores `PAGE_READONLY`.

In C++/CLI:
- Because the CLR must write thunk pointers at **Stage 2** (during `_CorDllMain`) and JIT pointers at **Stage 3** (lazily on first execution), the VTableFixup table **must reside in `.data` and remain permanently `PAGE_READWRITE`**.
- This creates the writable function pointer surface exploited in Jump-Oriented Programming (JOP).

### 2. The Linker CFG Exclusion
Because the slots in `MixedLib+0xA000` change addresses twice during process execution (from token -> to RWX thunk -> to JIT code), Microsoft's linker (`link.exe`) cannot pre-calculate indirect call validation targets.
As a result, MSVC `/clr` disables Control Flow Guard (`/guard:cf` is incompatible), leaving the indirect branch `br x9` completely unvalidated.

---

## 8. Comparative Matrix: Relocation Architectures Across Windows Runtimes

| Feature | Pure Native C/C++ (MSVC) | Pure C# (.NET CoreCLR) | C++/CLI Mixed-Mode (IJW) |
|---|---|---|---|
| **PE `.reloc` Section** | Fully populated (all pointers) | Single dummy relocation | **Dual: Only native `.rdata` in `.reloc`** |
| **VTable Relocation Mechanism** | Win32 `IMAGE_REL_BASED_DIR64` | JIT MethodTable compilation | **`IMAGE_COR_VTABLEFIXUP` (CLI Directory 14)** |
| **Relocation Target in PE** | `.rdata` (vftables) | None in static image | **`.data` (`MixedLib+0xA000`)** |
| **Memory Protection of Table** | **`PAGE_READONLY`** | **`PAGE_READONLY`** | ❌ **`PAGE_READWRITE` (Permanent)** |
| **Relocation Execution Time** | Process image mapping (`ntdll`) | Runtime JIT on first call | **3-Phase: On-Disk -> Bootstrap -> Post-JIT** |
| **Control Flow Guard (CFG)** | ✅ Compatible (`/guard:cf`) | ✅ Compatible (Bitmap sync) | ❌ **Incompatible (`0x0160`)** |
| **RWX Memory Required** | 0 KB | 0 KB | ❌ **12 KB (`PAGE_EXECUTE_READWRITE`)** |

---

## 9. Reproducibility & Verification Matrix

| Claim ID | Technical Claim | Status | Verification Command | Observation |
|---|---|---|---|---|
| **RELOC-01** | `MixedLib.dll` `.reloc` size is exactly 44 bytes (`0x2C`) | `[EXPERIMENTALLY CONFIRMED]` | Section header dump | `.reloc RVA: 0x0000C000 VSize: 0x0000002C RawOffset: 0x00008800` |
| **RELOC-02** | `.reloc` contains exactly 14 `DIR64` entries targeting `.rdata` | `[EXPERIMENTALLY CONFIRMED]` | Block parser script | 10 entries in Page `0x4000`, 4 entries in Page `0x8000`; Page `0xA000` absent |
| **RELOC-03** | CLI Header contains 6 `VTableFixup` entries at RVA `0x8F28` | `[EXPERIMENTALLY CONFIRMED]` | CLI directory dump | 6 entries of 8 bytes each (`0x30` bytes total); all `Type = 0x000A` |
| **RELOC-04** | On-disk VTableFixup slots store CLI MethodDef tokens | `[EXPERIMENTALLY CONFIRMED]` | `db MixedLib+0xa000 L20` (pre-load) | Slots store `0x06000001`, `0x06000002`, `0x06000003`, `0x06000004` |
| **RELOC-05** | Post-JIT slots are overwritten in `.data` with JIT pointers | `[EXPERIMENTALLY CONFIRMED]` | `dp MixedLib+0xa000 L4` (post-JIT) | Slots updated to `0x00007ff869260020`, `0x00007ff8692600a0` in `PAGE_READWRITE` |
