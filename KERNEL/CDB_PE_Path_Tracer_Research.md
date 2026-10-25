# Windows PE Invocation Vectors & DbgEng Call Path Tracing Architecture (ARM64)

**Date:** 2026-10-26  
**Target:** Windows 11 Pro ARM64 (Build 26200.1, 24H2), Parallels Desktop 20.2.2 on Apple Silicon  
**Toolchain:** MSVC Optimizing Compiler Version `19.51.36246` for ARM64 (`/std:c++20`), Windows SDK `10.0.26100.0` (DbgEng SDK)  
**Debugger:** `cdb.exe` ARM64 (DbgEng `10.0.26100.6584`)  
**Target Plugin:** `petrace.dll` (DbgEng Native Extension)  
**Workspace:** `/Volumes/External/Code/winDows-Internals/KERNEL/CDB_PE_Path_Tracer_Research.md`  

---

## 1. Executive Summary: The PE Invocation Surface Problem

In reverse engineering, vulnerability research, and binary security verification, a critical question is:  
**"What are all possible execution paths into and through this Portable Executable (PE) binary?"**

Standard reverse engineering workflows often focus exclusively on the primary entry point (`IMAGE_OPTIONAL_HEADER.AddressOfEntryPoint`, e.g. `mainCRTStartup` or `_DllMainCRTStartup`). However, in modern Windows 11 operating systems—spanning pure native C++, COM in-process servers, C++/WinRT components, C++/CLI mixed-mode assemblies, and NativeAOT binaries—code execution can be triggered through **six distinct architectural vectors**:

1. **`AddressOfEntryPoint`:** The canonical OS loader entry point (`ntdll!LdrpCallInitRoutine`).
2. **TLS Callbacks (`IMAGE_DIRECTORY_ENTRY_TLS`):** Executed by `ntdll!LdrpCallTlsInitializers` **before** the main entry point runs on both process creation and thread attachment.
3. **Export Address Table (`IMAGE_DIRECTORY_ENTRY_EXPORT`):** Callable externally via `GetProcAddress`, COM activation (`DllGetClassObject`, `DllGetActivationFactory`), or static import linking.
4. **Control Flow Guard (CFG) Valid Target Table (`IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG`):** Indirect branch destinations (`blr xn` / `call rax`) validated by the OS kernel and `ntdll!LdrpValidateUserCallTarget`.
5. **Structured Exception Handling (SEH) Personality & Filter Routines (`IMAGE_DIRECTORY_ENTRY_EXCEPTION` / `.pdata`):** Unwind tables on ARM64 mapping exception scopes, landing pads, and personality handlers (`__CxxFrameHandler4`, `__C_specific_handler`).
6. **CLI Header & VTable Fixups (`IMAGE_DIRECTORY_ENTRY_COMHEADER`):** In C++/CLI mixed-mode assemblies, native-to-managed transition thunks (`VTableFixups`) that intercept calls into managed IL bytecode.

This research documents the architecture and implementation of **`petrace.dll`**, a native **DbgEng extension plugin for `cdb.exe` and `windbg.exe` on ARM64**. The plugin interrogates the running process's virtual memory via the Microsoft Debugger Engine (`IDebugDataSpaces`, `IDebugControl`, `IDebugSymbols`), extracts all six invocation vectors, and constructs static Control Flow Graphs (CFGs) of basic blocks and branching paths.

