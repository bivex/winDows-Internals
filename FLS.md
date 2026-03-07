## FLS investigation notes

Target for this pass:
- process: `Creative Cloud UI Helper.exe`
- reason: this process shows real live FLS usage, which is better than a minimal `conhost` session for studying FLS internals

## 1. Where FLS lives per-thread

Live `TEB` inspection showed:
- `TEB+0x17c8 = FlsData`

Observed examples:
- some threads had `FlsData != 0`
- some threads had `FlsData = 0`
- debugger attach thread also had `FlsData = 0`

Conclusion:
- FLS is thread-local
- FLS state is lazy / selective, not guaranteed to exist on every thread

## 2. `KERNELBASE!Fls*` is only a wrapper layer

Disassembly showed:
- `KERNELBASE!FlsAlloc` calls `RtlFlsAlloc`
- `KERNELBASE!FlsSetValue` calls `RtlFlsSetValue`

Conclusion:
- real FLS logic lives in `ntdll`, not in `KERNELBASE`

## 3. What `RtlFlsSetValue` really does

`ntdll!RtlFlsSetValue` showed:
- read `TEB.FlsData`
- if null, allocate `0x50` bytes on the process heap
- zero-initialize the new block
- write it back to `TEB+0x17c8`
- link the new thread FLS block into global `ntdll!RtlpFlsContext`
- then write the requested slot value

Conclusion:
- first real FLS use on a thread lazily creates the per-thread FLS block

## 4. What `RtlFlsGetValue` does

`ntdll!RtlFlsGetValue` showed:
- read `TEB.FlsData`
- compute the slot/chunk location
- return the stored pointer from the thread-local block

Conclusion:
- FLS lookup is a direct per-thread structure lookup off the TEB

## 5. Global FLS state: `ntdll!RtlpFlsContext`

Live dump showed:
- `ntdll!RtlpFlsContext` is the central global FLS context
- `RtlpFlsContext+0x58 = 0x14`

Conclusion:
- current high-water mark is `0x14`
- so the process has at least roughly 20 active/allocated FLS slots tracked in the global context

## 6. What `RtlpFlsAlloc` does

`ntdll!RtlpFlsAlloc` showed:
- acquire lock on `RtlpFlsContext`
- allocate a slot from the binary-array slot table
- store callback pointer for the slot
- update the global high-water mark

Conclusion:
- slot metadata and callbacks are globally managed in `RtlpFlsContext`

## 7. What `RtlpFlsDataCleanup` does

`ntdll!RtlpFlsDataCleanup` showed:
- iterate slots up to `RtlpFlsContext+0x58`
- for each non-null thread-local value, resolve the corresponding global callback entry
- call the callback
- zero the slot
- unlink the thread FLS block from the global list
- free chunk storage
- free the `FlsData` block itself

Conclusion:
- thread-exit FLS cleanup is real and callback-driven
- this is the concrete cleanup path used by the runtime/loader on thread shutdown

## 8. Live thread-local FLS blocks

Examples observed:
- thread 0: `FlsData = 0x4cfd40`
- thread 5: `FlsData = 0x543990`

Dumping these blocks showed:
- list/header-like pointers
- pointers to chunk storage used for slot values

Conclusion:
- the live memory layout matches the behavior seen in `RtlFlsSetValue`, `RtlFlsGetValue`, and `RtlpFlsDataCleanup`

## 9. Real callback users already visible

From the callback storage in `RtlpFlsContext`, observed callback targets included:
- `msvcrt!freefls`
- `ucrtbase!_vcrt_freefls`
- `COMCTL32!freefls`
- callbacks inside `chrome_elf`
- callbacks inside `libcef`
- callbacks inside `VulcanWrapper`
- callbacks inside `PSHook64`
- callbacks inside `VulcanMessage5`

Conclusion:
- FLS in this process is not theoretical
- multiple CRT/UI/browser-related modules are using real FLS cleanup callbacks

## 10. Current best summary

