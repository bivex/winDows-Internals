# Findings: `LdrpInvertedFunctionTable` (agent-verified only)

## Scope

This revision keeps only statements that were verified in the current live WinDbg session through the agent.
Anything that was broader theory, public API background, or undocumented-but-unverified interpretation was removed.

Target observed by the agent:

- Windows 10 19045 x64
- `ntdll` symbols loaded

## Symbols confirmed by the agent

Command used:

`x ntdll!*InvertedFunctionTable*`

Observed symbols:

- `ntdll!LdrpInvertedFunctionTable`
- `ntdll!LdrpInvertedFunctionTableSRWLock`
- `ntdll!RtlInsertInvertedFunctionTable`
- `ntdll!RtlRemoveInvertedFunctionTable`
- `ntdll!RtlpInsertInvertedFunctionTableEntry`

## Structure layouts confirmed by the agent

Command used:

`dt ntdll!_INVERTED_FUNCTION_TABLE`

Observed layout:

- `CurrentSize : Uint4B`
- `MaximumSize : Uint4B`
- `Epoch : Uint4B`
- `Overflow : UChar`
- `TableEntry : [512] _INVERTED_FUNCTION_TABLE_ENTRY`

Command used:

`dt ntdll!_INVERTED_FUNCTION_TABLE_ENTRY`

Observed layout:

- `FunctionTable : Ptr64 _IMAGE_RUNTIME_FUNCTION_ENTRY`
- `DynamicTable : Ptr64 _DYNAMIC_FUNCTION_TABLE` at the same offset as `FunctionTable`
- `ImageBase : Ptr64 Void`
- `SizeOfImage : Uint4B`
- `SizeOfTable : Uint4B`

## Live contents of `LdrpInvertedFunctionTable`

Command used:

`dt ntdll!_INVERTED_FUNCTION_TABLE 0x00007ffd18d31500`

Observed values:

- `CurrentSize = 0x96`
- `MaximumSize = 0x200`
- `Epoch = 0x30a`
- `Overflow = 0`

Commands used:

- `dx ((ntdll!_INVERTED_FUNCTION_TABLE*)0x00007ffd18d31500)->TableEntry[0]`
- `dx ((ntdll!_INVERTED_FUNCTION_TABLE*)0x00007ffd18d31500)->TableEntry[1]`
- `dx ((ntdll!_INVERTED_FUNCTION_TABLE*)0x00007ffd18d31500)->TableEntry[2]`
- `lm m ntdll`
- `lm m Creative_Cloud`
- `lm m libcef`

Observed entries and matching module ranges:

1. `TableEntry[0]`
   - `ImageBase = 0x7ffd18bb0000`
   - `FunctionTable = 0x7ffd18d22000`
   - `SizeOfImage = 0x1f8000`
   - `SizeOfTable = 0xe4d8`
   - `lm m ntdll` shows `ntdll` at `00007ffd\`18bb0000 - 00007ffd\`18da8000`
2. `TableEntry[1]`
   - `ImageBase = 0x7ff7c03b0000`
   - `FunctionTable = 0x7ff7c05d1000`
   - `SizeOfImage = 0x270000`
   - `SizeOfTable = 0x12090`
   - `lm m Creative_Cloud` shows `Creative_Cloud` at `00007ff7\`c03b0000 - 00007ff7\`c0620000`
3. `TableEntry[2]`
   - `ImageBase = 0x7ffc93600000`
   - `FunctionTable = 0x7ffc9e87b000`
   - `SizeOfImage = 0x0b9e7000`
   - `SizeOfTable = 0x005cde64`
   - `lm m libcef` shows `libcef` at `00007ffc\`93600000 - 00007ffc\`9efe7000`

## Insert path confirmed by disassembly

Command used:

`u ntdll!RtlInsertInvertedFunctionTable L30`

Observed call sequence in order:

1. call `ntdll!RtlCaptureImageExceptionValues`
2. load `ntdll!LdrpInvertedFunctionTableSRWLock`
3. call `ntdll!RtlAcquireSRWLockExclusive`
4. call `ntdll!LdrProtectMrdata` with `ecx = 0`
5. call `ntdll!RtlpInsertInvertedFunctionTableEntry`
6. call `ntdll!LdrProtectMrdata` with `ecx = 1`
7. jump to `ntdll!RtlReleaseSRWLockExclusive`

This is directly visible in the disassembly; no extra interpretation is needed.

## Lookup path confirmed by disassembly

### `RtlLookupFunctionEntry`

Command used:

`u ntdll!RtlLookupFunctionEntry L80`

Observed facts:

- the function reads `qword ptr [ntdll!LdrpInvertedFunctionTable+0x18]`
- it later reads `dword ptr [ntdll!LdrpInvertedFunctionTable+0x20]`
- if the fast range check fails, it calls `ntdll!RtlpxLookupFunctionTable`
- if no result is found, execution branches to a call to `ntdll!RtlpLookupDynamicFunctionEntry`

