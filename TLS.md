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

## 17. Loader-side TLS lifecycle for `conhost`

This fills in the main TLS-specific gap left by the earlier notes: **how the loader actually creates and maintains the per-thread TLS state that `conhost` later reads through `gs:[58h]` and `tls_index`.**

### Process startup: `ntdll!LdrpInitializeTls`

- `LdrpInitializeTls` walks the loader module list from `PebLdr`.
- For each loaded image, it checks PE directory entry **9** (the TLS directory) via `RtlpImageDirectoryEntryToDataEx`.
- If an image has static TLS, it calls `LdrpAllocateTlsEntry` and links that image into the loader's TLS bookkeeping.
- After scanning all images, it initializes the TLS bitmap / bitmap vector and then calls `LdrpAllocateTls`.

So the loader-side meaning is:

1. discover which loaded images have static TLS
2. assign/track TLS slots for them
3. allocate the initial per-thread TLS vector for the current thread

### Per-thread allocation: `ntdll!LdrpAllocateTls`

- `LdrpAllocateTls` acquires `LdrpTlsLock` shared and reads `LdrpTlsBitmap`.
- It allocates a TLS vector with `LdrpGetNewTlsVector`.
- It then walks `LdrpTlsList`.
- For each TLS entry, it allocates heap memory for that thread's TLS block, copies the image TLS template with `memcpy`, and stores the resulting block pointer into the vector slot for that image.
- Finally it writes the TLS vector pointer into the TEB at `TEB+0x58` and increments `LdrpActiveThreadCount`.

This is the missing concrete explanation for the live `conhost` observations:

- `conhost!tls_index = 0`
- each thread has a TLS vector at `gs:[58h]`
- slot 0 points at that thread's private copy of the `conhost` TLS template

### Thread start: `ntdll!LdrpInitializeThread`

- On thread startup, `LdrpInitializeThread` calls `LdrpAllocateTls` early.
- It then acquires the loader lock and iterates loaded modules.
- For eligible modules, it calls `LdrpCallTlsInitializers(..., reason = 2)` and also runs general init routines.

So a newly created `conhost` thread gets its static TLS storage **before** normal module thread-attach/init work continues.

### Thread exit: `ntdll!LdrShutdownThread`

- On thread shutdown, `LdrShutdownThread` acquires the loader lock and walks loaded modules.
- It calls TLS initializers with thread-detach reason **3**.
- After that, it calls `LdrpFreeTls`.
- The same path also performs FLS cleanup and activation-context-stack cleanup.

So the loader does not just create static TLS blocks; it also tears them down on thread exit.

### Dynamic TLS updates: `ntdll!LdrpHandleTlsData`

- `LdrpHandleTlsData` handles the harder case where TLS state must be propagated while threads already exist.
- It uses `LdrpTlsLock`, sizes work from `LdrpActiveThreadCount`, allocates/copies TLS template blocks for active threads, expands TLS vectors with `LdrpGetNewTlsVector` when needed, and updates process information with `NtSetInformationProcess`.
- It also queues deferred TLS cleanup or frees old TLS blocks when entries are removed.

This matters because the loader's TLS story is not only a one-time process-start event; it also has machinery for keeping TLS-consistent state across already-active threads.

### Why some `conhost` threads still show `Init_thread_epoch = 0x80000000`

- The `conhost` TLS template is only 8 bytes.
- Earlier live inspection showed it begins as:
  - `+0x0 = 0x00000000`
  - `+0x4 = 0x80000000` (`Init_thread_epoch`)
- `LdrpAllocateTls` copies that template into each thread's private TLS block.

So a new thread starts with the **initial** thread epoch value. It only "catches up" later if that thread executes code that participates in the CRT guarded-static machinery.

That explains the earlier live split:

- one `conhost` thread had `Init_thread_epoch = 0x80000007`
- another still had `Init_thread_epoch = 0x80000000`

The second thread was not evidence of missing TLS. It was evidence that the loader had copied the initial TLS template correctly, but that thread had not yet advanced through the guarded-static epoch path.

### Final TLS-specific conclusion

- The loader creates the TLS slot and per-thread block.
- The per-thread block starts as a copy of the image TLS template.
- The CRT later consumes that block for thread-safe local static initialization.

So the complete `conhost` picture is:

1. `ntdll` discovers `conhost` static TLS
2. `ntdll` allocates a per-thread slot/vector entry and copies the 8-byte template
3. `conhost`/CRT later read that block through `gs:[58h]` and `tls_index`
4. `Init_thread_epoch` stays at `0x80000000` until that thread actually participates in guarded-static initialization

## 18. Attempted live thread-birth trace

After documenting the loader-side TLS lifecycle, the next planned proof was to catch a **newly created `conhost` thread** immediately after loader TLS allocation and inspect its fresh TLS state live.

### Intended live proof

- Break in `ntdll!LdrpInitializeThread` just after `LdrpAllocateTls` returns.
- Inspect:
  - `@$teb`
  - `poi(@$teb+58)` = thread TLS vector
  - `poi(poi(@$teb+58))` = TLS slot 0 block for `conhost`
  - first two dwords of that block
- Expected result for a new thread:
  - `+0x0 = 0`
  - `+0x4 = 0x80000000`

That would have been the direct live confirmation that the loader copied the initial 8-byte `conhost` TLS template into a just-created thread before guarded-static code advanced the per-thread epoch.

### What was armed in WinDbg

- Thread-create event break:
  - `sxe ct`