What is already proven:
- `TEB.FlsData` is the per-thread FLS root
- `RtlFlsSetValue` lazily creates it
- `RtlFlsGetValue` reads from it
- `RtlpFlsContext` holds the global slot/callback metadata
- `RtlpFlsDataCleanup` performs thread-exit callback cleanup and frees the per-thread FLS storage

## 11. Best next step

Next useful direction:
- build a slot-by-slot map of the FLS callback table
- identify which callback belongs to which slot and module
- then correlate that with per-thread live `FlsData` values

## 12. Callback slot map from `RtlpFlsContext`

The first callback chunk looked like a 16-entry block and the second chunk looked like a 5-entry extension block.

Public slot mapping observed so far:

- slot 0  -> no callback (`-1` sentinel)
- slot 1  -> `msvcrt!freefls`
- slot 2  -> `ucrtbase!_vcrt_freefls`
- slot 3  -> no callback (`-1` sentinel)
- slot 4  -> `ucrtbase!destroy_fls`
- slot 5  -> `Creative_Cloud_UI_Helper` (no symbols)
- slot 6  -> `Creative_Cloud_UI_Helper` (no symbols)
- slot 7  -> `COMCTL32!freefls`
- slot 8  -> `chrome_elf` callback
- slot 9  -> `chrome_elf` callback
- slot 10 -> `libcef` callback
- slot 11 -> `libcef` callback
- slot 12 -> `VulcanMessage5` callback
- slot 13 -> `VulcanMessage5` callback
- slot 14 -> `VulcanControl` callback
- slot 15 -> `VulcanControl` callback
- slot 16 -> `VulcanWrapper` callback
- slot 17 -> `VulcanWrapper` callback
- slot 18 -> `PSHook64` callback
- slot 19 -> `PSHook64` callback
- slot 20 -> `combase!FlsThreadCleanupCallback`

Resolved modules behind the callback pointers:
- `0x7ff8ffcb0fe0` -> `COMCTL32!freefls`
- `0x7ff8bcde1770`, `0x7ff8bcde5610` -> `chrome_elf`
- `0x7ff8b4feb840`, `0x7ff8b4ff7480` -> `libcef`
- `0x7ff8aa694020`, `0x7ff8aa6abc94` -> `VulcanMessage5`
- `0x7ff8aa4e223c`, `0x7ff8aa50082c` -> `VulcanControl`
- `0x7ff8aa7da290`, `0x7ff8aa7f0840` -> `VulcanWrapper`
- `0x7ff91aa871b8`, `0x7ff91aa89c0c` -> `PSHook64`
- `0x7ff92fc6a740` -> `combase!FlsThreadCleanupCallback`

This is already enough to conclude that the process has a mixed FLS ecology:
- CRT/runtime slots
- UI/common-controls slots
- Chromium/CEF-related slots
- Adobe/Vulcan slots
- COM slot(s)

## 13. Live per-thread slot population examples

Interpreting the thread-local FLS value chunks as:
- first qword = chunk header
- following qwords = public slot values

### Thread 0 (`FlsData = 0x4cfd40`)

Observed nonzero values included:
- slot 1  = `0x4a0860`
- slot 2  = `0x7ff92d860100`
- slot 4  = `0x4d5b70`
- slot 5  = `0x7ff67afcef40`
- slot 6  = `0x4d11e0`
- slot 7  = `0x22c0860`
- slot 8  = `0x7ff8bce4b9e0`
- slot 9  = `0x67dc0020c000`
- slot 10 = `0x7ff8b67d2a90`
- slot 11 = `0x5d640020c000`
- slot 12 = `0x7ff8aa708340`
- slot 13 = `0x4f0470`
- slot 14 = `0x7ff8aa5f3260`
- slot 15 = `0x519b10`
- slot 16 = `0x7ff8aa82ba40`
- slot 17 = `0x5228d0`
- slot 18 = `0x7ff91aa9ca60`
- slot 19 = `0x533b40`

### Thread 5 (`FlsData = 0x543990`)

