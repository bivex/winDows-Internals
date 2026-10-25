# Windows NativeAOT Manual PE Loader (ARM64)

Manual usermode PE loader for **NativeAOT shared libraries (`.dll`)**, implementing the architectural findings documented in:
📄 **[`KERNEL/NativeAOT_Loader_Internals_Research.md`](../../KERNEL/NativeAOT_Loader_Internals_Research.md)**.

---

## Architectural Comparison: NativeAOT vs. C++/CLI (IJW)

| Characteristic | C++/CLI Mixed Mode (IJW) | NativeAOT Shared Library |
|---|---|---|
| **Primary Binary Type** | Hybrid Mixed PE (`.dll`) | Pure Unmanaged Win32 PE (`.dll`) |
| **DataDirectory[14] (CLI)** | Populated (`NATIVE_ENTRYPOINT` = 1) | **0x00000000 (Strictly ABSENT!)** |
| **VTableFixup Relocations** | Populated (Layer 2 slot array) | **None** (Pure Win32 PE) |
| **External CLR Host** | `mscoree.dll` -> `clr.dll` | **ZERO external CLR DLLs** |
| **Garbage Collector** | `clr!WKS::gc_heap` | **In-binary `WKS::gc_heap`** |
| **Object Allocator** | `clr!JIT_New` | **In-binary `RhpNewFast`** |
| **Compilation Model** | CIL Bytecode + JIT (RyuJIT) | **Ahead-of-Time Machine Code** |
| **Unwind Tables** | `.pdata` + `clr!UMThunkStub` | `.pdata` + In-binary personality |
| **Thread Local Storage** | Win32 TLS + Managed `[ThreadStatic]` | **Win32 TLS (`DataDirectory[9]`)** |

---

## Key Loader Internals Discovered & Implemented

### 1. `RhpNewFast` Lock-Free Bump Allocator & Win32 TLS
Unlike classic .NET Framework or CoreCLR, NativeAOT's high-performance object allocator (`RhpNewFast`) allocates objects in **14 hardware instructions** without acquiring any heap lock:
```asm
ldr  w2, [x3, #0x300]         ; w2 = __tls_index
ldr  x3, [xpr, #0x58]         ; xpr (x18) = TEB! Read ThreadLocalStoragePointer
ldr  x3, [x3, w2 uxtw #3]     ; Load Thread allocation context block
add  x3, x3, #0x30            ; Point to [alloc_ptr, alloc_limit]
ldr  w2, [x0, #4]             ; w2 = object size from IEEType (+0x04)
ldr  x12, [x3, #8]            ; x12 = alloc_ptr (Current bump pointer)
ldr  x13, [x3]                ; x13 = alloc_limit (Quantum ceiling)
...
str  x0, [x12]                ; Store IEEType* into object header [alloc_ptr]!
str  x2, [x3, #8]             ; Update alloc_ptr
mov  x0, x12                  ; Return new object reference in x0!
ret
```
Because the binary is manually mapped without the OS loader's `LdrpInitializeTls`:
* `TlsSetValue(tlsIndex, block)` only populates `TEB->TlsSlots` (`TEB + 0x1480`).
* `RhpNewFast` strictly dereferences `TEB->ThreadLocalStoragePointer[tlsIndex]` (`TEB + 0x58`).
* The manual loader dynamically allocates/expands `TEB->ThreadLocalStoragePointer` and binds `pTlsArray[tlsIndex] = block`.

