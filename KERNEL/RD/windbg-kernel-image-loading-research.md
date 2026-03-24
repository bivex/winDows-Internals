# WinDbg Research: Early Kernel PE/Image Loading

## Scope

This document summarizes a live WinDbg MCP session exploring how Windows kernel images are loaded during early boot on the current target.

Target context observed during the session:

- Live remote kernel debugging session
- ARM64 kernel build `26100.1`
- WinDbg `10.0.29507.1001`
- Session stopped very early in boot at first, with `System Uptime: 0`

## Research Goal

The goal was to manually trace the kernel's image-loading path and confirm, with debugger evidence, how PE images are loaded in early boot rather than relying on static recollection.

## Initial Constraints

At the beginning of the session, the system was stopped so early that:

- `nt!PsLoadedModuleList` was still zeroed
- the normal loaded-module list was not yet useful for inspection

Because of that, the investigation started from symbol discovery and disassembly of kernel loader routines rather than from the module list.

## Loader-Related Symbols Identified

The following exported or internal routines were identified as relevant to system image loading:

- `nt!IopLoadDriverImage`
- `nt!MmLoadSystemImage`
- `nt!MmLoadSystemImageEx`
- `nt!MiCreateImageFileMap`
- `nt!MiReadImageHeaders`
- `nt!MiVerifyImageHeader`
- `nt!MiBuildImageControlArea`
- `nt!MiMapSystemImage`
- `nt!MiCreateImageOrDataSection`
- `nt!MiCreateSectionForDriver`
- `nt!PsSetLoadImageNotifyRoutine`

These symbols make the broad control flow clear:

1. I/O manager initiates driver loading.
2. Memory manager creates a section for the driver image.
3. Memory manager reads and validates PE headers.
4. The image is mapped into system space.
5. Debugger symbol notifications may occur around or after image presence in memory.

## PE Validation Path Confirmed

Disassembly of `nt!MiCreateImageFileMap` and `nt!MiVerifyImageHeader` showed the kernel performing the expected PE checks.

### Observed behaviors in `MiCreateImageFileMap`

- Calls `FsRtlGetFileSize`
- Prepares MDL/header state
- Uses `MiReadImageHeaders` if needed
- Checks DOS signature `MZ` (`0x5A4D`)
- Extracts `e_lfanew`
- Passes control to `MiVerifyImageHeader`
- Builds image control area with `MiBuildImageControlArea`

### Observed behaviors in `MiVerifyImageHeader`

- Verifies PE signature `PE\0\0` (`0x4550`)
- Verifies nonzero section count
- Validates optional header type
- On this target, the kernel image uses PE32+ optional header magic `0x20B`
- Copies important header fields into an internal image description structure
- Reads fields corresponding to image size, headers, entry point, subsystem and data directories

This confirmed that the memory manager is doing concrete PE-format validation in the kernel loader path, not just generic section creation.

## In-Memory Kernel Image Inspection

The in-memory `nt` image was inspected directly.

### Observed from `!dh nt -f`

- Image type: executable image
- Machine: ARM64
- Image base: `fffff8006bc00000`
- Size of image: `0x124A000`
- Entry point RVA: `0xA87BA0`
- Optional header magic: `0x20B` (PE32+)
- DLL characteristics included:
  - High entropy VA
  - Dynamic base
  - NX compatible
  - Guard

The loaded kernel image also showed the usual important PE directories, including:

- Export directory
- Import directory
- Exception directory
- Security directory
- Base relocation directory
- Debug directory
- Load configuration directory

## Earliest Image Notifications Observed

A breakpoint on `nt!DbgLoadImageSymbols` was used first to capture the earliest image announcements visible to the debugger.

The following early images were observed:

- `\SystemRoot\system32\hal.dll`
- `\SystemRoot\system32\kdcom.dll`
- `\SystemRoot\system32\symcryptk.dll`
- `\SystemRoot\System32\drivers\cng.sys`

### Notes

- `hal.dll` was observed at base `fffff8006d200000`
- `kdcom.dll` was observed at base `fffff800652d0000`
- `symcryptk.dll` was observed at base `fffff800652f0000`

These were debugger image-symbol notifications, which are useful for ordering early images but are not themselves the deepest part of the loader path.

## First Actual Loader Breakpoint Captured

