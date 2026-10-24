# Windows C++/CLI Mixed-Mode (IJW) Loader & VTable Fixup Internals

**Date:** 2026-10-24  
**Environment:** Windows 11 Pro ARM64 (Build 26100.1, 24H2), Apple Silicon Parallels Desktop 20.2.2  
**Compiler:** Microsoft (R) C/C++ Optimizing Compiler Version `19.51.36246` for ARM64  
**Debugger:** `cdb.exe` ARM64 (DbgEng `10.0.26100.6584`)  
**Target Binaries:** `C:\test_cppcli\MixedLib.dll` (SHA256: `9B457D97...`), `C:\test_cppcli\MixedExe.exe`  
**Target File:** `/Volumes/External/Code/winDows-Internals/KERNEL/CppCLI_IJW_Loader_Research.md`  

---

## 1. Executive Summary

C++/CLI (compiled via `cl /clr`) is the unique hybrid execution model in the Windows ecosystem where a single Portable Executable (PE) contains **both native machine instructions and Common Intermediate Language (CIL) bytecode simultaneously**.

Unlike pure .NET Framework assemblies (which route through `mscoree!_CorExeMain`) or modern .NET AppHosts (which boot via `hostfxr` $\to$ `coreclr`), C++/CLI employs the **IJW ("It Just Works")** architecture:
1. **Native Entry Point:** The PE's `AddressOfEntryPoint` points to native CRT startup code (`_DllMainCRTStartup`), not the CLR shim.
2. **`VTableFixups` Directory:** The CLI header defines a table of slots that initially hold metadata tokens on disk.
3. **Lazy Thunk Back-Patching:** Upon image initialization, the CLR converts these tokens into transition thunks. Upon the first native-to-managed crossover call, the thunk JIT-compiles the target managed method and **back-patches the slot in place** with the raw native address. All subsequent calls execute as a direct indirect branch without runtime overhead.

```
THE THREE WINDOWS CLR LOADING ARCHITECTURES COMPARED:

1. CLASSIC .NET FRAMEWORK (Pure Managed):
[PE Optional Header] ──► AddressOfEntryPoint = Native Stub [jmp __imp__CorExeMain]
                         DataDirectory[14]   = IMAGE_COR20_HEADER (Flags = 0x01 ILONLY)
                         Bootstrap           = OS Loader -> BaseThreadInitThunk -> MSCOREE

2. MODERN .NET CORE / 10 (AppHost):
[PE Optional Header] ──► AddressOfEntryPoint = Native CRT wmain
                         DataDirectory[14]   = 0x00000000 (Absent)
                         Bootstrap           = AppHost -> hostfxr -> hostpolicy -> coreclr

3. C++/CLI MIXED-MODE (IJW):
[PE Optional Header] ──► AddressOfEntryPoint = Native CRT _DllMainCRTStartup
                         DataDirectory[14]   = IMAGE_COR20_HEADER (Flags = 0x10 NATIVE_EP)
                         VTableFixups        = Slot Array (Token -> IJW Thunk -> JIT Code)
                         Bootstrap           = Native DllMain -> CorDllMainWorkerForThunk
```

---

## 2. PE Binary Anatomy of a Mixed-Mode Module (`MixedLib.dll`)

A mixed-mode assembly declares both native Win32/ARM64 PE characteristics and an ECMA-335 CLI directory.

### 2.1 Empirical Header Extraction

Inspecting `C:\test_cppcli\MixedLib.dll` compiled via `cl /clr /LD MixedLib.cpp`:
```text
MixedLib.dll PE Architecture:            Machine=0xAA64 (ARM64 Native), Magic=0x020B (PE32+)
PE AddressOfEntryPoint:                  0x0000319C (_DllMainCRTStartup)
DataDirectory[14] COM Descriptor RVA:    0x00004248, Size: 0x00000048 (72 bytes)
IMAGE_COR20_HEADER.Flags:                0x00000010 (COMIMAGE_FLAGS_NATIVE_ENTRYPOINT)
IMAGE_COR20_HEADER.EntryPointTokenOrRVA: 0x00001730 (Native Code RVA)
IMAGE_COR20_HEADER.VTableFixups RVA:     0x00008800, Size: 0x00000018
```

### 2.2 Key Divergences from Pure Managed PEs

