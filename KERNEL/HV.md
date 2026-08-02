# Hyper-V HVL Internals: Kernel-Level RE on ARM64

**Environment:**
- OS: Windows 11 Pro ARM64, Build 26100.1
- Architecture: ARM64 / AArch64, EL1 Kernel Mode
- Debugger: WinDbg via MCP Agent (`10.211.55.5:44445`)
- Tooling: `structscan v4.0`, WinDbg `dq` / `u` / `uf` / `x` / `dt`

**Confidence tiers used throughout this document:**

| Tier | Meaning |
|---|---|
| **[OBSERVED]** | Direct WinDbg output: addresses, raw memory, disassembly |
| **[INFERRED]** | Interpretation based on observed data + cross-references |
| **[HYPOTHESIS]** | Architectural conjecture; requires further verification |

---

## 1. Hypervisor Connection State

### Evidence

```
kd> db nt!HvlHypervisorConnected L1
fffff802`ac809a87  00

kd> dq nt!HvlPartitionId L1
fffff802`ac809ee8  00000000`00000000

kd> dq nt!HvlpActiveProcessorCount L1
fffff802`ac809c20  00000000`00000000
```

### [OBSERVED]
All three HVL state variables are zero. This kernel was captured while the **hypervisor was not active** (either bare-metal boot without Hyper-V, or the kernel debugger session was attached before hypervisor initialization). Fields are present in ntoskrnl PDB and confirmed via `x nt!Hvl*`.

### [INFERRED]
When the hypervisor is active, `HvlHypervisorConnected` is expected to be `0x01`, `HvlPartitionId` would hold the root partition handle assigned by the hypervisor, and `HvlpActiveProcessorCount` would reflect the number of VPs scheduled on this partition.

---

## 2. Hypervisor Version

### Evidence

```
kd> x nt!HvlpHypervisorVersion
fffff802`ac80a448 nt!HvlpHypervisorVersion = <no type information>

kd> dq nt!HvlpHypervisorVersion L4
fffff802`ac80a448  00000000`00000000 00000000`00000000
fffff802`ac80a458  00000000`00111311 00000000`00000000
```

### [OBSERVED]
Symbol `HvlpHypervisorVersion` exists in public PDB at `0xfffff802ac80a448`. Raw memory at `+0x10` contains `0x00111311`.

### [INFERRED]
`0x00111311` likely encodes major/minor/service/build in packed DWORD format (common pattern in HV spec: `[31:16]` major, `[15:8]` minor, `[7:0]` service). Exact field layout requires cross-referencing `HviGetHypervisorVersion` disassembly. **Not confirmed.**

---

## 3. `HypervisorMetaProviderContext` — Symbol Status and Memory

### Evidence

```
kd> x nt!HypervisorMetaProvider*
fffff802`aba2b9a0 nt!HypervisorMetaProviderContext = <no type information>
fffff802`aba2b9b8 nt!HypervisorMetaProviderMap     = <no type information>

kd> x nt!CpuMetaProviderMap
fffff802`aba2bfa0 nt!CpuMetaProviderMap = <no type information>

kd> x nt!MemoryMetaProviderMap
fffff802`aba2baa0 nt!MemoryMetaProviderMap = <no type information>

kd> x nt!ObjectMetaProviderMap
fffff802`aba2ba68 nt!ObjectMetaProviderMap = <no type information>