Also visible in this disassembly:

- after a table is selected, the code subtracts image base from `ControlPc`
- the code then performs a binary-search-style walk over table entries using comparisons against begin/end fields in the selected function table

### `RtlpxLookupFunctionTable`

Command used:

`u ntdll!RtlpxLookupFunctionTable L80`

Observed facts:

- the function acquires `ntdll!LdrpInvertedFunctionTableSRWLock`
- it reads `dword ptr [ntdll!LdrpInvertedFunctionTable]` as the current count
- search starts with `r9d = 1`
- the search base is `ntdll!LdrpInvertedFunctionTable+0x10`
- comparisons are performed against `ImageBase` and `ImageBase + SizeOfImage`
- on match, the selected entry is copied out to the caller buffer
- the lock is then released

On this build, the code therefore treats `TableEntry[0]` specially in `RtlLookupFunctionEntry`, while `RtlpxLookupFunctionTable` searches starting from index `1`.

## Current unwind example confirmed by the agent

Commands used:

- `r rip`
- `.fnent @rip`
- `ln @rip`

Observed output:

- `rip = 00007ffd18c51020`
- nearest symbol: `ntdll!DbgBreakPoint`
- `.fnent @rip` reports:
  - `BeginAddress = 00000000\`000a1020`
  - `EndAddress = 00000000\`000a1022`
  - `UnwindInfoAddress = 00000000\`0014e0b8`
  - unwind info: `version 1, flags 0, prolog 0, codes 0`

This confirms that, in the current session, WinDbg can resolve the current instruction pointer to a function entry and unwind metadata.

## What this file does not claim anymore

This file intentionally does **not** claim, unless separately proven in a later session:

- the full architectural purpose of the inverted table beyond what the disassembly directly shows
- any guarantee that the same layout or fast-path exists on other Windows builds
- any detailed statement about VEH/SEH policy
- any debugger-behavior claim under corruption scenarios
- any statement that depends only on Microsoft documentation rather than the live agent output

## Minimal conclusion supported by the agent

From the current live session, the agent verified that:

- `ntdll` contains a concrete `LdrpInvertedFunctionTable` object and related helper routines
- the object stores entries containing `ImageBase`, `SizeOfImage`, `FunctionTable`/`DynamicTable`, and `SizeOfTable`
- the current process has at least 150 populated entries
- the first observed entries correspond to `ntdll`, `Creative_Cloud`, and `libcef`
- `RtlInsertInvertedFunctionTable` updates this state while using `LdrpInvertedFunctionTableSRWLock` and `LdrProtectMrdata`
- `RtlLookupFunctionEntry` reads from this table, uses a special case for entry data located at the start of the object, and falls back to `RtlpxLookupFunctionTable`
- `RtlpxLookupFunctionTable` performs a range-based search over entries starting at index `1`
- if that path does not produce a result, `RtlLookupFunctionEntry` can branch to `RtlpLookupDynamicFunctionEntry`

## Dynamic function table symbols confirmed by the agent

Command used:

`x ntdll!*DynamicFunction*`

Observed symbols:

- `ntdll!RtlpDynamicFunctionTable`
- `ntdll!RtlpDynamicFunctionTableTreeMin`
- `ntdll!RtlpDynamicFunctionTableTreeMax`
- `ntdll!RtlpDynamicFunctionTableLock`
- `ntdll!RtlpLookupDynamicFunctionEntry`

Command used:

`x ntdll!*DynamicCallback*`

Observed symbols:

- `ntdll!RtlpDynamicCallbackTableTreeMin`
- `ntdll!RtlpDynamicCallbackTableTreeMax`

## Dynamic function table type enum confirmed by the agent

Command used:

`dt ntdll!_FUNCTION_TABLE_TYPE`

Observed values:

- `RF_SORTED = 0`
- `RF_UNSORTED = 1`
- `RF_CALLBACK = 2`
- `RF_KERNEL_DYNAMIC = 3`

## Current global state for dynamic tables on this target

Commands used:

- `dq ntdll!RtlpDynamicCallbackTableTreeMin L1`
- `dq ntdll!RtlpDynamicCallbackTableTreeMax L1`
- `dq ntdll!RtlpDynamicFunctionTableTreeMin L1`
- `dq ntdll!RtlpDynamicFunctionTableTreeMax L1`
- `dq ntdll!RtlpDynamicFunctionTable L2`
- `dt ntdll!_LIST_ENTRY 0x00007ffd18d312c0`

Observed values:

- `RtlpDynamicCallbackTableTreeMin = 0`
- `RtlpDynamicCallbackTableTreeMax = 0`
- `RtlpDynamicFunctionTableTreeMin = 0`
- `RtlpDynamicFunctionTableTreeMax = 0`
- `RtlpDynamicFunctionTable` is a self-linked list head:
  - `Flink = 0x00007ffd18d312c0`
  - `Blink = 0x00007ffd18d312c0`