After disabling the noisy `DbgLoadImageSymbols` breakpoint, the next stop occurred in the real loader path:

- Breakpoint hit: `nt!MmLoadSystemImageEx`
- Caller: `nt!IopLoadCrashdumpDriver`
- Boot phase context: `IoInitSystemPreDrivers`

The image being loaded at that moment was:

- `\SystemRoot\System32\Drivers\crashdmp.sys`

This is the first manually captured system-driver load in the actual memory-manager path during this session.

## Deeper Control Flow Captured for `crashdmp.sys`

Continuing from `MmLoadSystemImageEx` led into the lower image-creation path. The captured call stack showed:

1. `nt!IopLoadCrashdumpDriver`
2. `nt!MmLoadSystemImageEx`
3. `nt!MiObtainSectionForDriver`
4. `nt!MiCreateSectionForDriver`
5. `nt!MiCreateSystemSection`
6. `nt!MiCreateSection`
7. `nt!MiCreateImageOrDataSection`
8. `nt!MiCreateNewSection`
9. `nt!MiCreateImageFileMap`

This stack is the clearest direct evidence from the session for how a system driver PE image enters the kernel loader machinery.

## What `MmLoadSystemImageEx` Was Seen Doing

At the breakpoint in `nt!MmLoadSystemImageEx`, the routine was observed to:

- zero output pointers early
- validate incoming flags
- set up local structures
- call `nt!MiGenerateSystemImageNames`
- continue into memory-manager helper paths that obtain/create the driver section

This places `MmLoadSystemImageEx` as the policy/coordination layer above the deeper `Mi*` routines.

## What `MiCreateImageFileMap` Was Seen Doing

At the `nt!MiCreateImageFileMap` breakpoint for `crashdmp.sys`, the initial instructions clearly showed:

- file size acquisition through `FsRtlGetFileSize`
- local state and buffer setup
- progression toward header read/validation logic

Combined with the earlier disassembly, this confirms that `MiCreateImageFileMap` is where the kernel transitions from "driver section requested" into concrete PE image parsing and validation.

## Structural Observation: Loader Metadata

The structure `nt!_KLDR_DATA_TABLE_ENTRY` was inspected and is clearly central to loaded kernel image bookkeeping.

Relevant fields include:

- `DllBase`
- `EntryPoint`
- `SizeOfImage`
- `FullDllName`
- `BaseDllName`
- `Flags`
- `LoadCount`
- `SectionPointer`
- `LoadedImports`
- `TimeDateStamp`

This structure is consistent with the kernel maintaining loader metadata parallel to the PE image mapping itself.

## Key Findings

- Early in boot, `PsLoadedModuleList` may not yet be available, so loader research must start from code flow and direct breakpoints.
- Debugger image notifications appear very early and can reveal initial images such as `hal.dll`, `kdcom.dll`, and `symcryptk.dll`.
- The first manually captured real system-driver load in this session was `crashdmp.sys`.
- The concrete image-loading path for that driver ran through:
  - `MmLoadSystemImageEx`
  - `MiObtainSectionForDriver`
  - `MiCreateSectionForDriver`
  - `MiCreateImageOrDataSection`
  - `MiCreateImageFileMap`
- `MiCreateImageFileMap` and `MiVerifyImageHeader` perform explicit PE validation, including DOS and PE signature checks.
- The kernel image in memory is a normal PE32+ ARM64 executable with standard PE directories and modern mitigation-related characteristics.

## Practical Conclusion

For manual kernel-loader debugging in early boot, a useful strategy is:

1. Break on `nt!DbgLoadImageSymbols` to learn the earliest image order.
2. Then disable that breakpoint once it becomes too noisy.
3. Break on `nt!MmLoadSystemImageEx` and `nt!MiCreateImageFileMap` to catch the real loader path.
4. Step or trace further into `nt!MiReadImageHeaders` and `nt!MiVerifyImageHeader` for PE parsing details.

This approach worked well in the current session and produced direct evidence of both early image order and the underlying PE loader path.

## Suggested Follow-Up

The next useful investigation would be to single-step the `crashdmp.sys` load through:

- `nt!MiReadImageHeaders`
- `nt!MiVerifyImageHeader`
- `nt!MiBuildImageControlArea`
- `nt!MiMapSystemImage`

That would complete the picture from file-backed driver request to validated and mapped kernel image.