Observed nonzero values included:
- slot 1  = `0x4aba50`
- slot 4  = `0x536370`
- slot 8  = `0x67dc002fc180`
- slot 9  = `0x67dc0020d000`
- slot 10 = `0x5d64002e0580`
- slot 11 = `0x5d640020d000`
- slot 12 = `0x506770`
- slot 13 = `0x5342e0`
- slot 14 = `0x506fe0`
- slot 15 = `0x533f10`
- slot 16 = `0x505f00`
- slot 17 = `0x534e50`
- slot 18 = `0x4f6400`
- slot 19 = `0x5333a0`

Conclusion:
- FLS slot usage differs materially between threads
- some slots are process-wide concepts with thread-local payloads
- some CRT/UI/Adobe-related slots are populated only on some threads

## 14. Semantics of the key cleanup callbacks

### Slot 1 -> `msvcrt!freefls`

Disassembly showed that this callback:
- checks the slot value for null
- frees many pointers inside the pointed structure at offsets like `+0x38`, `+0x40`, `+0x48`, `+0x50`, `+0x58`, `+0x68`, `+0x70`, `+0x78`, `+0x80`, `+0xA0`
- acquires CRT locks around some shared locale / multibyte cleanup
- decrements and conditionally frees `mbcinfo`
- calls `_removelocaleref` / `_freetlocinfo` for locale-like state
- finally frees the FLS record itself

Conclusion:
- slot 1 is not a tiny scalar
- it is a large legacy `msvcrt` per-thread state block that owns many nested heap allocations

### Slot 2 -> `ucrtbase!_vcrt_freefls`

Disassembly showed that this callback:
- returns immediately if the slot value is null
- compares the value against `ucrtbase!UnDecorator::m_recursionLevel+0x2c`
- frees the value only if it is not that special static object

Live correlation:
- on thread 0, slot 2 value was exactly `0x7ff92d860100`
- `ln 0x7ff92d860100` resolved to `ucrtbase!UnDecorator::m_recursionLevel+0x2c`

Conclusion:
- slot 2 can hold either:
  - a heap object to be freed on cleanup, or
  - a special static sentinel used by the UCRT undecorator recursion-level path
- therefore this slot is a small/special UCRT FLS slot, not a large generic PTD block

### Slot 4 -> `ucrtbase!destroy_fls`

Disassembly showed that this callback:
- checks the slot value for null
- calls `destroy_ptd` twice
- the second call is on `base + 0x3C8`
- then frees the whole block

Conclusion:
- slot 4 is a structured UCRT per-thread data block
- that block appears to contain two PTD-like subregions separated by `0x3C8`
- this is a much richer runtime state object than slot 2

### Slot 20 -> `combase!FlsThreadCleanupCallback`

Disassembly showed that this callback:
- checks `RtlDllShutdownInProgress`
- checks `IsThreadAFiber`
- skips cleanup if either condition is true
- otherwise calls `combase!DoThreadSpecificCleanup`

Conclusion:
- slot 20 is COM's per-thread cleanup hook
- COM intentionally avoids running normal thread-specific cleanup during DLL shutdown and on fibers

### Slot 7 -> `COMCTL32!freefls`

Disassembly showed that this callback is structurally very close to `msvcrt!freefls`:
- frees many nested pointers inside the FLS record
- uses internal locks around locale / multibyte-style shared state
- calls `_removelocaleref` / `_freetlocinfo`
- finally frees the FLS block itself

Conclusion:
- slot 7 is another large CRT-like per-thread state block
- in this process, COMCTL32 carries its own FLS cleanup logic rather than delegating to the already-loaded `msvcrt` callback

### Slots 5 and 6 -> `Creative_Cloud_UI_Helper` private callbacks

Even without private symbols, the two application-owned callbacks already separate into two classes.

Slot 5 callback (`0x7ff67af4b660`) looked like:
- null check
- compare against a module static sentinel at `Creative_Cloud_UI_Helper+0x12ef40`
- if not the sentinel, call one cleanup/free helper

Conclusion for slot 5:
- this is a small/special application-private FLS object with a sentinel-protected fast path

Slot 6 callback (`0x7ff67af6ee20`) looked like:
- null check
- call an internal cleanup helper
- walk many structure fields (`+0x48`, `+0x50`, `+0x58`, `+0x60`, `+0x68`, `+0x70`, `+0x78`, `+0x80`, `+0x3C0`)
- recursively destroy/free nested objects through a common helper