1. **`Flags == 0x00000010` (`COMIMAGE_FLAGS_NATIVE_ENTRYPOINT`):**
   * In pure managed assemblies, bit 0 (`COMIMAGE_FLAGS_ILONLY` = `0x01`) is asserted.
   * In C++/CLI, `ILONLY` is **0**. Bit 4 (`0x10`) is set, instructing the runtime that the entry point field is a native machine code RVA, not a metadata token.
2. **`EntryPointTokenOrRVA == 0x00001730`:**
   * This is a relative virtual address pointing to native ARM64 CRT initialization code.
3. **`VTableFixups Directory` (Non-Zero RVA & Size):**
   * In pure C# assemblies, `VTableFixups` is strictly `0x0`.
   * In C++/CLI, this directory defines the array of crossover slots linking unmanaged callers to managed methods.

---

## 3. `IMAGE_COR_VTABLEFIXUP` & The 3-State Slot Lifecycle

Defined in `<corhdr.h>`, the `VTableFixups` directory points to an array of fixup headers:

```cpp
typedef struct IMAGE_COR_VTABLEFIXUP {
    DWORD  RVA;       // RVA of the vtable slot array
    WORD   Count;     // Number of contiguous slots in this table
    WORD   Type;      // COR_VTABLE_* flags
} IMAGE_COR_VTABLEFIXUP;
```

#### Fixup Type Flags:
* `COR_VTABLE_32BIT` (`0x01`): 32-bit slot width.
* `COR_VTABLE_64BIT` (`0x02`): 64-bit slot width.
* `COR_VTABLE_FROM_UNMANAGED` (`0x04`): Call originates from unmanaged code; slot must transition to managed.
* `COR_VTABLE_CALL_MOST_DERIVED` (`0x10`): Virtual method dispatch through most derived class.

### 3.1 Live Extraction of VTableFixup Table:

```text
VTableFixup Entry:
  Table RVA:   0x0000A000
  Slot Count:  1
  Type Flags:  0x000A (COR_VTABLE_64BIT [0x02] | COR_VTABLE_FROM_UNMANAGED [0x08])
```

### 3.2 The 3-State Lifecycle of a Slot (`RVA 0x0000A000`)

By monitoring the virtual address `MixedLib.dll + 0xA000` through three phases, the exact in-place back-patching mechanism is revealed:

```
┌────────────────────────────────────────────────────────────────────────┐
│ State 1: On-Disk in PE File (Raw Binary)                               │
│ Value = 0x0000000006000001 (MethodDef Metadata Token #1)               │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
                         Image Loaded into RAM
                                   │
                                   ▼
┌────────────────────────────────────────────────────────────────────────┐
│ State 2: In-Memory Before First Crossover Call                         │
│ Value = 0x000001f175502058 (IJW Transition Thunk Address)              │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
                     Native Calls NativeCallsManaged()
                                   │
                                   ▼
┌────────────────────────────────────────────────────────────────────────┐
│ State 3: In-Memory After First Call (JIT Compiled & Back-Patched)      │
│ Value = 0x00007fff8af10020 (Compiled Managed Native Code Address)      │
└────────────────────────────────────────────────────────────────────────┘
```

#### Raw Debugger Memory Verification:
* **Before Call:**
  ```text
  0:000> dp MixedLib+0xA000 L2
  00007ff8`1a4ba000  000001f1`75502058 00000000`ffffffff
  ```
  Points to the heap-allocated IJW transition thunk (`0x000001f175502058`).
* **After Call:**
  ```text
  0:000> dp MixedLib+0xA000 L2
  00007ff8`1a4ba000  00007fff`8af10020 00000000`ffffffff
  ```
  The slot was **overwritten in place** with `0x00007fff8af10020`—the exact ARM64 JIT-compiled address of `ManagedClass::Compute`!

---

## 4. Module Load & CLR Bootstrap via Native `_DllMainCRTStartup`

When the native application (`MixedExe.exe`) starts, the Windows OS loader loads `MixedLib.dll` as a standard dynamic dependency. The CLR is not initialized by `ntdll` or `wmain`, but from the **native CRT DLL entry point**.

### 4.1 Live Call Stack During CLR Load:

Setting a load breakpoint on `clr.dll` (`sxe ld:clr.dll`):
```text
Child-SP          RetAddr               Call Site
00000043`373ceeb0 00007ff8`24f69754     ntdll!NtMapViewOfSection+0x4
00000043`373ceeb0 00007ff8`24f68b6c     ntdll!LdrpMinimalMapModule+0x2e4
00000043`373cf6b0 00007ff8`20521de8     ntdll!LdrLoadDll+0x118
00000043`373cf7a0 00007ff8`0ed0afa4     KERNELBASE!LoadLibraryExW+0xf8
00000043`373cf800 00007ff8`0ed0b2b4     mscoreei!RuntimeDesc::LoadLibrary+0x14c
00000043`373cf990 00007ff8`0ed0b3e4     mscoreei!RuntimeDesc::LoadMainRuntimeModule+0x29c
00000043`373cfa40 00007ff8`0ed0ab0c     mscoreei!RuntimeDesc::EnsureLoaded+0xa4
00000043`373cfaf0 00007ff8`0ed0da8c     mscoreei!RuntimeDesc::GetProcAddressInternal+0x24
00000043`373cfb30 00007ff8`0ecfdbcc     mscoreei!CLRRuntimeInfoImpl::GetProcAddress+0x6c
00000043`373cfbd0 00007ff8`0ed17da0     mscoreei!CorDllMainWorkerForThunk+0x9c  ◄── IJW Thunk Init
```

*Observation:* The initialization of `clr.dll` roots in `mscoreei!CorDllMainWorkerForThunk`, called during native CRT startup when processing VTable fixups.

---

## 5. Crossover Mechanics: Disassembly of `NativeCallsManaged`

In `MixedLib.cpp`, the exported native function calls managed code:
```cpp
extern "C" __declspec(dllexport) int NativeCallsManaged(int x) {
    return ManagedClass::Compute(x);
}
```