On this target, the dynamic function table list appears empty at the moment these commands were executed.

## `RtlpLookupDynamicFunctionEntry` path confirmed by disassembly

Command used:

`u ntdll!RtlpLookupDynamicFunctionEntry L120`

Observed high-level flow:

1. acquires `RtlpDynamicFunctionTableLock` in shared mode
2. searches `RtlpDynamicCallbackTableTreeMin`
3. if not found, searches `RtlpDynamicCallbackTableTreeMax`
4. if still not found, searches `RtlpDynamicFunctionTableTreeMin`
5. if still not found, searches `RtlpDynamicFunctionTableTreeMax`
6. if no candidate is found, releases the shared lock and returns `NULL`

Observed candidate fields read from the selected `_DYNAMIC_FUNCTION_TABLE`:

- `+0x10` -> `FunctionTable`
- `+0x30` -> `BaseAddress`
- `+0x50` -> `Type`
- `+0x54` -> `EntryCount`

### Type-dependent behavior observed in `RtlpLookupDynamicFunctionEntry`

From the `_FUNCTION_TABLE_TYPE` enum and the direct comparisons in disassembly:

- if `Type == 0` (`RF_SORTED`), the function takes a binary-search-style path over the function table
- if `Type == 3` (`RF_KERNEL_DYNAMIC`), it takes the same binary-search-style path
- if `Type == 1` (`RF_UNSORTED`), it walks the function table linearly in `0x0c`-byte steps
- otherwise, it takes the callback path, which matches `Type == 2` (`RF_CALLBACK`)

### Callback path observed in `RtlpLookupDynamicFunctionEntry`

For the callback case, the disassembly shows:

- load callback from `+0x38`
- load context from `+0x40`
- release the shared lock before invoking the callback
- store `BaseAddress` through the output pointer
- invoke the callback through `ntdll!_guard_dispatch_icall_fptr`

## Registration paths confirmed by disassembly

### `RtlInstallFunctionTableCallback`

Command used:

`u ntdll!RtlInstallFunctionTableCallback L100`

Observed facts:

- it validates that the low two bits of `TableIdentifier` are `3`
- it allocates a `_DYNAMIC_FUNCTION_TABLE`-sized object plus optional string storage
- it stores:
  - `BaseAddress` into `+0x20` and `+0x30`
  - `BaseAddress + Length` into `+0x28`
  - callback pointer into `+0x38`
  - context into `+0x40`
- it stores `Type = 2` at `+0x50`
- it inserts the record into:
  - `RtlpDynamicCallbackTableTreeMin`
  - `RtlpDynamicCallbackTableTreeMax`
  - `RtlpDynamicFunctionTable` list

### `RtlAddGrowableFunctionTable`

Command used:

`u ntdll!RtlAddGrowableFunctionTable L100`

Observed facts:

- it allocates a `_DYNAMIC_FUNCTION_TABLE`-sized object
- it stores `Type = 3` at `+0x50`
- it inserts the record into:
  - `RtlpDynamicFunctionTableTreeMin`
  - `RtlpDynamicFunctionTableTreeMax`
  - `RtlpDynamicFunctionTable` list

### `RtlAddFunctionTable`

Command used:

`u ntdll!RtlAddFunctionTable L100`

Observed facts:

- it allocates a `_DYNAMIC_FUNCTION_TABLE`-sized object
- it initializes `Type` at `+0x50` to `0`
- while scanning the supplied `RUNTIME_FUNCTION` array, it can set `Type` to `1`
- the scan also computes min and max address bounds, which are later adjusted by `BaseAddress`
- it inserts the record into:
  - `RtlpDynamicFunctionTableTreeMin`
  - `RtlpDynamicFunctionTableTreeMax`
  - `RtlpDynamicFunctionTable` list

## Cleanup paths confirmed by disassembly

### `RtlDeleteFunctionTable`

Commands used:

- `u ntdll!RtlDeleteFunctionTable L140`
- `x ntdll!*Delete*FunctionTable*`

Observed facts:

- it calls `LdrProtectMrdata(0)` before touching the dynamic-table state
- it acquires `RtlpDynamicFunctionTableLock` exclusively
- it walks the `RtlpDynamicFunctionTable` list and compares the caller-supplied pointer against the field at `+0x10` in each record
- if no matching record is found, it releases `RtlpDynamicFunctionTableLock`, restores `LdrProtectMrdata(1)`, and returns failure
- if a matching record is found, it reads `Type` from `+0x50`
- if `Type == 2`, it removes the record from:
  - `RtlpDynamicCallbackTableTreeMin`
  - `RtlpDynamicCallbackTableTreeMax`
