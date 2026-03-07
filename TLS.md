# TLS / loader / CRT guarded-static investigation notes

## Scope

Target: live `conhost.exe` process in WinDbg.

Goal: understand how TLS relates to loader startup, `main`, and real runtime initialization.

## 1. Initial debugger state

- Initial stack was a debugger break-in thread, not the app's crash/work stack:
  - `ntdll!DbgBreakPoint`
  - `ntdll!DbgUiRemoteBreakin`
  - `KERNEL32!BaseThreadInitThunk`
  - `ntdll!RtlUserThreadStart`
- Process identified as `conhost.exe`.

## 2. Loader structure walk

- Inspected `TEB`, `PEB`, `PEB->Ldr`, loader lists, and `_LDR_DATA_TABLE_ENTRY` records.
- Loader state looked sane and internally consistent.
- Main image resolved from loader state as `conhost.exe`.

## 3. Loader -> entrypoint -> app startup

- `ntdll!LdrpImageEntry` pointed to the main image loader entry.
- Main image `EntryPoint` resolved to `conhost!wWinMainCRTStartup`.
- Startup path reconstructed as:
  1. `ntdll!LdrInitializeThunk`
  2. `ntdll!LdrpInitializeProcess`
  3. `conhost!wWinMainCRTStartup`
  4. `conhost!__scrt_common_main_seh`
  5. `conhost!wWinMain`
- `wWinMain` then hands off real work to `conhost!ConsoleIoThread` via `ConsoleCreateIoThreadLegacy`.

## 4. TLS directory and pre-main TLS setup

- `conhost.exe` has a real PE TLS directory.
- Important TLS symbols found:
  - `conhost!tls_used`
  - `conhost!tls_start`
  - `conhost!tls_end`
  - `conhost!tls_index`
- TLS template size is 8 bytes:
  - `+0x0 = 0x00000000`
  - `+0x4 = conhost!Init_thread_epoch`
- `tls_index` was assigned slot `0` in this run.

## 5. What TLS is *not* doing here

- No meaningful PE TLS callback execution was found.
- No active CRT dynamic TLS init callback was found.
- Therefore TLS here is not mainly a "run code before main" mechanism.

## 6. What TLS *is* doing here

- TLS is used by MSVC CRT thread-safe local static initialization.
- Key CRT helpers found:
  - `conhost!Init_thread_header`
  - `conhost!Init_thread_footer`
  - `conhost!Init_thread_abort`
  - `conhost!Init_thread_wait`
  - `conhost!Init_global_epoch`
  - `conhost!Init_thread_epoch`

## 7. Concrete guarded-static call sites found

### `Telemetry::Instance`
- Reads TLS via `gs:[58h]`
- Reads current thread epoch from TLS slot block `+4`
- Compares against `Telemetry::Instance::$TSS0`
- Calls `Init_thread_header`
- Constructs singleton
- Registers `atexit`
- Calls `Init_thread_footer`

### `CommandLine::Instance`
- Same guarded-static pattern
- Guard stored at `__PchSym_+0x4`
- Has failure path through `Init_thread_abort`

### `Microsoft::Console::VirtualTerminal::TermTelemetry::Instance`
- Same guarded-static pattern
- Guard stored in `$TSS0`

### `NtPrivApi::_Instance`
- Same guarded-static pattern
- Lazy loads DLL with `LoadLibraryExW`
- Registers `atexit`

### `NtPrivApi::s_NtClose / s_NtOpenProcess / s_NtQueryInformationProcess`
- Same TLS/guard logic around lazy `GetProcAddress`
- Caches function pointers after guarded initialization

## 8. Distinction from global constructors

- Separate `dynamic initializer for ...` functions were found.
- Those are normal CRT global/static constructors run through startup init tables.
- They are distinct from the TLS-backed guarded local-static mechanism above.

## 9. Live runtime proof from real threads

### Global epoch
- `conhost!Init_global_epoch = 0x80000007`

### Thread 0 (`ConsoleIoThread`)
- `TEB.ThreadLocalStoragePointer = 0x44c6e0`
- TLS slot 0 -> `0x4366a0`
- Slot 0 block:
  - `+0x0 = 0x00000000`
  - `+0x4 = 0x80000007`

### Thread 1 (render thread)
- `TEB.ThreadLocalStoragePointer = 0x44c0a0`
- TLS slot 0 -> `0x02c3d720`
- Slot 0 block:
  - `+0x0 = 0x00000000`
  - `+0x4 = 0x80000000`

This proves different threads can carry different cached TLS epochs.

## 10. Live guard values

- `Telemetry::Instance::$TSS0 = 0x80000001`
- `CommandLine::Instance guard = 0x80000004`
- `TermTelemetry::Instance::$TSS0 = 0x80000007`
- `NtPrivApi::_Instance guard = 0x00000000`

Interpretation:

- `0` = never initialized
- `0xFFFFFFFF` = initialization in progress
- `epoch value` = initialized when global epoch had that value

## 11. What the CRT helpers do

### `Init_thread_header`
- Enters a CRT critical section.
- If guard == `0`, sets guard to `0xFFFFFFFF` (initialization in progress).
- If guard == `0xFFFFFFFF`, waits.
- Otherwise updates the current thread TLS epoch from `Init_global_epoch`.

