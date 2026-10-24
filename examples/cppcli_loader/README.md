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
| `MixedExe.cpp` | Pure native Win32 executable calling `MixedLib.dll`. Multi-threaded validation driver. |
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
MixedExe.exe
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

====================================================================
  All C++/CLI IJW Loader Scenarios Verified Successfully
====================================================================
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