- otherwise, it removes the record from:
  - `RtlpDynamicFunctionTableTreeMin`
  - `RtlpDynamicFunctionTableTreeMax`
- it unlinks the record from the common `RtlpDynamicFunctionTable` doubly linked list
- it releases `RtlpDynamicFunctionTableLock`
- it restores `LdrProtectMrdata(1)` after the unlink/removal work
- if the removed record has `Type == 3`, it calls `RtlDeleteGrowableFunctionTable`
- otherwise, it frees the record with `RtlFreeHeap`

The disassembly also shows CFG/MRDATA-heap accounting around `LdrpMrdataLock`, `LdrpMrdataHeap`, and `LdrpMrdataHeapUnprotected` before and after the free path.

### `RtlDeleteGrowableFunctionTable`

Command used:

`u ntdll!RtlDeleteGrowableFunctionTable L140`

Observed facts:

- it first checks that `Type == 3` at `+0x50`
- if the type check fails, it raises status `0xC000000D` via `RtlRaiseStatus`
- before unlinking the record, it calls `NtSetInformationProcess`
- if that call fails, it raises the returned status via `RtlRaiseStatus`
- it calls `LdrProtectMrdata(0)` before editing dynamic-table state
- it contains the same CFG/MRDATA-heap accounting pattern around `LdrpMrdataLock`, `LdrpMrdataHeap`, and `LdrpMrdataHeapUnprotected`
- it acquires `RtlpDynamicFunctionTableLock` exclusively
- it removes the record from:
  - `RtlpDynamicFunctionTableTreeMin`
  - `RtlpDynamicFunctionTableTreeMax`
- it unlinks the record from the common `RtlpDynamicFunctionTable` list
- it releases `RtlpDynamicFunctionTableLock`
- it frees the record with `RtlFreeHeap`
- it restores `LdrProtectMrdata(1)` before returning

### `RtlGetFunctionTableListHead`

Command used:

`u ntdll!RtlGetFunctionTableListHead L20`

Observed fact:

- the function simply returns the address of `RtlpDynamicFunctionTable`

### `RtlGrowFunctionTable`

Commands used:

- `x ntdll!*Grow*FunctionTable*`
- `u ntdll!RtlGrowFunctionTable L140`
- `dt ntdll!_DYNAMIC_FUNCTION_TABLE`

Observed facts:

- the symbol `ntdll!RtlGrowFunctionTable` is present on this target
- `_DYNAMIC_FUNCTION_TABLE.Type` is at `+0x50`
- `_DYNAMIC_FUNCTION_TABLE.EntryCount` is at `+0x54`
- the function first checks that `Type == 3`
- if that type check fails, it raises status `0xC000000D` via `RtlRaiseStatus`
- it compares the caller-supplied `edx` value against the current `EntryCount`
- if the caller-supplied value is smaller than the current `EntryCount`, it raises status `0xC000000D` via `RtlRaiseStatus`
- on the accepted path, it updates `_DYNAMIC_FUNCTION_TABLE.EntryCount`
- the disassembly shows the same CFG/MRDATA-heap accounting pattern around `LdrpMrdataLock`, `LdrpMrdataHeap`, and `LdrpMrdataHeapUnprotected`
- in the observed disassembly, no call to `LdrProtectMrdata` was seen
- in the observed disassembly, no use of `RtlpDynamicFunctionTableLock` was seen
- in the observed disassembly, no AVL insert/remove calls were seen
- in the observed disassembly, no references to `RtlpDynamicFunctionTableTreeMin`, `RtlpDynamicFunctionTableTreeMax`, or the common `RtlpDynamicFunctionTable` list were seen

## Growable-function-table lifecycle supported by combined disassembly

Commands used:

- `u ntdll!RtlAddGrowableFunctionTable L120`
- `u ntdll!RtlGrowFunctionTable L120`
- `u ntdll!RtlDeleteGrowableFunctionTable L120`
- `dt ntdll!_DYNAMIC_FUNCTION_TABLE`

Observed facts:

- `RtlAddGrowableFunctionTable` allocates a `_DYNAMIC_FUNCTION_TABLE`-sized record (`0x88` bytes in the observed disassembly)
- `RtlAddGrowableFunctionTable` writes:
  - `FunctionTable` at `+0x10`
  - `EntryCount` at `+0x54`
  - `MinimumAddress` at `+0x20`
  - `MaximumAddress` at `+0x28`
  - `BaseAddress` at `+0x30`
  - `Type = 3` at `+0x50`
- `RtlAddGrowableFunctionTable` calls `NtSetInformationProcess` before inserting the record into global dynamic-table state
- `RtlAddGrowableFunctionTable` calls `RtlpProtectInvertedFunctionTable(0)`, acquires `RtlpDynamicFunctionTableLock`, inserts into:
  - `RtlpDynamicFunctionTableTreeMin`
  - `RtlpDynamicFunctionTableTreeMax`
  - `RtlpDynamicFunctionTable`