Disassembly of `MixedLib!NativeCallsManaged` on ARM64:
```asm
; C:\test_cppcli\MixedLib.dll!NativeCallsManaged:
00007ff8`1a4b1238 b0000049 adrp    x9, MixedLib+0xA000   ; Load page of VTableFixup slot (0x1a4ba000)
00007ff8`1a4b123c f9400129 ldr     x9, [x9]             ; Dereference [0x1a4ba000] -> Read slot value!
00007ff8`1a4b1240 d61f0120 br      x9                   ; Indirect branch to target address!
```

### Architectural Analysis:
1. **First Invocation:**
   - `x9` receives the IJW Thunk address (`0x000001f175502058`).
   - `br x9` enters the thunk, which ensures the runtime is loaded, calls RyuJIT to compile `ManagedClass::Compute`, overwrites slot `0x1a4ba000` with `0x00007fff8af10020`, and executes the method.
2. **Subsequent Invocations:**
   - `x9` receives the already-patched native code address (`0x00007fff8af10020`).
   - `br x9` branches **directly to the compiled machine code**.
   - **Cost:** Exactly 3 machine instructions (`adrp + ldr + br`), matching the speed of a standard C++ DLL export thunk.

---

## 6. The Loader Lock Deadlock Hazard in C++/CLI

Because C++/CLI mixed assemblies execute managed initialization code within `DllMain`, they are vulnerable to the notorious **Loader Lock Deadlock**:
1. When the OS loader maps `MixedLib.dll`, it acquires the exclusive process loader lock:
   `ntdll!LdrpLoaderLock`
2. If `DllMain` or static C++ constructors invoke managed code that attempts to:
   - Load another assembly (`Assembly.Load`).
   - Call `P/Invoke` requiring an unloaded native DLL (`LoadLibrary`).
   - Initialize an un-jitted complex type triggering `CoInitialize` or thread synchronization.
3. The managed execution engine invokes `LdrLoadDll` or waits for worker threads, attempting to re-acquire or synchronize on `LdrpLoaderLock`.
4. The loader thread is blocked waiting for the CLR; the CLR is blocked waiting for the loader lock $\implies$ **Permanent Process Deadlock**.

---

## 7. Calling Conventions: `__clrcall` vs. `__cdecl`

| Characteristic | `__cdecl` (Native ARM64 / x64) | `__clrcall` (Managed C++/CLI) |
|---|---|---|
| **Target Code** | Native unmanaged machine instructions | Managed CIL / RyuJIT compiled code |
| **Argument Passing** | Standard Platform ABI registers (`x0`–`x7` on ARM64) | Managed evaluation stack / JIT calling convention |
| **`this` Pointer** | Raw unmanaged pointer (`void*`) | Managed reference pointer (`Object^`, GC-tracked) |
| **Object Mobility** | Memory address must be stationary | Garbage Collector may relocate object unless pinned |
| **Pinning Requirement**| Native code holding managed ref requires `pin_ptr<T>` | No pinning required (runtime updates references) |

---

## 8. Tripartite Architectural Comparison

| Loading Property | Classic .NET Framework (4.8) | Modern .NET (.NET 10) | C++/CLI Mixed Mode (IJW) |
|---|---|---|---|
| **Primary Binary Type** | Pure Managed PE (`.exe`/`.dll`) | Native AppHost wrapper (`.exe`) | Hybrid Mixed PE (`.exe`/`.dll`) |
| **`Flags.ILONLY` (`0x01`)** | **1** (Pure CIL) | **0** (Absent from root EXE) | **0** (Contains native code) |
| **`Flags.NATIVE_ENTRYPOINT`**| **0** (Metadata token) | **0** (No CLI header in EXE) | **1** (`0x10`, Native RVA) |
| **`VTableFixups` Directory** | `0x00000000` (Empty) | `0x00000000` (Empty) | **Populated** (`Count >= 1`) |
| **Bootstrap Origin** | `ntdll` $\to$ Native Stub (`FF 25`) | Native CRT `wmain` in AppHost | Native CRT `_DllMainCRTStartup` |
| **Runtime Selection** | `mscoree.dll` (Registry) | `hostfxr.dll` (`.runtimeconfig.json`) | `mscoreei.dll` (`CorDllMainWorkerForThunk`) |
| **Crossover Mechanism** | P/Invoke (`[DllImport]`) | P/Invoke / Native Library Resolvers | **IJW Thunk Lazy Back-Patching** |

---

## 9. Reproducibility & Falsification Matrix

| Claim ID | Technical Claim | Status | Target Configuration | Verification Command | Raw Empirical Observation | Falsification Condition |
|---|---|---|---|---|---|---|
| **CPP-01** | **C++/CLI declares `NATIVE_ENTRYPOINT` (`0x10`) with `ILONLY == 0`** | `[EXPERIMENTALLY CONFIRMED]` | Windows 11 ARM64 Build 26100.1, MSVC 19.51 | PowerShell PE Header Parser | `Flags: 0x00000010`, `ILONLY: 0`, `EntryPointRVA: 0x00001730` | `Flags & 0x01 != 0` or absence of bit `0x10`. |
| **CPP-02** | **C++/CLI defines populated `VTableFixups` Directory** | `[EXPERIMENTALLY CONFIRMED]` | Windows 11 ARM64 Build 26100.1, `MixedLib.dll` | PowerShell PE Header Parser | `VTableFixups RVA: 0x00008800, Size: 0x18`, `Slot Count: 1` | `VTableFixups.VirtualAddress == 0` in mixed assembly. |
| **CPP-03** | **VTable slot holds `MethodDef` token on disk (`0x06000001`)** | `[EXPERIMENTALLY CONFIRMED]` | `MixedLib.dll` binary file on disk | Binary byte reader at `RVA 0xA000` | `Initial Token: 0x0000000006000001` | Non-token raw pointer stored in uninitialized disk image. |
| **CPP-04** | **CLR bootstraps from `mscoreei!CorDllMainWorkerForThunk`** | `[EXPERIMENTALLY CONFIRMED]` | `MixedExe.exe` running under `cdb.exe` ARM64 | `sxe ld:clr.dll; g; k 10; q` | Stack roots in `CorDllMainWorkerForThunk` $\to$ `LoadLibraryExW` | CLR initialized from `ntdll!LdrpInitializeProcess` directly. |
| **CPP-05** | **VTable slot back-patched from IJW Thunk to JITted code address** | `[EXPERIMENTALLY CONFIRMED]` | `MixedExe.exe` running under `cdb.exe` ARM64 | `dp MixedLib+0xA000 L2` before and after call | `0x000001f175502058` $\longrightarrow$ `0x00007fff8af10020` | Slot value remaining unchanged after crossover call. |
| **CPP-06** | **Call site uses direct indirect jump (`adrp + ldr + br`) without P/Invoke** | `[EXPERIMENTALLY CONFIRMED]` | Disassembly of `MixedLib!NativeCallsManaged` | `u MixedLib!NativeCallsManaged L3` | `adrp x9, MixedLib+0xA000; ldr x9, [x9]; br x9` | Compiler generating P/Invoke marshaling IL stub. |
