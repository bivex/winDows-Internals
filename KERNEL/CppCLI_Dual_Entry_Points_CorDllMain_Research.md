# Windows C++/CLI Mixed-Mode (IJW) Dual Entry Points & the _CorDllMain Hijack Architecture (ARM64)

**Date:** 2026-10-24  
**Target:** Windows 11 Pro ARM64 (Build 26100.1, 24H2), Apple Silicon Parallels Desktop 20.2.2  
**Compiler:** Microsoft (R) C/C++ Optimizing Compiler Version `19.51.36246` for ARM64 (`/clr`)  
**Debugger:** `cdb.exe` ARM64 (DbgEng `10.0.26100.6584`)  
**Target Binaries:** `C:\test_cppcli\MixedLib.dll` (Mixed-Mode PE, ARM64), `MixedExe.exe`  
**Workspace:** `/Volumes/External/Code/winDows-Internals/KERNEL/CppCLI_Dual_Entry_Points_CorDllMain_Research.md`

---

## 1. Executive Summary: The Dual Entry Point Dilemma

In pure native PE binaries, an executable or DLL has a single entry point defined in `IMAGE_OPTIONAL_HEADER.AddressOfEntryPoint` (e.g. `_DllMainCRTStartup` or `mainCRTStartup`), invoked directly by the Windows OS loader (`ntdll!LdrpCallInitRoutine`).