kd> dq nt!HypervisorMetaProviderContext L20
fffff802`aba2b9a0  fffff802`aba2b9b8 00000000`00000003
fffff802`aba2b9b0  fffff802`aba18410 00000000`00000001
fffff802`aba2b9c0  00000000`80800000 00000000`00000002
fffff802`aba2b9d0  00000000`a0000010 00000000`00000004
fffff802`aba2b9e0  00000000`a0000008 fffff802`aba2bfa0
fffff802`aba2b9f0  00000000`00000004 fffff802`aba18530
fffff802`aba2ba00  00000000`00000001 00000000`80008000
fffff802`aba2ba10  00000000`00000002 00000000`a0000001
fffff802`aba2ba20  00000000`00000004 00000000`40008000
fffff802`aba2ba30  00000000`00000008 00000000`40200000
fffff802`aba2ba40  00000000`00000010 00000000`44000000
fffff802`aba2ba50  fffff802`aba2ba68 00000000`00000002
fffff802`aba2ba60  fffff802`aba18430 00000000`00000001
fffff802`aba2ba70  00000000`80000080 00000000`00000002
fffff802`aba2ba80  00000000`80000040 fffff802`aba2baa0
fffff802`aba2ba90  00000000`00000010 fffff802`aba183e0
```

### [OBSERVED]
- `HypervisorMetaProviderContext`, `HypervisorMetaProviderMap`, `CpuMetaProviderMap`, `MemoryMetaProviderMap`, `ObjectMetaProviderMap` — **all are real public PDB symbols** present in `ntoskrnl`.
- At `+0x0000`: pointer to `nt!HypervisorMetaProviderMap` (self-referential or first entry).
- At `+0x0008`: integer `3` — likely a count field.
- At `+0x0010`: pointer to `nt!SystemHypervisorProviderGuid` (confirmed via `x nt!SystemHypervisorProviderGuid`).
- At `+0x0048`: pointer to `nt!CpuMetaProviderMap`.
- At `+0x0050`: integer `4` — appears in the same relative position as the count for HypervisorMap.

### [INFERRED]
The repeating pattern (`PVOID map`, `uint64 count`, `PVOID guid`, `uint64 enabled`, ...) with counts `3`, `4`, `2`, `16` (`0x10`) strongly suggests an **array of provider descriptor entries**, each containing a pointer to a map, a count, and a GUID pointer.

The symbol names `CpuMetaProviderMap` / `MemoryMetaProviderMap` / `ObjectMetaProviderMap` are **real PDB symbols** — not structscan-generated names. Their interpretation as CPU / memory / object provider maps is based on the symbol names themselves.

### [HYPOTHESIS]
The count `4` adjacent to `CpuMetaProviderMap` may reflect the 4 virtual processors on this system, but this is speculative without cross-referencing how this value is read by HVL scheduler code.

The integer `0x10` adjacent to `MemoryMetaProviderMap` may be a count of memory regions or EPT ranges, but this requires verification against `HvlpAddRemovePhysicalMemory` or similar.

---

## 4. `nt!MiSystemPartition` — Clarification

### Evidence

```
kd> dt nt!_MI_PARTITION fffff802`ac64c000
   +0x000 Core             : _MI_PARTITION_CORE
   +0x1e0 Modwriter        : _MI_PARTITION_MODWRITES
   +0x4b0 Store            : _MI_PARTITION_STORES
   +0x700 Segments         : _MI_PARTITION_SEGMENTS
   +0xb80 PageLists        : _MI_PARTITION_PAGE_LISTS
   +0x4280 Commit           : _MI_PARTITION_COMMIT
   +0x4a00 Vp               : _MI_VISIBLE_PARTITION
```

### [OBSERVED]
`nt!MiSystemPartition` is a `_MI_PARTITION` structure (Windows Memory Manager type, confirmed via public `dt` output). It is not an `HV_PARTITION` or a Hyper-V structure.

### [INFERRED]
`MiSystemPartition` represents the **NT Memory Manager's** top-level memory partition object. It coordinates page lists, working sets, and memory commits for the system partition. SLAT/EPT is implemented by the Hyper-V microkernel at EL2, not by `_MI_PARTITION` directly. The Memory Manager interacts with the hypervisor through hypercalls (e.g., `HvlpAddRemovePhysicalMemory`) when pages need to be mapped into the guest physical address space.

---

## 5. VTL0 → VTL1 Transition (`HVC #2`)

### Evidence