- `RtlAddGrowableFunctionTable` then calls `RtlpProtectInvertedFunctionTable(1)` and writes the resulting record pointer through its first argument (`mov qword ptr [r12], rdi`)
- `RtlGrowFunctionTable` accepts only `Type == 3` records, rejects decreasing `EntryCount`, and on the accepted path updates only `EntryCount`
- `RtlDeleteGrowableFunctionTable` accepts only `Type == 3` records, calls `NtSetInformationProcess`, removes the record from both dynamic AVL trees, unlinks it from the common list, and frees it

Minimal lifecycle conclusion for this step:

- the combined disassembly supports a three-step lifecycle for `Type == 3` dynamic records:
  1. create/register via `RtlAddGrowableFunctionTable`
  2. grow via `RtlGrowFunctionTable`
  3. unregister/free via `RtlDeleteGrowableFunctionTable`
- the same allocated `_DYNAMIC_FUNCTION_TABLE` record is the object created by `RtlAddGrowableFunctionTable`, later mutated by `RtlGrowFunctionTable`, and finally removed/freed by `RtlDeleteGrowableFunctionTable`

## What this session did and did not confirm about callers of `RtlGrowFunctionTable`

Commands used:

- `lm m Creative_Cloud`
- `lm m libcef`
- `!dh 00007ff7c03b0000 -i`
- `!dh 00007ffc93600000 -i`
- `u ntdll!RtlAddGrowableFunctionTable L120`
- `u ntdll!RtlGrowFunctionTable L120`
- `u ntdll!RtlDeleteGrowableFunctionTable L120`

Observed facts:

- `Creative_Cloud` and `libcef` were inspected as loaded modules during this session
- in the visible import output inspected for `Creative_Cloud`, no direct `RtlGrowFunctionTable` import line was shown
- in the visible import output inspected for `libcef`, no direct `RtlGrowFunctionTable` import line was shown
- in the visible import output inspected for `libcef`, direct imports for other runtime-function APIs were shown, including:
  - `RtlAddFunctionTable`
  - `RtlDeleteFunctionTable`
  - `RtlLookupFunctionEntry`
- the disassembly shows that `RtlAddGrowableFunctionTable` returns the allocated record pointer through its first argument, while both `RtlGrowFunctionTable` and `RtlDeleteGrowableFunctionTable` operate on a record whose `Type` is checked at `+0x50`
- in a later controlled repro, a live stack at the `RtlGrowFunctionTable` breakpoint showed:
  - `ntdll!RtlGrowFunctionTable`
  - `growable_ft_repro_x64+0x132c`
  - `growable_ft_repro_x64+0x1644`
  - `KERNEL32!BaseThreadInitThunk+0x14`
  - `ntdll!RtlUserThreadStart+0x21`

Minimal conclusion for the caller question:

- on the original `Creative_Cloud` / `libcef` target, I did not capture a live caller of `RtlGrowFunctionTable`
- in the visible import output inspected in this session, I also did not confirm a direct `RtlGrowFunctionTable` import from `Creative_Cloud` or `libcef`
- the strongest disassembly-supported statement is that a caller would need the growable `_DYNAMIC_FUNCTION_TABLE` record pointer produced by `RtlAddGrowableFunctionTable`, and that pointer is the object later consumed by `RtlGrowFunctionTable`
- in the later controlled repro, I did capture a live caller chain leading into `RtlGrowFunctionTable`

## Attempt to capture a live hit on `RtlGrowFunctionTable`

Commands used:

- `bp ntdll!RtlGrowFunctionTable`
- `bl`
- `g`
- `.lastevent`
- `r rip`
- `kb`
- `~`
- `~0s`
- `kb`

Observed facts:

- a breakpoint on `ntdll!RtlGrowFunctionTable` was present in the session
- attempting `g` did not produce a captured hit on `RtlGrowFunctionTable`
- after that attempt, the observed last event remained a first-chance break instruction exception
- `rip` was observed at `ntdll!DbgBreakPoint`
- on the current thread at that stop, the observed stack was:
  - `ntdll!DbgBreakPoint`
  - `ntdll!DbgUiRemoteBreakin+0x4e`
  - `KERNEL32!BaseThreadInitThunk+0x14`
  - `ntdll!RtlUserThreadStart+0x21`
- thread enumeration (`~`) showed many process threads; the current thread was the remote-break thread (`. 40`)
- after switching to `~0`, the observed stack was in the application/UI wait path through `win32u`, `USER32`, `libcef`, and `Creative_Cloud`

Minimal conclusion for this step:

- in this specific session, I did not capture a live hit on `RtlGrowFunctionTable`
- the session evidence is consistent with the debugger remaining in a break-state / remote-break context rather than producing a useful grow-function breakpoint hit

## Live hit on `RtlGrowFunctionTable` in a controlled repro

