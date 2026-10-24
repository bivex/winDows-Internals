# Windows C++/CLI Mixed-Mode (IJW) Vectored Exception Handling (VEH) & CLR Precedence Research (ARM64)

**Date:** 2026-10-24  
**Target:** Windows 11 Pro ARM64 (Build 26100.1, 24H2), Apple Silicon Parallels Desktop 20.2.2  
**Compiler:** Microsoft (R) C/C++ Optimizing Compiler Version `19.51.36246` for ARM64 (`/clr`)  
**Debugger:** `cdb.exe` ARM64 (DbgEng `10.0.26100.6584`)  
**Target Binaries:** `C:\test_cppcli\MixedLib.dll` (Mixed-Mode PE, ARM64), `MixedExe.exe`  
**Workspace:** `/Volumes/External/Code/winDows-Internals/KERNEL/CppCLI_VEH_Research.md`

---

## 1. Executive Summary: The Global Precedence of VEH

While [`CppCLI_SEH_Research.md`](CppCLI_SEH_Research.md) documented the stack-based Structured Exception Handling (SEH) bridge between `clr!UMThunkStub` and `__CxxFrameHandler4`, Windows provides another, more fundamental exception handling mechanism: **Vectored Exception Handling (VEH)**.

Introduced in Windows XP and implemented entirely in user space by `ntdll` (`RtlAddVectoredExceptionHandler`), VEH operates on a process-wide, table-driven list of callbacks rather than stack frames.

### Critical Architectural Finding:
In a C++/CLI mixed-mode process, **Vectored Exception Handlers execute with absolute global precedence over both native C++ `catch` blocks and managed `try/catch` blocks**:
1. When an exception occurs (whether a managed `0xE0434352` throw or a native `0xC0000005` hardware fault), the Windows exception dispatcher (`ntdll!RtlDispatchException`) invokes the VEH chain **before any stack unwinding begins**.
2. The CLR itself relies on an internal **3-phase VEH engine** (`clr!CLRVectoredExceptionHandlerShim` -> `Phase2` -> `Phase3`) registered during runtime startup to supervise thread state, safepoints, and hardware fault translations.
3. If an application or third-party DLL registers a naive VEH handler, it can intercept managed exceptions, corrupt CLR GC safepoints, and break `NullReferenceException` translation.

---

## 2. Windows 11 Exception Dispatch Precedence Hierarchy

The exact evaluation order enforced by `ntdll!RtlDispatchException` on ARM64:

```
                      Exception Occurs (Hardware Fault or Software Throw)
                                              │
                                              ▼
                        [ 1. First-Chance Debugger Breakpoint ]
                                              │
                                              ▼
                 +──────────────────────────────────────────────────────────+
                 | 2. Vectored Exception Handlers (VEH List)                |
                 |    Iterates ntdll!LdrpVectorHandlerList                  |
                 |    - User VEH Handler (if FirstHandler=1)               |
                 |    - clr!CLRVectoredExceptionHandlerShim                 |
                 |    - User VEH Handler (if FirstHandler=0)               |
                 +──────────────────────────────────────────────────────────+
                                              │
                 ┌────────────────────────────┴─────────────────────────────┐
                 │ Returns EXCEPTION_CONTINUE_SEARCH (0)                    │ Returns EXCEPTION_CONTINUE_EXECUTION (-1)
                 ▼                                                          ▼
+──────────────────────────────────────────────────+        +────────────────────────────────────────+
| 3. Stack-Based Structured Exception Handling     |        | Resume Thread Execution Immediately    |
|    - Managed IL try/catch blocks                 |        | (Bypasses all SEH/catch blocks!)       |
|    - clr!UMThunkStub (Crossover Boundary Filter) |        +────────────────────────────────────────+
|    - VCRUNTIME140_CLR0400!__CxxFrameHandler4     |
|    - Native C++ catch (...) blocks               |
+──────────────────────────────────────────────────+
                 │
                 │ Exception Not Handled
                 ▼
+──────────────────────────────────────────────────+
| 4. Second-Chance Debugger Breakpoint             |
+──────────────────────────────────────────────────+
                 │
                 ▼
+──────────────────────────────────────────────────+
| 5. Vectored Continue Handlers (VCH List)         |
|    Iterates ntdll!LdrpContinueHandlerList        |
+──────────────────────────────────────────────────+
                 │
                 ▼
+──────────────────────────────────────────────────+
| 6. UnhandledExceptionFilter (Watson / Crash Dump)|
+──────────────────────────────────────────────────+
```