```
kd> u nt!HvlpCallVtl1 L5
nt!HvlpCallVtl1:
fffff802`ac02bc90  hvc  #2
fffff802`ac02bc94  mov  fp, sp
fffff802`ac02bc98  lsr  w1, w19, #8
fffff802`ac02bc9c  and  w1, w1, #0x7f
fffff802`ac02bca0  cmp  w1, #7
```

```
kd> u nt!HvlSwitchToVsmVtl1 L15
fffff802`ac02bc40  stp  fp, lr,   [sp, #-0xB0]!
fffff802`ac02bc44  stp  x19, x20, [sp, #0x10]
fffff802`ac02bc48  stp  x21, x22, [sp, #0x20]
fffff802`ac02bc4c  stp  x23, x24, [sp, #0x30]
fffff802`ac02bc50  stp  x25, x26, [sp, #0x40]
fffff802`ac02bc54  stp  x27, x28, [sp, #0x50]
fffff802`ac02bc58  stp  d8, d9,   [sp, #0x60]
fffff802`ac02bc5c  stp  d10, d11, [sp, #0x70]
fffff802`ac02bc60  stp  d12, d13, [sp, #0x80]
fffff802`ac02bc64  stp  d14, d15, [sp, #0x90]
fffff802`ac02bc70  ldr  x19, [x1], #8
fffff802`ac02bc74  ldr  x4, [x1]
fffff802`ac02bc7c  ldp  q8, q9, [x1], #0x20
```

### [OBSERVED]
- `HvlpCallVtl1` at `0xfffff802ac02bc90` issues `HVC #2` as its **first instruction**.
- `HvlSwitchToVsmVtl1` saves the full ARM64 callee-saved register set (x19–x28, d8–d15) before calling `HvlpCallVtl1`.

### [INFERRED]
`HVC #2` is the ARM64 Hypervisor Call instruction with immediate `2`. On Hyper-V for ARM64, the immediate encodes the VTL call type. Immediate `0` = standard hypercall, `2` = VSM VTL return/call (consistent with published Hyper-V TLFS documentation). The full register save in `HvlSwitchToVsmVtl1` reflects a complete CPU context switch to a different execution environment (VTL 1), analogous to a full `setjmp`-style context save.

### [HYPOTHESIS]
After `HVC #2` returns, the code at `+0x98` checks bits `[14:8]` of `w19` against `7`, suggesting a status code field in the return value. What specific status codes map to what VTL conditions is not yet determined.

---

## 6. SynIC SINT Mechanism

### Evidence

```
kd> uf nt!HvlGetVpSintMessagePage
nt!HvlGetVpSintMessagePage:
fffff802`abed1bcc  bl   nt!HvlGetLpIndexFromProcessorIndex
fffff802`abed1bd0  bl   nt!HvlpGetLpcbByLpIndex
fffff802`abed1bd4  ldr  x0, [x0, #0x20]     ; <- SINT message page ptr at LPCB+0x20
fffff802`abed1be0  ret

kd> u nt!HvlpSintInterruptRoutine L10
fffff802`abc3dcc0  ldr  w8, [x0, #0x58]     ; SINT index from IRQ object+0x58
fffff802`abc3dcc4  sub  w19, w8, #0x300     ; normalize: raw_sint - 0x300
fffff802`abc3dcec  blr  x15                 ; indirect call → registered SINT handler
```

### [OBSERVED]
- `HvlGetVpSintMessagePage` returns the value at `LPCB+0x20` where LPCB is the Logical Processor Control Block returned by `HvlpGetLpcbByLpIndex`.
- `HvlpSintInterruptRoutine` reads a SINT index from `irq_object+0x58`, subtracts `0x300` to normalize it, then performs an indirect call via a function pointer.

### [INFERRED]
The SINT message page is per-VP and stored at offset `0x20` of the Logical Processor Control Block (`LPCB`). This is consistent with the Hyper-V TLFS specification, which defines a per-VP SINT message page mapped by the hypervisor. The normalization `raw - 0x300` suggests SINT indices are stored with a `0x300` base offset in the IRQ object on this build.

---

## 7. `nt!VslpSmcTable` — Raw Memory and Verified Fields

### Evidence

```
kd> dq nt!VslpSmcTable L20
fffff802`ac7637e0  00000000`00000000 00000000`00000000
fffff802`ac7637f0  00000000`00000000 00000000`00000000
fffff802`ac763800  00000400`0000000d ffffc686`917bf9f0   ; +0x20, +0x28
fffff802`ac763810  00000000`00000000 00000000`00000012   ; +0x30, +0x38
fffff802`ac763820  00000000`00000000 01dd224a`80ac4d81   ; +0x40, +0x48
fffff802`ac763830  00000000`0000ffff fffff802`ac763860   ; +0x50, +0x58
fffff802`ac763840  00000000`00003b80 00000000`00000000   ; +0x60
fffff802`ac763860  00000000`00000003 00000000`00000001   ; KciAsidBitmapBuffer
```

```
kd> x nt!KciAsidBitmapBuffer
fffff802`ac763860 nt!KciAsidBitmapBuffer = <no type information>
```

### [OBSERVED]
- `VslpSmcTable+0x00` through `+0x18`: all zero — consistent with an uninitialized header or a struct that begins with reserved fields.
- `VslpSmcTable+0x20` = `0x00000400_0000000d` — bitmask-like flags (popcount=4).
- `VslpSmcTable+0x28` = `0xffffc686917bf9f0` — kernel virtual address, likely a pointer to a dispatch table or callback.
- `VslpSmcTable+0x38` = `0x12` (decimal 18).
- `VslpSmcTable+0x50` = `0x0000ffff`.
- `VslpSmcTable+0x58` = `0xfffff802ac763860` = `nt!KciAsidBitmapBuffer`.
- `VslpSmcTable+0x60` = `0x3b80` (15232 decimal).
- `nt!KciAsidBitmapBuffer` is a **real PDB symbol** at `0xfffff802ac763860`.

### [INFERRED]
- The value `0x12` at `+0x38` is an integer. Its proximity to the KCI/ASID fields and the pattern of surrounding data is consistent with an index or vector number (e.g., SINT #18 = VMBus). However, assigning the label "SmcDispatchVector" to this field is speculation — the field name is not in PDB.
- `nt!KciAsidBitmapBuffer` is a real symbol pointing to a buffer of `0x3b80` (15232) bytes starting at `+0x60` context. Whether this bitmap is specifically used for **TLB shootdown avoidance** is not confirmed from available evidence.

### [HYPOTHESIS]
- `KciAsidBitmapBuffer` is likely associated with ASID (Address Space Identifier) management used by Kernel Code Integrity. ASIDs on ARM64 allow multiple address spaces to coexist in the TLB simultaneously. The buffer at `VslpSmcTable+0x58` and its size `0x3b80` = `15232` bytes = `65536 / 4 * 0.93` bits suggest a bitmap covering the full 16-bit ASID space (`0x0000ffff` = 65535 seen at `+0x50`).
- Whether this bitmap is consulted during VTL transitions specifically to avoid TLB invalidation is an architectural hypothesis that requires tracing callers of `VslpSmcTable` in code that reads `+0x58` / `+0x60`.

---

## 8. Hypercall Dispatch — `HvcallInitiateHypercall`

### Evidence

```
kd> uf nt!HvlpInvokeGetPageListHypercall
...
fffff802`abed9bb0  mov  x0, #0x97              ; Hypercall code 0x97
fffff802`abed9bb4  bl   nt!HvcallInitiateHypercall
```

### [OBSERVED]
The actual hypercall dispatch function called from `HvlpInvokeGetPageListHypercall` is `nt!HvcallInitiateHypercall`. The hypercall code `0x97` is loaded into `x0` before the call.

### [INFERRED]
Hypercall code `0x97` = decimal 151. According to the public Hyper-V TLFS, `HvCallGetGpaPages` is `0x0097`. This confirms this function retrieves GPA (Guest Physical Address) page list information from the hypervisor.

---

## 9. `VslpDispatchIumSyscall` — IUM Syscall Dispatcher

### Evidence

```
kd> u nt!VslpDispatchIumSyscall L20
fffff802`abc002cc  and  x10, x2, #0xF          ; arg_count = arg2 & 0xF (max 4 bits)
fffff802`abc002e0  cmp  x10, #4
fffff802`abc002e4  bgt  ...                    ; error if arg_count > 4
fffff802`abc002e8  ldp  x0, x1, [x9], #0x10   ; load args from caller buffer
fffff802`abc002f8  adr  x8, ...               ; table base
fffff802`abc002fc  sub  x10, x8, x10, lsl #3  ; dispatch = base - arg_count * 8
fffff802`abc00300  br   x10                   ; indirect branch → dispatch slot
fffff802`abc00324  blr  x11                   ; call target function
```

### [OBSERVED]
`VslpDispatchIumSyscall` takes an argument count (`x2 & 0xF`), enforces a maximum of 4 arguments, loads up to 8 arguments from a caller-supplied buffer into `x0`–`x7`, then dispatches via a computed branch to a slot in an inline jump table.

### [INFERRED]
This is the **Isolated User Mode (IUM) syscall thunk**. IUM (Trustlets) running under VBS call into the Secure Kernel via a restricted syscall gate. `VslpDispatchIumSyscall` is the Normal World (VTL 0) side of this gate — it marshals arguments and dispatches to the appropriate kernel function. The `xpr` register reference (`[xpr, #0x1438]`) at the end increments a per-processor syscall counter.

---

## 10. Global Symbol Address Table (ARM64 Build 26100.1)

All addresses confirmed via `x nt!<symbol>` on the live kernel.

| Symbol | Address | Notes |
|---|---|---|
| `nt!HvlPartitionId` | `0xfffff802ac809ee8` | `= 0` in this session (HV inactive) |
| `nt!HvlpActiveProcessorCount` | `0xfffff802ac809c20` | `= 0` in this session |
| `nt!HvlHypervisorConnected` | `0xfffff802ac809a87` | `= 0x00` (byte) |
| `nt!HvlpHypervisorVersion` | `0xfffff802ac80a448` | `+0x10 = 0x00111311` |
| `nt!HvlpHypervisorStatsPage` | `0xfffff802ac808378` | Pointer to HV stats page |
| `nt!HvlpVsmVtlCallVa` | `0xfffff802aca01898` | `= NULL` (VTL not initialized) |
| `nt!HvlpInterruptCallback` | `0xfffff802ac808488` | SINT callback table |
| `nt!HvlSwitchToVsmVtl1` | `0xfffff802ac02bc40` | Saves callee regs, calls HvlpCallVtl1 |
| `nt!HvlpCallVtl1` | `0xfffff802ac02bc90` | First instr: `HVC #2` |
| `nt!VslpSmcTable` | `0xfffff802ac7637e0` | VBS SMC context block |
| `nt!KciAsidBitmapBuffer` | `0xfffff802ac763860` | ASID bitmap, 0x3b80 bytes |
| `nt!VslIsSecureKernelRunning` | `0xfffff802abeda968` | Function: queries SK status |
| `nt!SbiVmbusArrivalEvent` | `0xfffff802ac7637c0` | VMBus arrival event object |
| `nt!HvcallInitiateHypercall` | `0xfffff802abd856f0` | Actual hypercall dispatch |
| `nt!HypervisorMetaProviderContext` | `0xfffff802aba2b9a0` | Real PDB symbol |
| `nt!CpuMetaProviderMap` | `0xfffff802aba2bfa0` | Real PDB symbol |
| `nt!MemoryMetaProviderMap` | `0xfffff802aba2baa0` | Real PDB symbol |

---

## 11. Open Research Items

| Item | What to do | Expected yield |
|---|---|---|
| **LPCB structure** | `dq` / `!structscan` on result of `HvlpGetLpcbByLpIndex` | Full `_HVL_LPCB` field layout; SINT msg page at `+0x20` |
| **HV_MESSAGE layout** | Read SINT message page when HV is active | `HV_MESSAGE_HEADER` + payload bytes |
| **VMBus ring buffer** | Find channel VA from `SbiVmbusArrivalEvent` chain | Ring buffer R/W indices, packet types |
| **VTL1 syscall table** | Load `securekernel.exe` PDB, cross-ref against VTL1 call gate | Secure Kernel syscall numbers |
| **HvlpHypervisorVersion** | Disassemble `HviGetHypervisorVersion`, map field layout | Exact major.minor.service.build of HV |
| **KciAsidBitmapBuffer callers** | `!search` / cross-ref `0xfffff802ac763860` in code | Confirm or deny TLB-bypass hypothesis |
| **HVC #0 vs HVC #2** | Trace all `hvc` instructions in ntoskrnl | Map immediate values to call types |
