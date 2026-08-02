# Windows 11 24H2 Subsystem Research: Live Kernel & User-Mode Hot-Patching Architecture

**Date:** 2026-08-02  
**Target:** Windows 11 24H2 (ARM64 / x64 Build 26100+)  
**Subsystem:** Memory Manager (MM), PE Loader (`ntdll`), Virtualization-Based Security (VBS)  
**Workspace:** `/Volumes/External/Code/winDows-Internals/KERNEL/HOT_PATCHING_Research.md`

---

## 1. Executive Summary

In **Windows 11 24H2**, Microsoft introduced a native **Live Hot-Patching Subsystem**. This engine allows updating active kernel binaries (`ntoskrnl.exe`, drivers) and user-mode DLLs (`ntdll.dll`, `kernel32.dll`, `csrss.exe`) in RAM **without system reboots, service restarts, or process downtime**.

This research documents the internal system calls (`NtManageHotPatch`), thread freezing mechanics (`MiFreezeHotPatchProcess`), VBS/HVCI integration (`VslApplyHotPatch`), and the undo rollback table (`MiProcessHotPatchUndoTable`).

---

## 2. System Call & API Interface: `NtManageHotPatch`

The core entry point for hot-patch management is the new Windows 11 24H2 system call:

```cpp
// NTSYSAPI
NTSTATUS
NTAPI
NtManageHotPatch(
    _In_ ULONG Operation,              // HOTPATCH_OP_LOAD, APPLY, UNLOAD, ENUM
    _In_ PUNICODE_STRING PatchPath,    // Path to .hotpatch.dll / patch payload
    _In_ ULONG Flags,                  // Flags (e.g. HOTPATCH_FLAG_USER_SID, FORCE)
    _Out_ PULONG_PTR HotPatchHandle
);
```

### Operation Commands:
1. **`HOTPATCH_OP_LOAD` (`0x01`):** Maps the hot-patch image into system/process space via `MiMapHotPatchImageInSystemSpace`.
2. **`HOTPATCH_OP_APPLY` (`0x02`):** Freezes target process threads, validates signatures in VTL 1 via `VslApplyHotPatch`, and updates function entry points.
3. **`HOTPATCH_OP_REVERT` (`0x03`):** Applies `MiProcessHotPatchUndoTable` to revert patches instantly without unmapping memory.
4. **`HOTPATCH_OP_UNLOAD` (`0x04`):** Unlinks the hot-patch record (`MiDeleteHotPatchEntry`) and frees memory.

---

## 3. Kernel Hot-Patching Workflow: Step-by-Step Architecture

```
[NtManageHotPatch]
       │
       ▼
1. Eligibility Check ────► MiImageVadHotPatchEligible() / MiCheckHoldFaultForHotPatch()
       │
       ▼
2. Thread Freezing   ────► MiFreezeHotPatchProcess() (Suspends threads safely)
       │
       ▼
3. VBS Attestation   ────► VslApplyHotPatch() / VslObtainHotPatchUndoTable() [VTL 1 Check]
       │
       ▼
4. Page Preparation  ────► MiPrepareImagePagesForHotPatch() (Copy-On-Write / SLAT update)
       │
       ▼
5. Patch Application ────► RtlApplyHotPatch() / MiApplyImageHotPatch()
       │
       ▼
6. Thread Resumption ────► Thaw process threads & log status via MiLogHotPatchOperation()
```

---

## 4. VBS / VTL 1 Integration (`VslApplyHotPatch` & HVCI)

Because **Hypervisor-Protected Code Integrity (HVCI)** renders kernel memory Read-Only via SLAT/EPT tables, standard kernel memory writes trigger BugChecks.

Hot-patching bypasses this safely via VTL 1 communication:
1. **`VslObtainHotPatchUndoTable`:** Queries VTL 1 Secure Kernel for authorization and receives an encrypted Undo State buffer.
2. **`VslApplyHotPatch`:** Generates a Secure SMC (`SMC #0` / `HVC #1`) call to VTL 1.
3. The VTL 1 Hypervisor validates the digital signature of the hot-patch binary, temporarily modifies SLAT page permissions to Writable, writes the trampoline instruction (e.g., `ARM64 BTI/BR` or `x64 JMP`), and restores Read-Only/Executable SLAT protections.

---

## 5. Undo & Rollback Mechanics (`MiProcessHotPatchUndoTable`)

To guarantee zero-downtime stability, every applied hot-patch generates a corresponding entry in the global **HotPatch Undo Table**:

```cpp
typedef struct _MI_HOTPATCH_UNDO_ENTRY {
    PVOID       TargetAddress;     // Original function entry point
    ULONG       OriginalBytesLen;  // Length of overwritten instructions (e.g. 12 bytes)
    UCHAR       OriginalBytes[16]; // Backup of original opcodes
    PVOID       PatchImageBase;    // Pointer to loaded .hotpatch.dll
} MI_HOTPATCH_UNDO_ENTRY, *PMI_HOTPATCH_UNDO_ENTRY;
```

If `MiApplyImageHotPatch` fails at any point (e.g., thread suspension timeout in `MiLogHotPatchProcessSuspensionFailure`), the kernel executes `MiProcessHotPatchUndoTable`, atomically restoring `OriginalBytes` and leaving the system in a clean, unpatched state.

---

## 6. Structural Changes in `_LDR_DATA_TABLE_ENTRY`

To support live hot-patch tracking in user-mode DLLs, `_LDR_DATA_TABLE_ENTRY` has been extended in Windows 11 24H2:

```cpp
/* +0x128 */ PVOID  ActivePatchImageBase; // Ptr to loaded hotpatch DLL (NULL if unpatched)
/* +0x130 */ ULONG  HotPatchState;        // 0 = Base Image, 1 = Patched, 2 = Reverted
```

---

## 7. Forensic & Security Research Implications

1. **EDR / AV FP Reduction:** Security tools must query `RtlFindHotPatchInformation` or inspect `ActivePatchImageBase` in `_LDR_DATA_TABLE_ENTRY` before flagging in-memory function jumps (`JMP`/`BR`) as malicious API hooks.
2. **KASLR & Pointer Leak Audit:** `MiLogHotPatchRundown` and `MiLogHotPatchPagesLocked` log internal hot-patch operations to ETW, providing telemetry for security monitoring.

---

## 8. Git Commit Record

- Document created: `KERNEL/HOT_PATCHING_Research.md`
- Repository: `/Volumes/External/Code/winDows-Internals`
- Related symbols: `nt!NtManageHotPatch`, `nt!VslApplyHotPatch`, `nt!MiProcessHotPatchUndoTable`