Commands used:

- `sxd ibp`
- `sxd iml`
- `.logopen /t C:\augment\windbg-agent\build-x64\Release\grow_hit.log`
- `bp ntdll!RtlGrowFunctionTable ".echo RTLGROW_HIT;.echo --REGS--;r rcx;r rdx;.echo --TABLE--;dt ntdll!_DYNAMIC_FUNCTION_TABLE @rcx;.echo --STACK--;kb;gc"`
- `bl`
- `g`

Observed facts:

- a live breakpoint hit on `ntdll!RtlGrowFunctionTable` was captured in the controlled process `growable_ft_repro_x64.exe`
- at the captured entry to `RtlGrowFunctionTable`:
  - `rcx = 0x00000000001a08c0`
  - `rdx = 2`
- `dt ntdll!_DYNAMIC_FUNCTION_TABLE @rcx` at that hit showed:
  - `Type = 3 (RF_KERNEL_DYNAMIC)`
  - `EntryCount = 1`
  - `FunctionTable = 0x00000000005691c0`
  - `MinimumAddress = 0x190000`
  - `MaximumAddress = 0x190300`
  - `BaseAddress = 0x190000`
- the live stack at the hit was:
  - `ntdll!RtlGrowFunctionTable`
  - `growable_ft_repro_x64+0x132c`
  - `growable_ft_repro_x64+0x1644`
  - `KERNEL32!BaseThreadInitThunk+0x14`
  - `ntdll!RtlUserThreadStart+0x21`
- the debugger log containing this hit was written to `grow_hit_0fa0_2026-03-08_03-47-13-903.log`

Minimal conclusion for this step:

- a real live hit on `RtlGrowFunctionTable` was reproduced and captured by the agent in a controlled process
- at function entry, the observed record was a `Type == 3` `_DYNAMIC_FUNCTION_TABLE` with `EntryCount == 1`, and the caller requested growth to `2`

## Post-grow state and lookup after grow in a controlled repro

Commands used:

- `|1s`
- `~0s`
- `kb`
- `u growable_ft_repro_x64+0x12b0 L100`
- `.frame 2`
- `dq 00000000\`0014fe70 L24`
- `dps 00000000\`0014fe70 L24`
- `dq ntdll!RtlpDynamicFunctionTable L2`
- `dt ntdll!_DYNAMIC_FUNCTION_TABLE poi(ntdll!RtlpDynamicFunctionTable)`
- `dd 00000000\`00533ed0 L6`
- `dt ntdll!_IMAGE_RUNTIME_FUNCTION_ENTRY 00000000\`00533ed0`
- `dt ntdll!_IMAGE_RUNTIME_FUNCTION_ENTRY 00000000\`00533edc`
- `.fnent 00000000\`001d0120`

Observed facts:

- in the controlled repro, thread `~0` was observed stopped in `SleepEx` with return address `growable_ft_repro_x64+0x1376`
- the disassembly of `growable_ft_repro_x64` shows:
  - a call to `RtlGrowFunctionTable` at `growable_ft_repro_x64+0x1328`
  - a later call to `RtlLookupFunctionEntry` at `growable_ft_repro_x64+0x1346`
  - the sleep-after-grow call at `growable_ft_repro_x64+0x1370`
- the common dynamic-table list head `ntdll!RtlpDynamicFunctionTable` pointed to `0x00000000001e0880`
- `dt ntdll!_DYNAMIC_FUNCTION_TABLE 0x00000000001e0880` showed:
  - `FunctionTable = 0x0000000000533ed0`
  - `MinimumAddress = 0x1d0000`
  - `MaximumAddress = 0x1d0300`
  - `BaseAddress = 0x1d0000`
  - `Type = 3 (RF_KERNEL_DYNAMIC)`
  - `EntryCount = 2`
- `dd 0x0000000000533ed0 L6` and `dt ntdll!_IMAGE_RUNTIME_FUNCTION_ENTRY` showed two entries in that runtime-function table:
  - entry 0: `BeginAddress = 0x100`, `EndAddress = 0x106`, `UnwindData = 0x200`
  - entry 1: `BeginAddress = 0x120`, `EndAddress = 0x126`, `UnwindData = 0x200`
- `.fnent 0x00000000001d0120` resolved the second entry and reported:
  - `BeginAddress = 0x120`
  - `EndAddress = 0x126`
  - `UnwindInfoAddress = 0x200`
  - unwind info at `0x1d0200`

Minimal conclusion for this step:

- in the controlled repro, the post-grow dynamic-table state had `EntryCount == 2`
- the second `RUNTIME_FUNCTION` entry (`0x120..0x126`) was present in the table after grow
- the debugger was able to resolve `ControlPc = 0x1d0120` via `.fnent`, which is consistent with the post-grow `RtlLookupFunctionEntry` path observed in the repro disassembly