Conclusion for slot 6:
- this is a richer application-private per-thread aggregate state object
- it behaves much more like a destructor for a composite thread context than a simple scalar free

## 15. Correlating callback meaning with live slot values

Two strong live correlations are already proven:

- thread 0 slot 2 = `ucrtbase!UnDecorator::m_recursionLevel+0x2c`
  - this matches the special-case branch in `_vcrt_freefls`
- thread 0 and thread 5 slot 4 values are heap pointers
  - this matches `destroy_fls`, which expects a heap block and frees it after `destroy_ptd`

There is also a useful negative result:

- in the two sampled threads, slot 20 was not populated

Conclusion:
- a slot may be globally allocated and have a registered cleanup callback without being used on every thread
- this reinforces that FLS in the process is both lazy and thread-selective

## 16. Current best semantic summary

At this point the FLS map is no longer only structural.

What is now specifically understood:
- slot 1  = legacy `msvcrt` thread-state cleanup
- slot 2  = UCRT small/special object cleanup, including a static sentinel case
- slot 4  = UCRT PTD-block destruction
- slot 5  = application-private small/special object cleanup
- slot 6  = application-private composite thread-context cleanup
- slot 7  = COMCTL32 CRT-like thread-state cleanup
- slot 20 = COM thread-specific cleanup gate

This means the process uses FLS simultaneously for:
- CRT per-thread runtime state
- C++/UCRT helper state
- COM per-thread apartment / COM-local cleanup
- Adobe / CEF / UI-library private thread state in the remaining unresolved slots

## 17. Best next deep-dive

After proving a live nonzero COM slot, the best next reverse-engineering step is now one of these:

- resolve the exact meaning of one Adobe slot (`VulcanWrapper` / `VulcanControl`)
- resolve one Chromium slot (`chrome_elf` or `libcef`)

## 18. Live thread with nonzero slot 20

A full thread sweep found that, in the current snapshot, only one live thread had a nonzero COM FLS slot:

- thread index `2`
- OS thread id `0x48b0`
- `FlsData = 0x00518c10`
- second value chunk (`FlsData+0x18`) = `0x0050a960`
- slot 20 value (`chunk2+0x28`) = `0x0270f520`

Direct evidence chain:
- `~2e dq poi(@$teb+0x17c8) L4`
  - showed `FlsData+0x18 = 0x0050a960`
- `~2e dq poi(poi(@$teb+0x17c8)+0x18) L6`
  - showed the last qword in that 5-entry extension chunk was `0x0270f520`

Conclusion:
- slot 20 is not just globally registered in `RtlpFlsContext`
- it is live and populated on at least one real thread in this process

## 19. Correlating slot 20 with actual COM usage

Typing the slot-20 value through `combase` symbols gave:
- `dt combase!tagSOleTlsData 0x0270f520`

This resolved the pointed object as COM's per-thread `tagSOleTlsData` structure and exposed COM-specific fields such as:
- `dwApartmentID`
- `dwFlags`
- `cComInits`
- `pServerCall`
- `pObjServer`
- `pNativeApt`
- `hwndSTA`
- `pClipboardBroker`

The same thread also had:
- `TEB.ReservedForOle = 0x00561a30`

The current stack for that thread was not inside `combase`; it was blocked in a long-lived `VulcanMessage5` wait path.
That is still consistent with COM usage:
- COM per-thread state persists in TLS/FLS even when the thread is currently executing application code or waiting
- therefore COM activity does not need to be visible on the instantaneous stack to be proven

Conclusion:
- slot 20 is a real live pointer to COM per-thread TLS/FLS state
- in this process, COM is active on at least one `VulcanMessage5`-owned worker thread
- this confirms that `combase!FlsThreadCleanupCallback` is not dormant metadata; it has live thread-local state to clean up

Note:
- the public `dt` output for `tagSOleTlsData` is somewhat noisy in later fields, so the safest claim is the type identity and the presence of clearly COM-specific members, not every individual field value