- One-shot breakpoint after `LdrpAllocateTls` inside `ntdll!LdrpInitializeThread`:
  - `bp /1 ntdll!LdrpInitializeThread+0x6f ...`
- The breakpoint command was set up to dump:
  - current thread
  - `@$teb`
  - `poi(@$teb+58)`
  - the TLS vector contents
  - TLS slot 0 block contents
  - stack backtrace

### Baseline state observed before resume

- Existing one-shot breakpoints from the earlier `NtPrivApi` work were still present but disabled.
- On the currently selected thread, live inspection still showed a valid TLS vector:
  - `$teb = 0x00000000002eb000`
  - `poi(@$teb+58) = 0x000000000044c0a0`
- The current stack was still consistent with an already-running `conhost` worker thread, not a fresh thread-start stop.

### Actual blocker

- The session still could not resume execution.
- Both of the usual resume commands failed:
  - `g`
  - `gh`
- Both returned the same failure:
  - `hr = -2147220987`
- `.lastevent` still reported a first-chance break instruction exception.

So the failure was **not** in the TLS plan or the breakpoint placement. The failure was that this debugger session remained non-resumable for execution control.

### What this means

- The loader-side TLS explanation is already strong and consistent with the earlier static and live inspections.
- The missing piece is only the final live thread-birth capture.
- No contradictory TLS evidence appeared during this attempt.
- The session limitation prevented catching a newly created thread after `LdrpAllocateTls`.

### Current final status

- Documented and supported:
  - static TLS discovery in `LdrpInitializeTls`
  - per-thread TLS block copy in `LdrpAllocateTls`
  - thread attach path in `LdrpInitializeThread`
  - thread detach cleanup in `LdrShutdownThread`
  - explanation for threads still showing `Init_thread_epoch = 0x80000000`
- Not yet captured live:
  - a brand-new `conhost` thread stopped immediately after TLS allocation with slot 0 still at the initial template value

So from this point onward, the remaining TLS gap is no longer reverse-engineering; it is only the lack of a continue-capable debugger session needed for the final live proof.

## 19. Reattached-session live TLS check

After reattaching to `conhost` in a fresh session, live thread inspection produced an important new distinction: **ordinary `conhost` threads still showed normal static TLS state, but the debugger-created attach thread did not.**

### Live thread set in the reattached session

- The process had three threads:
  - `conhost!ConsoleIoThread`
  - `conhost!Microsoft::Console::Render::RenderThread::_ThreadProc`
  - `ntdll!DbgUiRemoteBreakin`
- The `DbgUiRemoteBreakin` thread was the debugger-created attach thread, not an ordinary application worker thread.

### TLS state of the ordinary `conhost` threads

- Thread 0 (`ConsoleIoThread`):
  - `TEB = 0x22b000`
  - `ThreadLocalStoragePointer = 0x5f23d0`
  - TLS slot 0 block = `0x5e6880`
  - first 8 bytes = `00000000 80000003`
- Thread 1 (render thread):
  - `TEB = 0x231000`
  - `ThreadLocalStoragePointer = 0x5f1fb0`
  - TLS slot 0 block = `0x2819300`
  - first 8 bytes = `00000000 80000000`

So the earlier live TLS result still held in the reattached session:

- one ordinary thread had caught up to the current epoch
- another ordinary thread still had the initial per-thread epoch value

### Global state still matched the thread-local state

- `conhost!Init_global_epoch = 0x80000003`
- `conhost!tls_index = 0`
- `conhost!tls_start` still held the 8-byte template:
  - `00 00 00 00 00 00 00 80`

This remained consistent with the observed thread state:

- caught-up thread: `Init_thread_epoch = 0x80000003`
- not-yet-caught-up thread: `Init_thread_epoch = 0x80000000`

### New special-case finding: the debugger attach thread had no TLS vector

- For the `ntdll!DbgUiRemoteBreakin` thread:
  - `TEB = 0x239000`
  - `ThreadLocalStoragePointer = 0`
- So this brand-new attach-created thread did **not** have a normal TLS vector installed at the observed stop.

This is important because it means the debugger-created thread cannot be treated as an ordinary example of loader-created application thread TLS state.

### Why this attach thread differs

- Comparing `TEB+0x17EE` (`SameTebFlags`) across threads showed:
  - ordinary threads: `SameTebFlags = 0`
  - `DbgUiRemoteBreakin` thread: `SameTebFlags = 8`
- Re-checking `ntdll!LdrpInitializeThread` showed that it tests bit `0x08` in `SameTebFlags` before the normal `LdrpAllocateTls` call.
- If that bit is set, control goes to a special path.
- That special path tests bit `0x20`; if `0x20` is not set, it exits before the normal TLS-allocation path.

That exactly matched the attach thread:

- `SameTebFlags = 8`
- no observed `0x20` bit
- `ThreadLocalStoragePointer = 0`

### Meaning of this result

- Ordinary `conhost` threads still behave exactly as the earlier TLS model predicts.
- The debugger-created attach thread is a **special loader/debugger case** that bypasses the normal `LdrpAllocateTls` path.
- Therefore it cannot serve as the missing live proof of a normal new `conhost` worker thread receiving the initial copied TLS template.

### Updated practical conclusion

- The earlier loader analysis remains supported.
- The new reattached-session result does not weaken that TLS model.
- It adds a new caveat: **a debugger attach thread is not a valid stand-in for an ordinary thread-start TLS event in `conhost`.**