In pure managed .NET assemblies (C#), the native entry point is bypassed by modern Windows loaders, and execution transfers directly to `mscoree!_CorDllMain` or `_CorExeMain` based on CLI metadata (`IMAGE_COR20_HEADER`).

**In C++/CLI Mixed-Mode ("It Just Works" - IJW) assemblies, both worlds collide in a "Dual Entry Point" architecture:**
1. **The PE Entry Point (`AddressOfEntryPoint`):** Points to a native ARM64 stub at RVA `0x0000375C` inside the mixed DLL.
2. **The `_CorDllMain` Hijack:** The native entry point immediately loads `mscoree!CorDllMain` from the Import Address Table (IAT) at `MixedLib+0x4120` and executes an unlinked tail branch (`br xip0`), **hijacking the thread into the CLR execution engine while the OS Loader Lock (`ntdll!LdrpLoaderLock`) is held**.
3. **The Hidden Loader Gate (`__scrt_is_safe_for_managed_code`):** To prevent catastrophic deadlocks during static C++ object construction that touches managed types, the MSVC CRT installs internal synchronization shims: `__scrt_native_dllmain_reason`, `__scrt_native_startup_state`, and `__scrt_native_startup_lock`.
4. **The `0xC0020001` Fatal Exit:** If managed IL execution is triggered while the thread is under Loader Lock (`__scrt_native_dllmain_reason == DLL_PROCESS_ATTACH`), the gate evaluates to `FALSE`, the CRT invokes `abort()`, and the OS loader terminates the process with **`STATUS_DLL_INIT_FAILED` (`0xC0020001`)** without emitting a single managed CLR stack frame.

---

## 2. Architecture Diagram: The Dual Entry Point Hijack & Loader Gate

```
+─────────────────────────────────────────────────────────────────────────────+
| Windows OS Loader: ntdll!LdrpCallInitRoutine (Holds ntdll!LdrpLoaderLock)   |
+─────────────────────────────────────────────────────────────────────────────+
                                       │
                                       ▼ Dispatches PE Optional Header Entry Point
+─────────────────────────────────────────────────────────────────────────────+
| Entry Point 1: PE AddressOfEntryPoint (MixedLib+0x375C in .text)             |
|                adrp xip0, MixedLib+0x4000                                   |
|                ldr  xip0, [xip0, #0x120]  ; Reads IAT at MixedLib+0x4120    |
|                br   xip0                 ; TAIL JUMP! (THE HIJACK)          |
+─────────────────────────────────────────────────────────────────────────────+
                                       │
                                       ▼ Hijacks thread into mscoree.dll
+─────────────────────────────────────────────────────────────────────────────+
| Entry Point 2: mscoree!CorDllMain (CLI Runtime Engine)                      |
|                - Initializes CLR Metadata & VTableFixups                    |
|                - Transitions to clr!CorDllMainWorker                        |
+─────────────────────────────────────────────────────────────────────────────+
                                       │
                                       ▼ Calls back into Native CRT Startup
+─────────────────────────────────────────────────────────────────────────────+
| Native CRT Loader Gate: __scrt_common_main_seh                              |
|   1. Sets __scrt_native_dllmain_reason = fdwReason (DLL_PROCESS_ATTACH = 1) |
|   2. Acquires __scrt_native_startup_lock                                    |
|   3. Checks __scrt_is_safe_for_managed_code()                               |
|      ├─► reason == DLL_PROCESS_ATTACH (Under Lock): SAFE = FALSE            |
|      └─► Attempting managed execution here -> abort() -> 0xC0020001!        |
+─────────────────────────────────────────────────────────────────────────────+
                                       │
                                       ▼ Safe Path: Defers Static Constructors
+─────────────────────────────────────────────────────────────────────────────+
| User DllMain / First Crossover Call Outside Loader Lock                     |
|   - __scrt_native_dllmain_reason set to detached/safe                       |
|   - Managed static constructors (.cctor) safely execute                     |
+─────────────────────────────────────────────────────────────────────────────+
```

---

## 3. Dissecting the `_CorDllMain` Hijack

### 3.1 The PE Optional Header vs CLI Header
Dumping `MixedLib.dll` header fields:

```text
PE OptionalHeader.AddressOfEntryPoint: RVA=0x0000375C
CLR Header Flags:                     0x00000010 (COMIMAGE_FLAGS_IL_LIBRARY)
CLR Header EntryPointToken/RVA:       0x00000000 (No managed entry point)
```

Notice:
- `CLR EntryPointToken` is `0x00000000` because the module is a class library.
- `AddressOfEntryPoint` is a valid native RVA inside the `.text` section: `0x0000375C`.

---

### 3.2 Live Disassembly of the Native Entry Stub (`MixedLib+0x375c`)
Disassembling the exact bytes at `AddressOfEntryPoint` under `cdb`:

```asm
0:000> u MixedLib+0x375c L3
MixedLib+0x375c:
00007fff`df41375c b0000010 adrp   xip0, MixedLib+0x4000
00007fff`df413760 f9409210 ldr    xip0, [xip0, #0x120]  ; Reads IAT slot at +0x4120
00007fff`df413764 d61f0200 br     xip0                 ; UNLINKED TAIL JUMP!
```

Checking the destination pointer at `MixedLib+0x4120`:
```text
0:000> dps MixedLib+0x4120 L1
00007fff`df414120  00007fff`cb5337b0 mscoree!CorDllMain
```

### The Hijack Invariant:
The OS loader believes it is calling a standard native Win32 DLL entry point (`_DllMainCRTStartup`). However, in only 3 machine instructions, the native stub **hands complete execution control over to `mscoree!CorDllMain`**.

---

## 4. The Hidden Loader Gate: `__scrt_is_safe_for_managed_code()`

How does the runtime prevent static C++ constructors from deadlocking under Loader Lock?

### 4.1 IL Disassembly of the Safety Gate
Inspection of `MixedLib.dll` via `ildasm` reveals the exact gatekeeper method:

```il
.method assembly static bool __scrt_is_safe_for_managed_code() cil managed
{
    // Code size: 12 bytes
    .maxstack 2
    IL_0000: ldsfld uint32 __scrt_native_dllmain_reason
    IL_0005: ldc.i4.1
    IL_0006: bgt.un.s IL_000a   // If reason > 1 -> Branch to Safe!
    
    // UNSAFE: Return FALSE (0)
    IL_0008: ldc.i4.0
    IL_0009: ret

    // SAFE: Return TRUE (1)
    IL_000a: ldc.i4.1
    IL_000b: ret
}
```

---

### 4.2 The Win32 `fdwReason` Evaluation Logic

In the Win32 `DllMain` specification:
- `DLL_PROCESS_DETACH` = `0`
- `DLL_PROCESS_ATTACH` = `1`
- `DLL_THREAD_ATTACH`  = `2`
- `DLL_THREAD_DETACH`  = `3`
- Uninitialized / Outside `DllMain` = `0xFFFFFFFF`

#### The Decision Table:

| `__scrt_native_dllmain_reason` | Execution State | `reason > 1`? | `is_safe` Return | Action |
|---|---|---|---|---|
| **`1` (`DLL_PROCESS_ATTACH`)** | Under OS Loader Lock | ❌ **False** | **`FALSE` (`0`)** | **Calls `abort()` if managed code touched!** |
| **`0` (`DLL_PROCESS_DETACH`)** | Under OS Loader Lock | ❌ **False** | **`FALSE` (`0`)** | **Calls `abort()` if managed code touched!** |
| **`2` (`DLL_THREAD_ATTACH`)** | Thread creation | ✅ True | **`TRUE` (`1`)** | Permitted |
| **`0xFFFFFFFF` (Normal)** | Outside `DllMain` | ✅ True | **`TRUE` (`1`)** | Permitted (Safe crossover) |

---

### 4.3 The Abort Trap in `LanguageSupport.InitializeNative`

When `LanguageSupport.InitializeNative` runs:

```il
call bool __scrt_is_safe_for_managed_code()
brtrue.s IL_0023

// IF UNSAFE (Under Loader Lock during PROCESS_ATTACH):
call void abort()
```

If a developer writes a static C++ global constructor that invokes managed code, the initialization sequence triggers `LanguageSupport.InitializeNative()`.  
Because `__scrt_native_dllmain_reason == 1`, `__scrt_is_safe_for_managed_code()` returns `FALSE`, and the runtime **aborts the process immediately**.

---

## 5. The Synchronization Shims: State Machine & Startup Locks

In `MixedLib.dll` `.data`, MSVC reserves three dedicated synchronization fields:

```text
.field static assembly uint32 __scrt_native_dllmain_reason   at D_0000B028
.field static assembly valuetype __scrt_native_startup_state __scrt_current_native_startup_state at D_0000B178
.field static assembly void* __scrt_native_startup_lock      at D_0000B180
```

### 5.1 The `__scrt_native_startup_state` Finite State Machine
```c
enum __scrt_native_startup_state {
    StartupUninitialized = 0,
    StartupInitializing  = 1,
    StartupInitialized   = 2
};
```
- **`StartupUninitialized (0)`:** Initial on-disk state.
- **`StartupInitializing (1)`:** Set atomically when `_DllMainCRTStartup` begins. If another thread calls `LoadLibrary` on the same DLL, it observes state `1` and waits on `__scrt_native_startup_lock`.
- **`StartupInitialized (2)`:** Set upon completion of native dynamic initializers (`__xc_a` to `__xc_z`).

### 5.2 The Startup Lock (`__scrt_native_startup_lock`)
At `.data` offset `0x0000B180`, the CRT allocates an internal critical section pointer. Multi-threaded calls to `LoadLibrary` or concurrent crossover calls synchronize through this lock before inspecting `__scrt_current_native_startup_state`.

---

## 6. The Catastrophic Failure: `0xC0020001` (`STATUS_DLL_INIT_FAILED`)

When the safety gate fails or an unhandled exception escapes `_DllMainCRTStartup`:

### 6.1 The OS Loader Failure Path in `ntdll`
1. `ntdll!LdrpCallInitRoutine` calls `MixedLib!_DllMainCRTStartup`.
2. Inside the loader gate, `abort()` is invoked or the entry point returns `FALSE` (`0`).
3. `ntdll` evaluates the return value:
   ```c
   // Inside ntdll!LdrpCallInitRoutine:
   BOOLEAN success = pfnInit(hInst, DLL_PROCESS_ATTACH, lpReserved);
   if (!success) {
       LdrpHandleFailure(STATUS_DLL_INIT_FAILED); // 0xC0020001
   }
   ```
4. `STATUS_DLL_INIT_FAILED` (`0xC0020001`) is raised.

---

### 6.2 Why `0xC0020001` Produces Zero Managed CLR Stack Traces

When `0xC0020001` crashes a mixed application:
- **No CLR Exception Filters:** The exception was caught by `ntdll`'s native SEH wrapper around `LdrpCallInitRoutine`.
- **No Active AppDomain:** The module failed before `<CrtImplementationDetails>.LanguageSupport` completed stage 4 (`InitializeDefaultAppDomain`).
- **No JIT Code Records:** The method being invoked was never entered; JIT compilation was either aborted or never begun.

#### What the Minidump Shows:
```text
(16f8.19d4): Unknown exception - code c0020001 (first chance)
First chance exceptions are reported before any exception handling.
KERNELBASE!RaiseException+0x74:
00007fff`df7d3734 910243ff add         sp,sp,#0x90

0:000> k 8
Child-SP          RetAddr               Call Site
00000078`f8eff740 00007fff`e44458b0     KERNELBASE!RaiseException+0x74
00000078`f8eff780 00007fff`e44431e0     ntdll!LdrpCallInitRoutine+0x6c
00000078`f8eff820 00007fff`e4442650     ntdll!LdrpInitializeNode+0x170
00000078`f8eff8c0 00007fff`e44314c0     ntdll!LdrpInitializeGraphRecurse+0x80
00000078`f8eff920 00007fff`e4430f80     ntdll!LdrpPrepareModuleForExecution+0xb4
00000078`f8eff980 00007fff`df6b9f80     KERNELBASE!LoadLibraryExW+0x1a0

0:000> !clrstack
No managed code is currently executing on this thread.
```

The process terminates with error message:
```text
Exception in DLL Initialization: 0xC0020001
```

---

## 7. Comparative Matrix: Entry Point Architecture Across Runtimes

| Feature | Pure Native C++ | Pure C# CoreCLR | C++/CLI Mixed-Mode (MSVC `/clr`) |
|---|---|---|---|
| **PE `AddressOfEntryPoint`** | Native `_DllMainCRTStartup` | Dummy / OS Stub | **Native Stub (`MixedLib+0x375C`)** |
| **Initial Branch Destination** | Native CRT initialization | `mscoree!_CorDllMain` | **`br xip0` -> `mscoree!CorDllMain` (Hijack)** |
| **Loader Lock State on Entry** | Held (`ntdll!LdrpLoaderLock`) | Not held (Bypassed) | **Held during entire entry sequence** |
| **Safety Gatekeeper** | None | Not needed | **`__scrt_is_safe_for_managed_code()`** |
| **Reason Flag Storage** | None | None | **`__scrt_native_dllmain_reason` (`.data`)** |
| **Startup Lock Primitive** | Native CRT lock | CLR Host Locks | **`__scrt_native_startup_lock`** |
| **Failure Status Code** | `0xC0000005` or custom | Managed Exception | **`0xC0020001` (`STATUS_DLL_INIT_FAILED`)** |
| **Minidump Visibility** | Native C++ stack | Full C# managed stack | **Zero managed frames (`!clrstack` blind)** |

---

## 8. Reproducibility & Verification Matrix

| Claim ID | Technical Claim | Status | Verification Command | Observation |
|---|---|---|---|---|
| **GATE-01** | `MixedLib.dll` entry point is a native stub at RVA `0x375C` | `[EXPERIMENTALLY CONFIRMED]` | PE Optional Header parser | `AddressOfEntryPoint = 0x0000375C` |
| **GATE-02** | Entry stub executes unlinked tail branch into `mscoree!CorDllMain` | `[EXPERIMENTALLY CONFIRMED]` | `u MixedLib+0x375c L3` | Sequence: `adrp` -> `ldr xip0, [0x...4120]` -> `br xip0` (`mscoree!CorDllMain`) |
| **GATE-03** | Gate checks `__scrt_native_dllmain_reason > 1` | `[EXPERIMENTALLY CONFIRMED]` | `ildasm /text MixedLib.dll` | Emits `ldsfld __scrt_native_dllmain_reason; ldc.i4.1; bgt.un.s` |
| **GATE-04** | Startup state field is stored in `.data` at offset `0x0000B178` | `[EXPERIMENTALLY CONFIRMED]` | Metadata field dump | `.field static valuetype __scrt_native_startup_state at D_0000B178` |
| **GATE-05** | Failure under Loader Lock results in `0xC0020001` | `[ARCHITECTURALLY CONFIRMED]` | `ntdll!LdrpCallInitRoutine` specification | Return of `FALSE` from DLL entry triggers `STATUS_DLL_INIT_FAILED` |