## Unwind of the second entry (`0x1d0120`) in the controlled repro

Commands used:

- `|1s`
- `~0s`
- `u 0x00000000001d0120 L8`
- `db 0x00000000001d0200 L8`
- `.fnent 0x00000000001d0120`
- `dt ntdll!_UNWIND_INFO 0x00000000001d0200`
- `!unwind 0x00000000001d0120`

Observed facts:

- the bytes at `0x1d0120` were:
  - `55` (`push rbp`)
  - `48 8b ec` (`mov rbp, rsp`)
  - `5d` (`pop rbp`)
  - `c3` (`ret`)
- the raw unwind-info bytes at `0x1d0200` were:
  - `01 04 02 05 04 03 01 50`
- `.fnent 0x1d0120` resolved the second runtime-function entry and decoded the unwind info as:
  - `BeginAddress = 0x120`
  - `EndAddress = 0x126`
  - `UnwindInfoAddress = 0x200`
  - `version 1`
  - `prolog 4`
  - `codes 2`
  - `frame reg 5 (rbp)`
  - unwind code 0: `UWOP_SET_FPREG` at offset `4`
  - unwind code 1: `UWOP_PUSH_NONVOL reg: rbp` at offset `1`
- `dt ntdll!_UNWIND_INFO 0x1d0200` did not work in this session because the symbol `ntdll!_UNWIND_INFO` was not found
- `!unwind 0x1d0120` was not available in this session (`No export unwind found`)

Minimal conclusion for this step:

- for the second entry at `0x1d0120`, the agent verified both the function bytes and the decoded unwind metadata
- the decoded unwind operations match the observed function prolog/epilog for `push rbp; mov rbp, rsp; ...; pop rbp; ret`
- in this session, `.fnent` was the working debugger path for inspecting unwind on the second entry

## Live `RtlVirtualUnwind` on a synthetic context for the second entry

Commands used:

- `dt _CONTEXT`
- `.logopen /t C:\augment\windbg-agent\build-x64\Release\virtual_unwind_hit.log`
- `bp /1 ntdll!RtlVirtualUnwind ...`
- `g`
- `|`
- `.lastevent`
- `~0s`
- `kb`
- `x growable_ft_virtual_unwind_probe_x64!g_vu*`
- `dq growable_ft_virtual_unwind_probe_x64!g_vu_stage L12`
- `? poi(growable_ft_virtual_unwind_probe_x64!g_vu_image_base)`
- `? poi(growable_ft_virtual_unwind_probe_x64!g_vu_control_pc)`
- `? poi(growable_ft_virtual_unwind_probe_x64!g_vu_function_entry)`
- `? poi(growable_ft_virtual_unwind_probe_x64!g_vu_pre_rip)`
- `? poi(growable_ft_virtual_unwind_probe_x64!g_vu_pre_rsp)`
- `? poi(growable_ft_virtual_unwind_probe_x64!g_vu_pre_rbp)`
- `? poi(growable_ft_virtual_unwind_probe_x64!g_vu_post_rip)`
- `? poi(growable_ft_virtual_unwind_probe_x64!g_vu_post_rsp)`
- `? poi(growable_ft_virtual_unwind_probe_x64!g_vu_post_rbp)`
- `? poi(growable_ft_virtual_unwind_probe_x64!g_vu_establisher)`
- `? poi(growable_ft_virtual_unwind_probe_x64!g_vu_handler)`
- `dt ntdll!_IMAGE_RUNTIME_FUNCTION_ENTRY poi(growable_ft_virtual_unwind_probe_x64!g_vu_function_entry)`
- `u growable_ft_virtual_unwind_probe_x64+0x14e0 L32`

Observed facts:

- the debugger captured a live breakpoint hit on `ntdll!RtlVirtualUnwind` in the controlled process `growable_ft_virtual_unwind_probe_x64.exe`
- at the entry breakpoint, the observed register arguments were:
  - `rcx = 0` (`HandlerType`)
  - `rdx = 0x190000` (`ImageBase`)
  - `r8 = 0x190124` (`ControlPc`)
  - `r9 = 0x52904c` (`FunctionEntry`)
- `dt ntdll!_IMAGE_RUNTIME_FUNCTION_ENTRY 0x52904c` showed:
  - `BeginAddress = 0x120`
  - `EndAddress = 0x126`
  - `UnwindInfoAddress = 0x200`
- the breakpoint handler successfully dumped the input `CONTEXT` before unwind:
  - `Rip = 0x190124`
  - `Rsp = 0x14fe90`
  - `Rbp = 0x14fe90`