```
+───────────────────────────────────────────────────────────────────────────────+
|               THE SIX PE INVOCATION VECTORS ON WINDOWS 11 ARM64               |
+───────────────────────────────────────────────────────────────────────────────+

                 Windows OS Loader (ntdll!LdrpInitializeProcess)
                                        │
    ┌───────────────────────────────────┼───────────────────────────────────┐
    ▼                                   ▼                                   ▼
[VECTOR 2: TLS Callbacks]    [VECTOR 1: Entry Point]      [VECTOR 3: Export Table]
• Directory[9] (TLS)         • AddressOfEntryPoint        • Directory[0] (Exports)
• Executes BEFORE DllMain!   • DllMain / mainCRTStartup   • DllGetClassObject
• Persistence & Evasion      • ntdll!LdrpCallInitRoutine  • DllGetActivationFactory
    │                                   │                                   │
    ├───────────────────────────────────┼───────────────────────────────────┤
    ▼                                   ▼                                   ▼
[VECTOR 4: CFG Call Targets] [VECTOR 5: SEH .pdata]       [VECTOR 6: CLI / VTable]
• Directory[10] (LoadConfig) • Directory[3] (.pdata)      • Directory[14] (COM)
• GuardCFFunctionTable       • RUNTIME_FUNCTION entries   • VTableFixups Thunks
• Legal indirect calls       • Unwind Personality Handlers• Native <-> Managed IJW
```

---

## 2. Anatomy of the Six Invocation Vectors

### 2.1 Vector 1: `AddressOfEntryPoint`
* **PE Header Location:** `IMAGE_NT_HEADERS64.OptionalHeader.AddressOfEntryPoint` (RVA).
* **Virtual Address:** `ImageBase + AddressOfEntryPoint`.
* **Invocation Mechanism:** For EXEs, invoked by `KERNEL32!BaseThreadInitThunk`; for DLLs, invoked by `ntdll!LdrpCallInitRoutine` with `DLL_PROCESS_ATTACH (1)`.
* **Analysis Challenge:** Packaged apps and .NET assemblies frequently point this to a generic runtime shim (`mscoree!_CorExeMain` or CRT trampoline) that redirects execution through secondary tables.

### 2.2 Vector 2: Thread Local Storage (TLS) Callbacks
* **PE Header Location:** `IMAGE_NT_HEADERS64.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS (9)]`.
* **Internal Structure:** `IMAGE_TLS_DIRECTORY64.AddressOfCallBacks` points to an array of function pointers (`PIMAGE_TLS_CALLBACK*`) terminated by a null pointer.
* **Execution Precedence:**  
  $$\text{TLS Callbacks} \prec \text{AddressOfEntryPoint}$$
  On process startup, `ntdll!LdrpCallTlsInitializers` iterates through `AddressOfCallBacks` and executes each callback before `DllMain` or `main` is called. This represents a primary vector for early process instrumentation, anti-debugging, and initialization.

### 2.3 Vector 3: Export Address Table (EAT)
* **PE Header Location:** `IMAGE_NT_HEADERS64.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT (0)]`.
* **Internal Structure:** `IMAGE_EXPORT_DIRECTORY` contains:
  * `AddressOfFunctions`: Array of export RVAs.
  * `AddressOfNames`: Array of string RVAs for named exports.
  * `AddressOfNameOrdinals`: Mapping array connecting names to function table indices.
* **Invocation Mechanism:** Dynamically resolved by consumers via `GetProcAddress(hMod, "ExportName")` or COM class factories (`DllGetClassObject`, `DllGetActivationFactory`).
* **Forwarders:** If an export RVA falls within the bounds of the Export Directory (`[exportDir.VA, exportDir.VA + exportDir.Size)`), it is not code; it is an ASCII forwarder string (e.g. `NTDLL.RtlAllocateHeap`).

### 2.4 Vector 4: Control Flow Guard (CFG) Valid Target Table
* **PE Header Location:** `IMAGE_NT_HEADERS64.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG (10)]`.
* **Internal Structure:** `IMAGE_LOAD_CONFIG_DIRECTORY64` fields:
  * `GuardCFFunctionTable`: Virtual address of the sorted RVA table of valid indirect call targets.
  * `GuardCFFunctionCount`: Number of entries in the table.
  * `GuardFlags`: `IMAGE_GUARD_CF_INSTRUMENTED (0x00000100)`, `IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT (0x00000400)`.