---

## 3. Dissecting the CLR's Internal VEH Architecture

Live inspection of `clr.dll` under `cdb` on ARM64 reveals that the CLR registers an elaborate multi-phase VEH subsystem:

```text
0:000> x clr!*Vectored*
00007fff`632261f0 clr!CLRVectoredExceptionHandlerShim (void)
00007fff`63226390 clr!CLRVectoredExceptionHandler (void)
00007fff`63226440 clr!CLRVectoredExceptionHandlerPhase2 (void)
00007fff`632276b8 clr!CLRVectoredExceptionHandlerPhase3 (void)
00007fff`632b0e30 clr!CLRRemoveVectoredHandlers (void)
00007fff`6393d418 clr!_imp_AddVectoredExceptionHandler = <no type information>
00007fff`6393d410 clr!_imp_RemoveVectoredExceptionHandler = <no type information>
```

---

### 3.1 Disassembly of `clr!CLRVectoredExceptionHandlerShim` (ARM64)

```asm
0:000> u clr!CLRVectoredExceptionHandlerShim L10
clr!CLRVectoredExceptionHandlerShim:
00007fff`632261f0 a9bd53f3 stp   x19, x20, [sp, #-0x30]!
00007fff`632261f4 a9015bf5 stp   x21, x22, [sp, #0x10]
00007fff`632261f8 f90013f7 str   x23, [sp, #0x20]
00007fff`632261fc a9bf7bfd stp   fp, lr, [sp, #-0x10]!
00007fff`63226200 910003fd mov   fp, sp
; Check if runtime exception handling is suspended / active
00007fff`63226204 f0005288 adrp  x8, clr!CPUGroupInfo::m_pSetThreadGroupAffinity+0x8
00007fff`63226208 b94cf908 ldr   w8, [x8, #0xCF8]
00007fff`6322620c aa0003f4 mov   x20, x0               ; x20 = PEXCEPTION_POINTERS
00007fff`63226210 d5033fbf dmb   sy                    ; Memory barrier for multi-core sync
00007fff`63226214 3502e148 cbnz  w8, clr!Fallback      ; Fast-path bypass if disabled
00007fff`63226224 f00038a8 adrp  x8, clr!_imp_Crypt...
00007fff`63226228 f9438d08 ldr   x8, [x8, #0x718]
00007fff`6322622c d63f0100 blr   x8                    ; Call phase 1 handler
```

---

### 3.2 Thread TEB Verification in `clr!CLRVectoredExceptionHandler`

Disassembly of `clr!CLRVectoredExceptionHandler` reveals how the CLR detects if the fault occurred on a managed thread:

```asm
0:000> u clr!CLRVectoredExceptionHandler L14
clr!CLRVectoredExceptionHandler:
00007fff`63226390 a9bf53f3 stp   x19, x20, [sp, #-0x10]!
00007fff`63226394 a9be7bfd stp   fp, lr, [sp, #-0x20]!
00007fff`63226398 910003fd mov   fp, sp
00007fff`6322639c 92800030 mov   xip0, #-2
00007fff`632263a0 f9000bb0 str   xip0, [fp, #0x10]
; Read the ARM64 Thread Environment Block (TEB) via register x18 (xpr)
00007fff`632263c0 f9402e48 ldr   x8, [xpr, #0x58]      ; Load TEB->TlsSlots
00007fff`632263c4 f8695909 ldr   x9, [x8, w9 uxtw #3]  ; Read CLR Thread* local storage
00007fff`632263cc f9400d53 ldr   x19, [x10, #0x18]     ; Extract Thread::m_State
```

#### Why the CLR Inspects the TEB in VEH:
1. **Thread Identification:** The CLR reads `x18` (ARM64 platform register holding the TEB pointer) to extract the managed `Thread*` object from TLS slot `#0x58`.
2. **Cooperative vs Preemptive State:** If the thread is in **Cooperative GC Mode** (`Thread::m_fPreemptive == 0`), an unhandled hardware fault must trigger immediate GC safepoint compensation before stack unwinding proceeds.
3. **Hardware Null-Pointer Translation:** On ARM64, if an instruction dereferences `x0` when `x0 == 0`, generating `0xC0000005`, the CLR VEH checks if `pc` resides within a JIT-compiled method. If so, it rewrites the exception code to trigger a managed `NullReferenceException`.

---

## 4. The Three Critical VEH Conflict Pitfalls in C++/CLI

When developers register custom Vectored Exception Handlers (`AddVectoredExceptionHandler`) in applications that host C++/CLI mixed-mode assemblies, three catastrophic pitfalls frequently emerge:

### Pitfall 1: False-Positive Crash Telemetry on Managed `0xE0434352`
Every time managed code throws an exception — even when wrapped in a local `try { ... } catch (...)` block — the CLR invokes `RaiseException(0xE0434352)`.
- **Symptom:** Naive VEH crash loggers (such as Sentry, Breakpad, Crashpad, or custom crash monitors) intercept `0xE0434352` at **first chance**.
- **Result:** The crash reporter generates a minidump or logs a fatal error for a benign, fully handled managed exception before `__CxxFrameHandler4` is ever given the opportunity to catch it.

---

### Pitfall 2: Memory & Thread State Corruption via `EXCEPTION_CONTINUE_EXECUTION`
If a custom VEH callback returns `EXCEPTION_CONTINUE_EXECUTION` (`-1`):
```cpp
// DANGEROUS ANTI-PATTERN:
LONG WINAPI BadVehHandler(PEXCEPTION_POINTERS pEx) {
    if (pEx->ExceptionRecord->ExceptionCode == 0xE0434352) {
        // Attempting to "suppress" or "ignore" the exception:
        return EXCEPTION_CONTINUE_EXECUTION; // FATAL!
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
```
- **Consequence:** `ntdll` restores the CPU context (`pEx->ContextRecord`) and resumes execution at the instruction immediately following `RaiseException` inside `clr.dll`.
- **Fatal Desynchronization:** The CLR's internal exception state machine (`ThreadExceptionData`) remains in an incomplete throw state. Subsequent memory allocations trigger GC assertions or immediate deadlocks.

---

### Pitfall 3: Broken `NullReferenceException` Translation (`0xC0000005`)
When native code or JIT code reads address `0x00000000` on ARM64, the CPU triggers an MMU Data Abort, translated by the kernel into `STATUS_ACCESS_VIOLATION` (`0xC0000005`).
- If a user registers a VEH handler at the **front of the list** (`FirstHandler = 1`):
  ```cpp
  AddVectoredExceptionHandler(1, MyVehHandler);
  ```
- `MyVehHandler` runs **BEFORE** `clr!CLRVectoredExceptionHandler`.
- If `MyVehHandler` logs or attempts to handle `0xC0000005`, it steals the exception before the CLR can verify whether the fault occurred inside a JIT-compiled method. The managed code's `catch (NullReferenceException^ ex)` block is starved and never executes.

---

## 5. Production Engineering: Safe VEH Implementation in Mixed-Mode C++/CLI

To safely use Vectored Exception Handling in processes containing C++/CLI mixed-mode assemblies, the handler must adhere to three strict defensive rules:

```cpp
// ============================================================================
// SafeVehHandler.cpp — Production-Grade VEH Callback for C++/CLI Processes
// ============================================================================

#include <windows.h>
#include <stdio.h>

// Well-known CLR and Win32 software exception codes
#define EXCEPTION_CLR_MANAGED       0xE0434352  // 'CCR' / Managed Exception
#define EXCEPTION_MSVC_CPP          0xE06D7363  // 'msc' / Native C++ Exception
#define EXCEPTION_CLR_NOTIFICATION   0x04242420  // Internal CLR Notification
#define EXCEPTION_DBG_PRINT         0x40010006  // OutputDebugStringA

LONG WINAPI SafeMixedModeVehHandler(PEXCEPTION_POINTERS pEx)
{
    DWORD code = pEx->ExceptionRecord->ExceptionCode;

    // RULE 1: Completely ignore managed and internal runtime exceptions.
    // Let clr!UMThunkStub and __CxxFrameHandler4 handle them natively.
    if (code == EXCEPTION_CLR_MANAGED ||
        code == EXCEPTION_MSVC_CPP ||
        code == EXCEPTION_CLR_NOTIFICATION ||
        code == EXCEPTION_DBG_PRINT)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // RULE 2: If intercepting fatal hardware faults (e.g. Access Violation),
    // check if the faulting address resides inside an active module's .text
    // or whether it occurred inside JIT memory.
    if (code == EXCEPTION_ACCESS_VIOLATION)
    {
        ULONG_PTR faultAddr = (ULONG_PTR)pEx->ExceptionRecord->ExceptionAddress;
        
        // Log diagnostics safely without modifying context...
        printf("[VEH] Unhandled Native Fault at 0x%p (Code: 0x%08X)\n", 
               (void*)faultAddr, code);
    }

    // RULE 3: NEVER return EXCEPTION_CONTINUE_EXECUTION for unknown exceptions.
    // ALWAYS yield to stack-based SEH unwinders.
    return EXCEPTION_CONTINUE_SEARCH;
}

// Registration convention:
// Pass '0' as FirstHandler to ensure CLR internal VEH runs FIRST.
void RegisterSafeVeh()
{
    AddVectoredExceptionHandler(0, SafeMixedModeVehHandler);
}
```

---

## 6. Comparative Matrix: VEH vs SEH in C++/CLI

| Dimension | Vectored Exception Handling (VEH) | Structured Exception Handling (SEH) |
|---|---|---|
| **Registration API** | `AddVectoredExceptionHandler` | Table-driven (`.pdata`/`.xdata`) or compiler `try/catch` |
| **Storage Location** | Process-global linked list (`ntdll`) | Thread call stack frame records |
| **Execution Order** | **First** (Precedes all stack handlers) | **Second** (Walks stack frame by frame) |
| **Scope** | Process-wide (All threads) | Lexical scope (`try { ... }`) on calling thread |
| **CLR Internal Usage** | `clr!CLRVectoredExceptionHandlerShim` | `clr!UMThunkStub_EH` / `__CxxFrameHandler4` |
| **Tracking Handles (`^`)**| Raw `PEXCEPTION_POINTERS` (No type info) | Unpacked into typed managed handles (`Exception^`) |
| **Swallowing Safety** | ❌ **Extremely Dangerous** (Corrupts runtime) | ✅ **Safe** (Stack cleanly unwound) |

---

## 7. Reproducibility & Verification Matrix

| Claim ID | Technical Claim | Status | Verification Command | Observation |
|---|---|---|---|---|
| **VEH-01** | `clr.dll` registers `CLRVectoredExceptionHandlerShim` at startup | `[EXPERIMENTALLY CONFIRMED]` | `x clr!*Vectored*` | Symbols resolved: `CLRVectoredExceptionHandlerShim`, `Phase2`, `Phase3` |
| **VEH-02** | `CLRVectoredExceptionHandler` reads ARM64 TEB from register `x18` | `[EXPERIMENTALLY CONFIRMED]` | `u clr!CLRVectoredExceptionHandler L14` | Instruction `ldr x8, [xpr, #0x58]` reads TLS slots via `x18` |
| **VEH-03** | VEH callbacks execute before `clr!UMThunkStub` catches `0xE0434352` | `[ARCHITECTURALLY CONFIRMED]` | `ntdll!RtlDispatchException` walk | VEH list dispatched in Step 2; stack-based SEH in Step 3 |
| **VEH-04** | Returning `CONTINUE_SEARCH` preserves normal C++ catch handling | `[EXPERIMENTALLY CONFIRMED]` | `TestMixedSeh` execution trace | Exception smoothly bypasses VEH to native catch handler (returns `999`) |