### `Init_thread_footer`
- Enters critical section.
- Increments `Init_global_epoch`.
- Writes the new epoch to:
  - the guard variable
  - current thread TLS slot block `+4`
- Leaves critical section and notifies waiters.

### `Init_thread_abort`
- Resets guard back to `0`.
- Notifies waiters.

## 12. Final conclusion

For this `conhost.exe` session:

- the loader prepares TLS before `main`
- the TLS block contains per-thread CRT epoch state
- the main consumer of that TLS is MSVC guarded local-static initialization
- TLS is therefore acting as a runtime synchronization/cache mechanism, not mainly as a TLS-callback pre-main execution path

## 13. Next live target

- `NtPrivApi::_Instance` guard is still `0`
- Best next live step: catch the first transition of that guard in real time:
  - `0 -> 0xFFFFFFFF -> epoch`

## 14. Follow-up on `NtPrivApi`

- The lazy-loaded module string for `NtPrivApi::_Instance` is `"ntdll.dll"`.
- The lazily resolved export names are:
  - `NtOpenProcess`
  - `NtQueryInformationProcess`
  - `NtClose`
- Current cached state is still completely empty:
  - cached module handle = `0`
  - cached `NtOpenProcess` pointer = `0`
  - cached `NtQueryInformationProcess` pointer = `0`
  - cached `NtClose` pointer = `0`
- Therefore this whole lazy-init path has not executed yet in the observed session.

### What would trigger it

- `NtPrivApi::s_GetProcessParentId` is the concrete consumer.
- That helper performs:
  1. `NtOpenProcess`
  2. `NtQueryInformationProcess`
  3. `NtClose`
- So the first real use of parent-process querying logic should trigger the guarded-static transition in `NtPrivApi`.

## 15. Follow-up on telemetry / connection flow

- `ConsoleProcessHandle::ConsoleProcessHandle` does **not** use `NtPrivApi`.
- It calls imported Win32 `OpenProcess` directly:
  - desired access = `0x02000000`
  - inherit handle = `FALSE`
  - process id = constructor input
- If that succeeds, it stores the process handle, creates `ConsoleProcessPolicy`, then calls:
  - `Telemetry::Instance`
  - `Telemetry::LogProcessConnected(handle)`

### What `Telemetry::LogProcessConnected` actually does

- On the active telemetry-enabled path it:
  1. calls `Telemetry::TotalCodesForPreviousProcess`
  2. calls `QueryFullProcessImageNameW` on the process handle
  3. strips the basename with `PathFindFileNameW`
  4. calls `Telemetry::FindProcessName`
  5. updates telemetry tables / counters for the process image name
- `Telemetry::TotalCodesForPreviousProcess` itself only drains counters from `TermTelemetry::Instance` and clears them.

### Why this matters

- The main process-connection telemetry path is real and active.
- But it still does **not** explain the cold `NtPrivApi` singleton.
- So the first caller of `NtPrivApi::s_GetProcessParentId` is likely in a different feature path than ordinary console process connection telemetry.

## 16. Direct caller of `NtPrivApi::s_GetProcessParentId`

- Additional eliminations from nearby symbol tracing:
  - `Tracing::s_TraceConsoleAttachDetach` uses `Telemetry::Instance`, calls `ConsoleProcessHandle::GetProcessCreationTime`, then emits TraceLogging.
  - `ConsoleProcessHandle::GetProcessCreationTime` only lazy-caches `GetProcessTimes` on the stored process handle.
  - `capture_previous_context` is only unwind/context logic (`RtlCaptureContext`, `RtlLookupFunctionEntry`, `RtlVirtualUnwind`).
  - `ConsoleArguments::_GetClientCommandline` only rebuilds / escapes a client command line string and does not use `NtPrivApi`.
  - `attemptHandoff` is a handoff/delegation path (`CoInitializeEx`, `CoCreateInstance`, `CreatePipe`, `DuplicateHandle`, host signal thread startup) and showed no direct parent-process query.

### Static xref result

- A direct static scan of on-disk `C:\Windows\System32\conhost.exe` for `call rel32` sites targeting `NtPrivApi::s_GetProcessParentId` found exactly **one** hit.
- The only direct call site resolves to:
  - `ApiDispatchers::ServerGenerateConsoleCtrlEvent+0x73`

### What that function does with the parent id

- `ApiDispatchers::ServerGenerateConsoleCtrlEvent`:
  1. enters the console-process-list critical section
  2. reads a process id from the request message (`[rdi+8Ch]`)
  3. searches the console process list for that id
  4. if not found, calls `NtPrivApi::s_GetProcessParentId(&pid)`
  5. searches the console process list again using the returned parent id
  6. if found, allocates process data for the original pid
  7. stores the original pid into global state and calls `HandleCtrlEvent`

### Updated conclusion

- The cold `NtPrivApi` singleton is not first exercised by ordinary process connection telemetry.
- It is exercised by the **console control-event generation path**, specifically when `ServerGenerateConsoleCtrlEvent` must translate a request pid to its parent pid to find the owning console process entry.