* **Security Significance:** Any function whose address is taken (e.g. virtual method tables, function pointers, callbacks) MUST be declared in this table. An indirect call (`blr x16`) targeting an address not in this bitmap triggers `STATUS_STACK_BUFFER_OVERRUN` / `STATUS_FAST_FAIL` (`0xC0000409`, subcode `FAST_FAIL_CONTROL_FLOW_GUARD_CHECK_FAILURE`).

### 2.5 Vector 5: Structured Exception Handling (.pdata) Handlers
* **PE Header Location:** `IMAGE_NT_HEADERS64.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION (3)]`.
* **Internal Structure (ARM64):** An array of `IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY` (8 bytes each):
  ```cpp
  typedef struct _IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY {
      DWORD BeginAddress; // Function RVA
      union {
          DWORD UnwindData;
          struct {
              DWORD Flag : 2;
              DWORD FunctionLength : 11;
              DWORD RegF : 3;
              DWORD RegI : 4;
              DWORD H : 1;
              DWORD CR : 2;
              DWORD FrameSize : 9;
          } PackedUnwindData;
      };
  } IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY;
  ```
* **Invocation Mechanism:** When a CPU exception occurs (`0xC0000005` Access Violation, `0xE0434352` CLR Exception), `ntdll!RtlDispatchException` binary searches the `.pdata` table via `ntdll!RtlLookupFunctionEntry` to locate personality routines (`__CxxFrameHandler4`, `__C_specific_handler`) and landing pad exception filters.

### 2.6 Vector 6: CLI Header & VTable Fixup Descriptors
* **PE Header Location:** `IMAGE_NT_HEADERS64.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COMHEADER (14)]`.
* **Internal Structure:** `IMAGE_COR20_HEADER`:
  * `Flags`: `COMIMAGE_FLAGS_ILONLY (1)`, `COMIMAGE_FLAGS_32BITREQUIRED (2)`, `COMIMAGE_FLAGS_NATIVE_ENTRYPOINT (0x10)`.
  * `VTableFixups`: RVA and Size of an array of `IMAGE_COR_VTABLEFIXUP` descriptors.
* **C++/CLI Mixed-Mode Invocation:** Each `VTableFixup` slot is initially loaded with a managed MethodDef token (`0x06000001`). At startup, `mscoree!_CorDllMain` replaces the slot with an **IJW Transition Thunk** (`adrp / ldr / br x9`). Calling this slot transitions the thread from native machine code into the CLR JIT execution engine.

---

## 3. DbgEng Extension Architecture (`petrace.dll`)

A Microsoft Debugger Extension DLL integrates directly into `cdb.exe` and `windbg.exe` via the **DbgEng COM interfaces**:

```
┌────────────────────────────────────────────────────────────────────────┐
│                        DbgEng Engine (dbgeng.dll)                      │
│                                                                        │
│  [Client: IDebugClient4] ──► QueryInterface:                           │
│                                • IDebugControl4      (Output, Disasm)  │
│                                • IDebugDataSpaces4   (Virtual Memory)  │
│                                • IDebugSymbols3      (Symbol Names)    │
│                                • IDebugSystemObjects (Processes/Threads)│
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│                    petrace.dll Extension Entry Points                  │
│                                                                        │
│  1. DebugExtensionInitialize(&Version, &Flags)                         │
│  2. DebugExtensionUninitialize()                                       │
│  3. pepaths(Client, Args) ──► Complete 6-Vector PE Invocation Analysis │
│  4. pecfg(Client, Args)   ──► Basic Block CFG & Branch Tree Generator  │
│  5. pehelp(Client, Args)  ──► User Command Help & Syntax Reference     │
└────────────────────────────────────────────────────────────────────────┘
```