- after the controlled process continued, exported globals in `growable_ft_virtual_unwind_probe_x64.exe` showed:
  - `g_vu_stage = 2`
  - `g_vu_image_base = 0x190000`
  - `g_vu_control_pc = 0x190124`
  - `g_vu_function_entry = 0x52904c`
  - `g_vu_pre_rip = 0x190124`
  - `g_vu_pre_rsp = 0x14fe90`
  - `g_vu_pre_rbp = 0x14fe90`
  - `g_vu_post_rip = 0x1902f0`
  - `g_vu_post_rsp = 0x14fea0`
  - `g_vu_post_rbp = 0x1902e0`
  - `g_vu_establisher = 0x14fe90`
  - `g_vu_handler = 0`
- at the time of post-unwind inspection, thread `~0` was stopped in `NtDelayExecution` / `SleepEx`
- the disassembly around `growable_ft_virtual_unwind_probe_x64+0x14e0` showed the post-unwind path leading to the later sleep call

Minimal conclusion for this step:

- the agent verified a live call to `RtlVirtualUnwind` for the second runtime-function entry (`0x120..0x126`)
- for the synthetic context with `Rip = 0x190124`, `Rsp = 0x14fe90`, and `Rbp = 0x14fe90`, the observed post-unwind state was:
  - `Rip = 0x1902f0`
  - `Rsp = 0x14fea0`
  - `Rbp = 0x1902e0`
- `EstablisherFrame` remained `0x14fe90`, and the returned exception handler pointer was `0`
- this is consistent with unwinding the verified second-entry prolog/epilog and restoring a saved frame pointer plus synthetic return address from the synthetic stack

## Attempt to capture a live registration event in this session

Commands used:

- `bp ntdll!RtlAddFunctionTable`
- `bp ntdll!RtlInstallFunctionTableCallback`
- `bp ntdll!RtlAddGrowableFunctionTable`
- `bl`
- `g`
- `.lastevent`
- `r rip`
- `kb`

Observed facts:

- breakpoints were set on:
  - `ntdll!RtlAddFunctionTable`
  - `ntdll!RtlInstallFunctionTableCallback`
  - `ntdll!RtlAddGrowableFunctionTable`
- no breakpoint hit on those three registration APIs was captured in this session
- the observed last event remained a first-chance break instruction exception
- `rip` was observed at `ntdll!DbgBreakPoint`
- the observed stack was:
  - `ntdll!DbgBreakPoint`
  - `ntdll!DbgUiRemoteBreakin+0x4e`
  - `KERNEL32!BaseThreadInitThunk+0x14`
  - `ntdll!RtlUserThreadStart+0x21`

Minimal conclusion for this step:

- in this specific agent session, I did not reproduce a live dynamic function table registration event
- therefore this document still does not contain an agent-captured non-empty runtime example of `_DYNAMIC_FUNCTION_TABLE`

## Minimal dynamic-path conclusion supported by the agent

From the current live session, the agent verified that:

- dynamic function tables are tracked separately from `LdrpInvertedFunctionTable`
- `ntdll` maintains a shared lock, a common list head, and four AVL roots for dynamic-table lookup
- callback-based registrations and non-callback registrations use different AVL roots
- `RtlpLookupDynamicFunctionEntry` first searches callback trees, then non-callback trees
- lookup behavior depends on `_FUNCTION_TABLE_TYPE`
- callback-type entries are invoked only after releasing the shared lock
- deletion walks the common dynamic-table list, removes matching nodes from the appropriate AVL roots, and unlinks them from the list
- growable-table cleanup is handled by the dedicated `RtlDeleteGrowableFunctionTable` path
- `RtlGetFunctionTableListHead` exposes the common list head by returning `RtlpDynamicFunctionTable`
- `RtlGrowFunctionTable` was observed updating `EntryCount` for `Type == 3` records and rejecting non-growing / wrong-type inputs with `0xC000000D`
- combined disassembly supports a `RtlAddGrowableFunctionTable -> RtlGrowFunctionTable -> RtlDeleteGrowableFunctionTable` lifecycle for `Type == 3` records
- on the original `Creative_Cloud` / `libcef` target, no live caller of `RtlGrowFunctionTable` was captured, and no direct `RtlGrowFunctionTable` import was confirmed in the visible import output that was inspected
- in a later controlled repro, a live caller chain into `RtlGrowFunctionTable` was captured
- in that controlled repro, post-grow inspection showed `EntryCount == 2`, two `RUNTIME_FUNCTION` entries, and successful resolution of `ControlPc = 0x1d0120` through `.fnent`
- for the second entry `0x1d0120`, the agent also matched the decoded unwind metadata to the actual stub bytes and prolog/epilog
- in a later controlled probe, the agent also verified a live `RtlVirtualUnwind` call for the second entry and observed the post-unwind `CONTEXT` state
- a live registration event was attempted with breakpoints on the three registration APIs, but no hit was captured in this session
- a dedicated live-hit attempt on `RtlGrowFunctionTable` also did not capture a hit in this session
- a later controlled repro did capture a real live hit on `RtlGrowFunctionTable`
- on this target, no dynamic function table entries were present at the time of inspection