### 2. `PalGetModuleHandleFromPointer` & `KiUserInvertedFunctionTable`
When an unmanaged caller invokes an `[UnmanagedCallersOnly]` export, NativeAOT executes `RhpReversePInvoke`, which calls `PalGetModuleHandleFromPointer`:
* It queries `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, codePtr, &hMod)`.
* In `KERNELBASE`, this delegates to `ntdll!RtlPcToFileHeader(codePtr)`.
* `RtlPcToFileHeader` binary-searches `ntdll!KiUserInvertedFunctionTable` (exported by `ntdll` as Export #111).
* If the manually mapped module is absent from `KiUserInvertedFunctionTable`, `RtlPcToFileHeader` returns `NULL`, causing `RhRegisterOSModule` to dereference NULL and crash with `STATUS_FAIL_FAST_EXCEPTION` (`0xC0000602`).
* The loader resolves `ntdll!KiUserInvertedFunctionTable`, unprotects its page via `VirtualProtect`, and inserts the module's `.pdata` and image boundaries in sorted order.

### 3. W^X TLS Callback Order
NativeAOT shared libraries emit a TLS callback (`__dyn_tls_init`) at `PIMAGE_TLS_DIRECTORY64.AddressOfCallBacks`. Calling callbacks while pages are still `PAGE_READWRITE` immediately triggers a hardware execution fault (`pacibsp` on ARM64). The loader enforces strict W^X sequencing: `ProtectSections()` executes *before* invoking TLS callbacks.
### 4. Background Thread TLS Auto-Attachment (`KERNEL32!CreateThread`)
NativeAOT spawns background runtime threads (e.g. the `.NET Finalizer` thread and threadpool workers). When manually mapped without the OS loader, newly spawned threads lack a `ThreadLocalStoragePointer` entry, causing immediate access violations upon reading TLS. The loader hooks `KERNEL32!CreateThread` inside the mapped image's IAT, wrapping thread start routines to automatically invoke `ThreadAttach()` before executing managed code.

### 5. `ntdll!LdrpFreeTls` Heap Layout & `ExitThread`
When threads terminate, `ntdll!LdrpFreeTls` frees `TEB->ThreadLocalStoragePointer` via `RtlFreeHeap(RtlProcessHeap(), 0, x21 - 0x10)`. The OS loader expects an exact 16-byte (`0x10`) header preceding the array pointer. Allocating with `calloc` or omitting the `0x10` prefix throws `0xC0000374` (`STATUS_HEAP_CORRUPTION`). The loader allocates `0x10 + N * 8` bytes via `HeapAlloc(GetProcessHeap(), ...)` and returns `ptr + 0x10`.

### 6. Executable Binary (`.EXE`) Mode & CRT `exit()` Interception
When loading a standalone NativeAOT executable (`AOT_Gui.exe`), the loader spawns a dedicated worker thread to invoke `wmainCRTStartup`. When `wmain` returns, the CRT calls `exit(0)` / `_exit(0)`. The loader intercepts `exit` and `ExitProcess` via IAT redirection to `ExitThread`, allowing the host loader process to survive, capture the exit code, and report success.

---

## Files

| File | Description |
|---|---|
| `AotLib/AotLib.csproj` | NativeAOT shared library project (`<NativeLib>Shared</NativeLib>`, `<PublishAot>true</PublishAot>`). |
| `AotLib/AotLib.cs` | C# library declaring 7 unmanaged exports (`[UnmanagedCallersOnly]`) exercising the GC heap, bump allocator, exceptions, BCL, and GUI MessageBox. |
| `AOT_Gui/AOT_Gui.csproj` | Standalone NativeAOT executable project (`<OutputType>Exe</OutputType>`). |
| `AOT_Gui/Program.cs` | C# executable displaying an interactive native Win32 GUI MessageBox with bump allocation on `WKS::gc_heap`. |
| `NativeAotLoader.cpp` | Dual-mode manual PE loader in native Win32 C++ supporting both `.dll` and `.exe` NativeAOT targets. |
| `build.bat` | Automated compilation script building both targets and running the test suite. |

---

## Build & Run

From a Visual Studio Developer Command Prompt on ARM64:

```cmd
build.bat
```

Or manually:
```cmd
dotnet publish AotLib\AotLib.csproj -c Release -r win-arm64 -p:PublishAot=true
dotnet publish AOT_Gui\AOT_Gui.csproj -c Release -r win-arm64 -p:PublishAot=true
copy /y AotLib\bin\arm64\Release\net10.0\win-arm64\publish\AotLib.dll .
copy /y AOT_Gui\bin\arm64\Release\net10.0\win-arm64\publish\AOT_Gui.exe .
cl /EHsc NativeAotLoader.cpp user32.lib /Fe:NativeAotLoader.exe
NativeAotLoader.exe AotLib.dll
NativeAotLoader.exe AOT_Gui.exe
```

---

## Expected Output (Observed on Windows 11 ARM64 Build 26100)

```text
====================================================================
  Windows NativeAOT Manual PE Loader — host ARM64
====================================================================
[load] Target: AotLib.dll

--- PE Binary Anatomy (mirrors NativeAOT Research Section 2) ---
Machine                 : 0xAA64 (ARM64 Native)
Magic                   : 0x020B (PE32+)
Preferred ImageBase     : 0x0000000180000000
AddressOfEntryPoint RVA : 0x000AD820 (Native _DllMainCRTStartup)
COM Descriptor (dir 14) : RVA 0x00000000 Size 0x00000000
  CLI Header Status     : ABSENT (0x00000000)                [AOT-01 OK]
  Kernel Perception     : Pure Win32 Unmanaged Image          [AOT-01 OK]
TLS Directory (dir 9)   : RVA 0x000E9580 Size 0x00000028
  Native Win32 TLS      : POPULATED (TEB allocator context)  [AOT-02 OK]

Mapped Base             : 0x0000000180000000 (size 0x120000)
Relocation Delta        : 0
[reloc] Base relocations applied: 3871 (padding: 25)
[imports] Resolving import directory dependencies:
[imports]   ADVAPI32.dll                       6 thunk(s) resolved
[imports]   bcrypt.dll                         1 thunk(s) resolved
[imports]   KERNEL32.dll                      97 thunk(s) resolved
[imports]   ole32.dll                          4 thunk(s) resolved
[imports]   api-ms-win-crt-heap-l1-1-0.dll     4 thunk(s) resolved
[imports]   api-ms-win-crt-math-l1-1-0.dll     1 thunk(s) resolved
[imports]   api-ms-win-crt-string-l1-1-0.dll   5 thunk(s) resolved
[imports]   api-ms-win-crt-convert-l1-1-0.dll   1 thunk(s) resolved
[imports]   api-ms-win-crt-stdio-l1-1-0.dll    5 thunk(s) resolved
[imports]   api-ms-win-crt-runtime-l1-1-0.dll  12 thunk(s) resolved
[imports] Zero CLR dependencies confirmed (pure OS & UCRT links) [AOT-03 OK]
[tls] Initializing Win32 TLS Directory:
        StartAddressOfRawData : 0x00000001800E9D10
        EndAddressOfRawData   : 0x00000001800E9E34
        AddressOfIndex        : 0x0000000180116910
        AddressOfCallBacks    : 0x00000001800B4530
        SizeOfZeroFill        : 0x0
[tls] Assigned __tls_index = 4 for RhpNewFast context
[tls] Bound TEB->ThreadLocalStoragePointer[4] = 00000267EA11ABE0 (TEB=000000C0EE644000)
[ldr] Registered LDR ghost entry & RB-tree node (path: "C:\test_nativeaot\AotLib.dll")
[pdata] KiUserInvertedFunctionTable at 00007FF94A33B4D0 (current=17, max=512)
[pdata] Inserted entry at index 0: ImageBase=0000000180000000, Size=0x120000, pdata=0000000180117000 (size=0x5F60)
[tls] Executing TLS callback 0x00000001800AD878(DLL_PROCESS_ATTACH)...
[load] Mapping complete; ready for DllMain initialization

>>> STEP 1: In-Binary Runtime Initialization (RhInitialize) <<<
[entry] Calling _DllMainCRTStartup(DLL_PROCESS_ATTACH)...
[entry] Runtime initialized: WKS::gc_heap active, RhRegisterOSModule complete

>>> EXPERIMENT 1: NativeAOT ABI & Calling Convention <<<
[call ] AotAdd(20, 22) returned: 42
>>> Result: 42 (Expected: 42) <<<

>>> EXPERIMENT 2: RhpNewFast Bump Allocator & GC Heap <<<
[call ] Invoking AotAllocateAndCompute(100) -> allocating 100 objects...
[call ] Computed sum of allocated node values: 9900
>>> Allocator Result: 9900 (Expected: 9900) <<<

>>> EXPERIMENT 3: Statically Linked Workstation GC (GC.Collect) <<<
[call ] GC Memory Before Collection : 988944 bytes
[call ] GC Memory After Collection  : 12624 bytes
[call ] In-binary GC reclaimed memory: YES (freed 976320 bytes)
>>> GC Verification: PASS <<<

>>> EXPERIMENT 4: BCL String Infrastructure <<<
[call ] Formatted BCL string: "[NativeAOT BCL] Value=777, IsDynamicCodeSupported=False" (len=55)
>>> BCL String Result: PASS <<<

>>> EXPERIMENT 5: NativeAOT Internal Exception Handling <<<
[call ] AotTestExceptions(0) -> normal return: 100 (Expected: 100)
[call ] AotTestExceptions(1) -> managed exception caught: 999 (Expected: 999)
>>> Exception Unwinding Result: PASS <<<

>>> EXPERIMENT 6: RuntimeFeature Inspection <<<
[info ] Architecture: Arm64, OS: Microsoft Windows 10.0.26200, Framework: .NET 10.0.8, DynamicCode: False
>>> IsDynamicCodeSupported == False: VERIFIED (Hallmark of NativeAOT) <<<

>>> EXPERIMENT 7: GUI MessageBox (NativeAOT -> user32!MessageBoxW) <<<
[AotLib!AotTestGuiMessageBox] Showing GUI MessageBox from NativeAOT...
[watchdog] Found NativeAOT GUI window 00000000000A0388: waiting 3s then auto-closing (or click OK)...
>>> NativeAOT GUI MessageBox Result: 1 (Expected: 1 = IDOK) <<<

====================================================================
  NATIVEAOT MANUAL LOADER VERDICT
    [AOT-01] DataDirectory[14] Absent (No CLR): PASS
    [AOT-02] Win32 TLS Bound for RhpNewFast    : PASS
    [AOT-03] Zero External CLR DLL Imports    : PASS
    [EXP-1]  ABI & Calling Convention (42)    : PASS
    [EXP-2]  RhpNewFast Bump Allocator (9900) : PASS
    [EXP-3]  In-Binary WKS::gc_heap (Allocate): PASS
    [EXP-4]  BCL String Formatting            : PASS
    [EXP-5]  Internal Exception Unwinding (999): PASS
    [EXP-6]  IsDynamicCodeSupported == False  : PASS
    [EXP-7]  GUI MessageBox (IDOK=1)          : PASS
    OVERALL                                   : ALL CHECKS PASSED
====================================================================
```

### Test 2: Mapped Executable Mode (`AOT_Gui.exe`)

```text
====================================================================
  Windows NativeAOT Manual PE Loader — host ARM64
====================================================================
[load] Target: AOT_Gui.exe

--- PE Binary Anatomy (mirrors NativeAOT Research Section 2) ---
Machine                 : 0xAA64 (ARM64 Native)
Magic                   : 0x020B (PE32+)
Binary Type             : Executable Binary (.EXE)
Preferred ImageBase     : 0x0000000140000000
AddressOfEntryPoint RVA : 0x000EA2D8 (Native wmainCRTStartup)
COM Descriptor (dir 14) : RVA 0x00000000 Size 0x00000000
  CLI Header Status     : ABSENT (0x00000000)                [AOT-01 OK]
  Kernel Perception     : Pure Win32 Unmanaged Image          [AOT-01 OK]
TLS Directory (dir 9)   : RVA 0x00142D80 Size 0x00000028
  Native Win32 TLS      : POPULATED (TEB allocator context)  [AOT-02 OK]

Mapped Base             : 0x0000000140000000 (size 0x18F000)
Relocation Delta        : 0
[reloc] Base relocations applied: 8001 (padding: 33)
[imports] Resolving import directory dependencies:
    * Hooked KERNEL32.dll!CreateThread -> LoaderCreateThreadHook (auto-TLS attach)
    * Hooked api-ms-win-crt-runtime-l1-1-0.dll!_exit -> LoaderCrtExitHook
    * Hooked api-ms-win-crt-runtime-l1-1-0.dll!exit -> LoaderCrtExitHook
[imports] Zero CLR dependencies confirmed (pure OS & UCRT links) [AOT-03 OK]
[tls] Bound TEB->ThreadLocalStoragePointer[4] (TEB + 0x58 with 0x10 header)
[ldr] Registered LDR ghost entry & RB-tree node (path: "C:\test_nativeaot\AOT_Gui.exe")
[pdata] KiUserInvertedFunctionTable at 00007FF94A33B4D0 (current=17, max=512)
[pdata] Inserted entry at index 0: ImageBase=0000000140000000, Size=0x18F000
[tls] Executing TLS callback (DLL_PROCESS_ATTACH)...
[load] Mapping complete; ready for DllMain initialization

>>> STEP 1: Launching Mapped AOT Executable Thread <<<
[entry] Thread starting at AddressOfEntryPoint RVA 0x000EA2D8...
====================================================================
  AOT_Gui.exe - NativeAOT Standalone Executable (ARM64)
====================================================================
[AOT_Gui] Running in Process: NativeAotLoader (PID=10752)
[AOT_Gui] Architecture      : Arm64
[AOT_Gui] OS Description    : Microsoft Windows 10.0.26200
[AOT_Gui] Framework         : .NET 10.0.8
[AOT_Gui] Dynamic Code      : False (AOT Invariant)
[AOT_Gui] GC Heap Memory    : 49176 -> 573488 bytes (bump allocation OK)
[AOT_Gui] Displaying native Win32 GUI MessageBox (timeout=4000ms)...
[AOT_Gui] MessageBox DialogResult = 1 (1=IDOK, 32000=TIMEOUT)
[AOT_Gui] Exiting wmain with status 0...
====================================================================

[loader] Intercepted CRT exit(0) from mapped EXE -> calling ExitThread

>>> Mapped EXE Thread Terminated. Exit Code: 0 <<<

====================================================================
  NATIVEAOT MANUAL LOADER VERDICT (EXE MODE)
    [AOT-01] DataDirectory[14] Absent (No CLR): PASS
    [AOT-02] Win32 TLS Bound for RhpNewFast    : PASS
    [AOT-03] Zero External CLR DLL Imports    : PASS
    [EXE-01] Mapped Execution Completed       : PASS
    [EXE-02] ExitProcess Interception         : PASS
    OVERALL                                   : ALL CHECKS PASSED
====================================================================
```
