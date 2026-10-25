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

---

## Files

| File | Description |
|---|---|
| `AotLib/AotLib.csproj` | NativeAOT shared library project (`<NativeLib>Shared</NativeLib>`, `<PublishAot>true</PublishAot>`). |
| `AotLib/AotLib.cs` | C# library declaring 6 unmanaged exports (`[UnmanagedCallersOnly]`) exercising the GC heap, bump allocator, exceptions, and BCL. |
| `NativeAotLoader.cpp` | Manual PE loader in native Win32 C++ executing `AotLib.dll` with zero external CLR dependencies. |
| `build.bat` | Automated compilation script for Visual Studio ARM64/x64 developer environments. |

---

## Build & Run

From a Visual Studio Developer Command Prompt on ARM64:

```cmd
build.bat
```

Or manually:
```cmd
dotnet publish AotLib\AotLib.csproj -c Release -r win-arm64 -p:PublishAot=true
copy /y AotLib\bin\arm64\Release\net10.0\win-arm64\publish\AotLib.dll .
cl /EHsc NativeAotLoader.cpp /Fe:NativeAotLoader.exe
NativeAotLoader.exe AotLib.dll
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

====================================================================
  NATIVEAOT MANUAL LOADER VERDICT
    [AOT-01] DataDirectory[14] Absent (No CLR): PASS
    [AOT-02] Win32 TLS Bound for RhpNewFast    : PASS
    [AOT-03] Zero External CLR DLL Imports    : PASS
    [EXP-1]  ABI & Calling Convention (42)    : PASS
    [EXP-2]  RhpNewFast Bump Allocator (9900) : PASS
    [EXP-3]  In-Binary WKS::gc_heap (Collect) : PASS
    [EXP-4]  BCL String Formatting            : PASS
    [EXP-5]  Internal Exception Unwinding (999): PASS
    [EXP-6]  IsDynamicCodeSupported == False  : PASS
    OVERALL                                   : ALL CHECKS PASSED
====================================================================
```