### 3.1 Resolving Virtual Memory via `IDebugDataSpaces4`
Because debugger extensions execute in the debugger process (not the target process), all memory reads must route through:
```cpp
HRESULT hr = pDataSpaces->ReadVirtual(
    (ULONG64)targetVirtualAddress,
    pLocalBuffer,
    bufferSize,
    &bytesRead
);
```
This guarantees safe reading of memory without access violations even if pages are guarded or uncommitted.

### 3.2 Basic Block & Branch Control Flow Graph (CFG) Algorithm
To reconstruct all execution paths through an individual function, `petrace` implements an opcode analyzer:
1. Start at Function Entry `VA`.
2. Disassemble sequential instructions via `IDebugControl4::Disassemble`.
3. Detect branch instructions on ARM64:
   * **Unconditional Direct Branch (`b <target>`):** Terminates current basic block. Adds directed edge `CurrentBlock -> TargetBlock`.
   * **Conditional Branch (`b.eq`, `b.ne`, `b.gt`, `cbz`, `cbnz`, `tbz`, `tbnz`):** Terminates basic block. Adds two directed edges:
     * `Edge 1 (Taken): CurrentBlock -> TargetBlock`
     * `Edge 2 (Fallthrough): CurrentBlock -> NextInstruction`
   * **Function Call (`bl <target>`):** Records call edge. Does not terminate basic block.
   * **Indirect Branch / Jump Table (`br xn`):** Marks dynamic indirect jump (switch statement, vtable slot).
   * **Indirect Call (`blr xn`):** Marks dynamic indirect call (CFG protected dispatch).
   * **Function Return (`ret`):** Terminal node of control flow path.
4. Render output as an ASCII execution graph or Mermaid flowchart diagram.

---

## 4. Falsification & Reproducibility Matrix

| Claim ID | Technical Claim | Status | Verification Command | Raw Empirical Evidence | Falsification Condition |
|---|---|---|---|---|---|
| **PETRACE-01** | `petrace.dll` loads in `cdb.exe` on ARM64 | `[CONFIRMED]` | `cdb -c ".load petrace.dll; !pehelp; q" calc.exe` | Output displays extension banner and commands | Load failure or architecture mismatch error. |
| **PETRACE-02** | `!pepaths` identifies `AddressOfEntryPoint` with disassembly | `[CONFIRMED]` | `!pepaths ComComponent.dll` | Displays Entry Point RVA, absolute VA, first instructions | Failure to read PE headers from memory. |
| **PETRACE-03** | `!pepaths` parses all Export Address Table (EAT) entries | `[CONFIRMED]` | `!pepaths ComComponent.dll` | Resolves `DllGetClassObject`, `DllCanUnloadNow`, etc. | Missing exported symbols or incorrect RVAs. |
| **PETRACE-04** | `!pepaths` detects presence/absence of TLS Callbacks | `[CONFIRMED]` | `!pepaths <binary>` | Correctly indicates Directory[9] status and callbacks | False positive when TLS directory is absent. |
| **PETRACE-05** | `!pepaths` discovers CFG `GuardCFFunctionTable` entries | `[CONFIRMED]` | `!pepaths ComComponent.dll` | Displays LoadConfig Directory[10] & Guard CF targets | Inability to parse LoadConfig directory. |
| **PETRACE-06** | `!pepaths` parses ARM64 `.pdata` runtime function entries | `[CONFIRMED]` | `!pepaths ComComponent.dll` | Displays Directory[3] count of `RUNTIME_FUNCTION` entries | Failure to parse ARM64 exception table. |
| **PETRACE-07** | `!pepaths` analyzes CLI COM header Directory[14] | `[CONFIRMED]` | `!pepaths MixedLib.dll` vs `ComComponent.dll` | Identifies C++/CLI VTableFixups vs Pure Native | Incorrect classification of binary runtime nature. |
| **PETRACE-08** | `!pecfg` constructs basic blocks and branch edges | `[CONFIRMED]` | `!pecfg ComComponent!DllGetClassObject` | Displays basic blocks, branches, and call targets | Disassembly failure or incorrect branch targets. |
