# C++/CLI Mixed-Mode (IJW - "It Just Works") Loader Suite

This sample implements the complete empirical verification harness for the C++/CLI IJW loader architecture detailed in:
📄 **[`KERNEL/CppCLI_IJW_Loader_Research.md`](../../KERNEL/CppCLI_IJW_Loader_Research.md)**.

---

## Architecture Overview

```
MixedExe.exe (Native PE)
  │
  ├──► [Call NativeCallsManaged(21)]
  │      │
  │      ▼
  │    MixedLib.dll (Hybrid Native/Managed PE)
  │      ├── VTableFixup Slot [0xA000]
  │      │     ├── State 1 (On Disk): MethodDef Token (0x06000001)
  │      │     ├── State 2 (Unresolved): IJW Transition Thunk (0x000001d3`ce502058)
  │      │     └── State 3 (Resolved): JIT Native Code (0x00007fff`8af10020)
  │      │
  │      ▼
  │    ManagedClass::Compute(21) [CIL -> RyuJIT]
  │      │
  │      ▼ (Reverse Crossover: Managed -> Native without P/Invoke stub)
  │    NativeHelper(21) [Pure C++ Machine Code]
  │      │
  │      └── Returns 42
  │
  ├──► [Call TestMixedSeh()]
  │      │
  │      ├── ManagedClass::ThrowException() [throw gcnew InvalidOperationException]
  │      │     └── clr!IL_Throw -> RaiseException(0xE0434352)
  │      │
  │      └── Intercepted by clr!UMThunkStub -> Native C++ catch block -> Returns 999
  │
  └──► [Call TestBothTls() across Main & Worker Threads]
         │
         ├── Native Win32 TLS: __declspec(thread) int g_nativeThreadLocal
         │     └── Cloned per-thread by OS loader (ntdll!LdrpInitializeTls)
         │
         └── Managed CLR TLS: [ThreadStatic] int s_managedThreadLocal
               └── Allocated via clr!JIT_GetNonGCThreadStaticBase_Helper (.cctor runs once)
```

---

## Files

| File | Description |
|---|---|
| `MixedLib.cpp` | Hybrid C++/CLI library compiled with `cl /clr /LD`. Declares managed ref classes, native exported functions, dual TLS, and SEH handlers. |
| `MixedExe.cpp` | Pure native Win32 executable calling `MixedLib.dll` via the OS loader (`LoadLibrary` baseline). Multi-threaded validation driver. |
| `CppCliLoader.cpp` | **Manual (usermode) PE loader** for the mixed DLL: no `LoadLibrary`, no import lib. Implements COR20 validation, dual-layer relocation (Layer-1 DIR64 with Layer-2 VTableFixup exclusion), IAT resolution, TLS template cloning, PEB→Ldr ghost registration + `LdrpModuleBaseAddressIndex` RB-tree insertion via exported `ntdll!RtlRbInsertNodeEx`, the `_CorDllMain` hijack call, and live 3-state slot lifecycle observation. |
| `build.bat` | Automated compilation script for Visual Studio ARM64/x64 developer environments. |

---

## Build & Run

From a Visual Studio Developer Command Prompt (Native ARM64 or x64):

```cmd
build.bat
```
Or manually:
```cmd
cl /clr /LD MixedLib.cpp /Fe:MixedLib.dll
cl /EHsc MixedExe.cpp MixedLib.lib /Fe:MixedExe.exe
cl /EHsc CppCliLoader.cpp /Fe:CppCliLoader.exe
MixedExe.exe
CppCliLoader.exe MixedLib.dll
```

---

## Expected Output

```text
====================================================================
  Windows C++/CLI Mixed-Mode (IJW) Loader & Interop Verification
====================================================================

>>> EXPERIMENT 1: Forward & Reverse Crossover <<<
[MixedLib!NativeCallsManaged] entering from native, calling ManagedClass::Compute(21)...
  [ManagedClass::Compute] entering managed context, input = 21
    [NativeHelper] pure native function executing, x = 21
  [ManagedClass::Compute] native helper returned: 42
[MixedLib!NativeCallsManaged] returned from managed: 42
>>> Result returned to NativeExe: 42 (Expected: 42) <<<

>>> EXPERIMENT 2: Mixed-Mode SEH Exception Unwinding <<<
[MixedLib!TestMixedSeh] entering native try block...
  [ManagedClass::ThrowException] throwing System::InvalidOperationException...
[MixedLib!TestMixedSeh] CAUGHT managed exception in native handler: System.InvalidOperationException
>>> SEH Handler Result: 999 (Expected: 999) <<<

>>> EXPERIMENT 3: Dual-TLS (Native vs Managed) <<<

[Main Thread - Pass 1]
[MixedLib!TestBothTls] OS Thread 0x1A28 (ID 1): g_nativeThreadLocal = 1235 (&g_native = 000001686110A094)
  [ManagedClass] Thread 1: s_managedThreadLocal = 5679

--- [Worker Thread 2] Starting Execution ---
[MixedLib!TestBothTls] OS Thread 0x20D8 (ID 2): g_nativeThreadLocal = 1236 (&g_native = 000001686119AF74)
  [ManagedClass] Thread 2: s_managedThreadLocal = 2

[Main Thread - Pass 2 (Revisit)]
[MixedLib!TestBothTls] OS Thread 0x1A28 (ID 1): g_nativeThreadLocal = 1236 (&g_native = 000001686110A094)
  [ManagedClass] Thread 1: s_managedThreadLocal = 5680

>>> EXPERIMENT 4: Pointer Anatomy (gcroot, pin_ptr, interior_ptr) <<<
[MixedLib!TestPointerAnatomy] Starting Pointer Anatomy Test...
  [gcroot] NativeHolder at 0000027E8A12ACE0, sizeof(gcroot) = 8, nativeId = 42
  [pin_ptr] Managed array pinned at 0000027E8B046988, raw value = 12345
  [interior_ptr] Interior pointer at 0000027E8B04698C, value = 67890
>>> Pointer Anatomy Result: 1 (Expected: 1) <<<

====================================================================
  All C++/CLI IJW Loader Scenarios Verified Successfully
====================================================================

---

## CppCliLoader.exe — Manual Mapping of a Mixed-Mode DLL

`CppCliLoader.exe` maps `MixedLib.dll` **without `LoadLibrary`**, reproducing
what `ntdll` does for an IJW image and demonstrating which loader services the
CLR hard-requires:

| Stage | Implementation | Research reference |
|---|---|---|
| COR20 validation | `ILONLY == 0`, `NATIVE_ENTRYPOINT` set, `VTableFixups` enumerated | CPP-01/02/03 |
| Section mapping | `VirtualAlloc` + raw copy + per-section `VirtualProtect` (W^X) | — |
| Layer-1 relocations | DIR64 walk **excluding** every VTableFixup slot RVA (Layer 2 belongs to the CLR) | `CppCLI_Relocations_Research.md` |
| Import resolution | `LoadLibraryA`/`GetProcAddress` — pulls in `mscoree.dll` (the `_CorDllMain` IAT hijack target) | `CppCLI_Dual_Entry_Points_CorDllMain_Research.md` |
| TLS | `TlsAlloc` index stored into `__tls_index`, per-thread template clone (`ntdll!LdrpInitializeTls` analog) | CPP-09 |
| LDR registration | Clone of a real `LDR_DATA_TABLE_ENTRY`, linked into `InLoadOrder`/`InMemoryOrder` lists **and** inserted into the private `LdrpModuleBaseAddressIndex` RB-tree via exported `ntdll!RtlRbInsertNodeEx` | required by `mscoreei!GetRuntimeForHModule → GetModuleFileNameW` |
| Entry hijack | call `AddressOfEntryPoint` → 3-instruction stub tail-jumps to `mscoree!_CorDllMain` (CLR installs State-2 thunks during attach) | CPP-04/05 |
| Slot lifecycle | dump slots pre-attach (tokens), post-attach (heap thunks), post-first-call (JIT addresses) | CPP-05/06 |

### Key empirical findings baked into the loader

1. **Linked lists are not enough.** A ghost entry in `InLoadOrderModuleList`
   does not make handle→module lookups succeed; `GetModuleFileNameW` resolves
   through the `LdrpModuleBaseAddressIndex` RB-tree (a private ntdll global).
   The tree is located at runtime by scanning ntdll `.data` for a validated
   `(Root, Min)` pair of node pointers collected from the module list
   (node offset `+0xC8` on Win11 26100 ARM64).
2. **Hand-built LDR entries crash.** The loader dereferences undocumented
   fields (`DdagNode` etc.); the entry is therefore cloned wholesale from
   `ntdll`'s entry, with identity fields patched and unjoined links zeroed.
   An incomplete entry AVs inside `LdrGetDllFullName`, and the AV then
   deadlocks in `UnhandledExceptionFilter → LoadLibraryExW` under the loader
   lock.
3. **Native TLS divergence (documented, not fixable from the loader):** the
   `/clr`-compiled access to the RVA-mapped `__declspec(thread)` field goes
   through the CLR's own per-thread RVA-static machinery against its private
   PEImage copy of the assembly, so the 1234 template is not applied under
   manual mapping (observed `1/2/2` vs `1235/1236/1236` under `LoadLibrary`).
   The managed `[ThreadStatic]` divergence (`5679/2/5680`) is unaffected.

### Manual-loader verdict block (observed on Win11 26100 ARM64)

```text
STATE 1  slot @ RVA 0x0000B000 -> 0x0000000006000001  MethodDef token
STATE 2  slot @ RVA 0x0000B000 -> 0x00000222C6502058  heap pointer -> IJW transition thunk
STATE 3  slot @ RVA 0x0000B000 -> 0x00007FF8686F0020  high VA -> JIT/native code

VERDICT
  [CPP-03/05] token   -> IJW thunk  transition : OBSERVED
  [CPP-05]    thunk   -> JIT code  transition : OBSERVED
  [EXP-1]     NativeCallsManaged(21) == 42    : PASS
  [EXP-2]     TestMixedSeh()        == 999    : PASS
  [EXP-4]     TestPointerAnatomy()  == 1      : PASS
  [EXP-5]     TestGuiMessageBox()   == 1 (OK) : PASS
  OVERALL                                       : ALL CHECKS PASSED
```

---

## WinDbg / CDB Inspection Recipes

### 1. Observe In-Place VTable Back-Patching
```cmd
cdb -c "sxe ld:MixedLib.dll; g; dp MixedLib+0xA000 L1; bp MixedLib!NativeCallsManaged; g; dp MixedLib+0xA000 L1; q" MixedExe.exe
```

### 2. Disassemble IJW Transition Thunk (State 2)
```cmd
cdb -c "sxe ld:MixedLib.dll; g; u poi(MixedLib+0xA000) L10; q" MixedExe.exe
```

### 3. Trace SEH Unwind through `clr!UMThunkStub`
```cmd
cdb -c "sxe 0xe0434352; g; k 15; q" MixedExe.exe
```

### 4. Trace Managed `[ThreadStatic]` Access
```cmd
cdb -c "sxe ld:clr.dll; g; bu clr!JIT_GetNonGCThreadStaticBase_Helper; g; k 10; q" MixedExe.exe
```
