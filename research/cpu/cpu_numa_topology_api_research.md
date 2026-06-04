# CPU & NUMA Topology API Research — Kernel-Level Analysis

> WinDbg kernel debugging | Windows 11 ARM64 (Build 26100) | 4-core VM

---

## 1. Architecture Overview

```
User Mode                          Kernel Mode
─────────                          ──────────
GetLogicalProcessorInformation  → NtQuerySystemInformation (SystemLogicalProcessorInformation)
GetLogicalProcessorInformationEx → NtQuerySystemInformation (SystemLogicalProcessorInformationEx)
GetNumaHighestNodeNumber        → KeQueryHighestNodeNumber
GetNumaNodeProcessorMask        → ExpQueryNumaProcessorMap → KeQueryNodeActiveAffinity
GetNumaNodeProcessorMaskEx      → ExpQueryNumaProcessorMap (Ex variant)
VirtualAllocExNuma              → NtAllocateVirtualMemoryEx → MiComputePreferredNode
SetThreadGroupAffinity          → NtSetInformationThread → KeSetUserGroupAffinityThread
GetThreadGroupAffinity          → NtQueryInformationThread → KeQueryPrimaryGroupThread
```

**Central global:** `nt!KeNumberNodes` (`fffff800'70008000`) — packed structure:
- +0x000: `HighestNodeNumber` (USHORT) — value 0x0001 (1 node, highest index = 0)
- +0x002: padding
- +0x004: (flags)
- +0x014: `ActiveGroupCount` (USHORT) — via KeQueryActiveGroupCount
- +0x018: `MaximumGroupCount` (USHORT) — via KeQueryMaximumGroupCount

---

## 2. KNODE — Kernel NUMA Node Structure

```
dt nt!_KNODE
   +0x000 NodeNumber       : Uint2B          // NUMA node index
   +0x002 PrimaryNodeNumber : Uint2B         // Primary sub-node index
   +0x004 ProximityId      : Uint4B          // ACPI proximity domain ID
   +0x008 MaximumProcessors : Uint2B         // Max processors for this node
   +0x00a Flags            : <unnamed-tag>   // Node capability flags
   +0x00b GroupSeed        : UChar           // Round-robin group selector
   +0x00c PrimaryGroup     : UChar           // Primary group for this node
   +0x00d Padding          : [3] UChar
   +0x010 ActiveGroups     : _KGROUP_MASK    // 16 bytes: bitmasks per group
   +0x020 SchedulerSubNodes : [32] Ptr64 _KSCHEDULER_SUBNODE
   +0x120 ActiveTopologyElements : [6] Uint4B  // Cache/NUMA/core counts
   +0x138 PerformanceSearchRanks : [1] _KNODE_SUBNODE_SEARCH_RANKS
   +0x158 EfficiencySearchRanks : [1] _KNODE_SUBNODE_SEARCH_RANKS
```

**Live data on this system (4-core ARM64, single NUMA):**
```
NODE 0 (FFFFF80070014300):
    ProximityId      : 0
    Capacity         : 4

    SUBNODE (FFFFF8006FE17900):
        Group            : 15
        ProcessorMask    : (f)
        IdleCpuSet       : 0000000f
        IdleSmtSet       : 0000000f
        NonParkedSet     : 00000001
```

---

## 3. GetLogicalProcessorInformation / GetLogicalProcessorInformationEx

### 3.1 Kernel entry

```
User API → NtQuerySystemInformation (info class 107 or 0x73)
         → ExpQuerySystemInformation
         → KeQueryLogicalProcessorRelationship
```

### 3.2 KeQueryLogicalProcessorRelationship

**Address:** `fffff800'6f468590`

This is a massive function (~2400 bytes, 0x968 instructions for the main path). Key observations:

**Frame setup:**
```armasm
; Stack frame: 0xE0 bytes + local vars
; x20 = RelationshipType (LOGICAL_PROCESSOR_RELATIONSHIP enum)
; x23 = 0xFFFF (ALL_PROCESSOR_GROUPS sentinel)
; x27 = &KeNumberNodes
; x25 = KeNumberNodes->HighestNodeNumber (ldrh [x27,#0x18])
; x24 = (HighestNodeNumber + 1) * 8 + 8 = node array size
```

**Flow:**
1. Reads `RelationshipType` from parameter (x1 → w20)
2. If `RelationshipType == 0xFFFF` (ALL_PROCESSOR_GROUPS), branch to extended path
3. Reads `KeNumberNodes+0x18` → number of active groups
4. Allocates stack buffer for per-group affinity tracking
5. Iterates over groups: for each group, calls internal topology fill
6. Returns `SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX` array to user buffer

**Key internal calls:**
- Uses `KeQueryGroupAffinity(groupIndex)` to get per-group processor masks
- Uses `KeGetProcessorNumberFromIndex` to map linear indices to `PROCESSOR_NUMBER`
- Compiles results into `SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX` structures

### 3.3 Supporting structures

```
_PROCESSOR_NUMBER:
   +0x000 Group    : Uint2B    // Processor group (0-based)
   +0x002 Number   : UChar     // Processor number within group
   +0x003 Reserved : UChar

_GROUP_AFFINITY:
   +0x000 Mask     : Uint8B    // Affinity mask (up to 64 processors)
   +0x008 Group    : Uint2B    // Group number
   +0x00a Reserved : [3] Uint2B

_KGROUP_MASK:
   +0x000 Masks    : [2] Uint8B  // 128-bit mask (supports up to 128 processors)
```

---

## 4. GetNumaHighestNodeNumber

### 4.1 KeQueryHighestNodeNumber

**Address:** `fffff800'6f468570` (3 instructions, leaf function)

```armasm
nt!KeQueryHighestNodeNumber:
    adrp    x8, nt!KeNumberNodes          ; load global address
    ldrh    w8, [x8]                       ; w8 = KeNumberNodes.HighestNodeNumber
    mov     w9, #0xFFFF                    ; sentinel: ALL_NODES = 0xFFFF
    add     w8, w8, w9                     ; w8 = HighestNodeNumber + 0xFFFF
    uxth    w0, w8                         ; zero-extend to 16-bit
    ret
```

**Mechanism:** Returns `KeNumberNodes[0] + 0xFFFF` as USHORT. Since HighestNodeNumber is stored as the actual node index (0-based), this computes the total count via modular arithmetic. The API returns the highest node NUMBER (0-based), which is `KeNumberNodes[0]`.

On this system: `KeNumberNodes[0] = 0x0001` → HighestNodeNumber = 0.

---

## 5. GetNumaNodeProcessorMask / GetNumaNodeProcessorMaskEx

### 5.1 ExpQueryNumaProcessorMap

**Address:** `fffff800'6fb76950`

**Flow (clean, ~200 bytes):**
```armasm
; Input: Buffer, Length, ReturnLength
1. if (Length < 4) → STATUS_INFO_LENGTH_MISMATCH
2. call KeQueryHighestNodeNumber → store as first DWORD
3. Compute: numNodes = HighestNodeNumber + 1
4. Compute: fitCount = (Length - 8) / 16   // Each entry = 16 bytes
5. actualCount = min(fitCount, numNodes)
6. if (Length == 8) → done, return 0
7. if (fitCount > numNodes) → done, return 0
8. FOR each node from 0 to actualCount-1:
     offset = node * 16 + 8
     call KeQueryNodeActiveAffinity(node, &buffer[offset], NULL)
9. Return STATUS_SUCCESS
```

**Output format:** Array of `GROUP_AFFINITY` structures (16 bytes each), preceded by a DWORD count.

### 5.2 KeQueryNodeActiveAffinity

**Address:** `fffff800'6f469120`

```armasm
; Input: NodeNumber, out GROUP_AFFINITY, out USHORT GroupNumber
1. Zero out the output GROUP_AFFINITY
2. Zero out GroupNumber
3. Validate: if (NodeNumber >= KeNumberNodes) → return
4. Lookup: KiNodeArray[NodeNumber] (from KiClockState+0xD00 table)
5. Call KeGetNodePrimarySubNode(knode) → get primary sub-node
6. If sub-node exists → call KiQuerySubNodeActiveAffinity(subnode, affinity, group)
```

**Key offset:** Node array at `KiClockState + 0xD00` (array of KNODE pointers indexed by node number).

### 5.3 KeGetNodePrimarySubNode

**Address:** `fffff800'6f468218`

- Copies KNODE.ActiveGroups (16 bytes) via `RtlCopyVolatileMemory`
- Calls `RtlNumberOfSetBitsEx` to count active groups
- If groups exist, uses KNODE.PrimaryGroup (+0x0C) as seed to index SchedulerSubNodes[]
- Returns `_KSCHEDULER_SUBNODE*` for the primary sub-node

---

## 6. VirtualAllocExNuma

### 6.1 Kernel path

```
VirtualAllocExNuma → NtAllocateVirtualMemoryEx
                  → MmAllocateVirtualMemory
                  → MiComputePreferredNode → MiThreadIdealNode
```

The NUMA node parameter flows through as a "PreferredNode" hint in the allocation attributes.

### 6.2 MiComputePreferredNode

**Address:** `fffff800'6f7d1290`

```armasm
; Input: VAD (x1), returns node number in w0
1. w9 = VAD[0x30]                    ; VAD flags/allocation attributes
2. w8 = ubfx(w9, #12, #7)            ; extract bits [12:18] → PreferredNode (7 bits)
3. if (PreferredNode != 0) → return PreferredNode - 1
4. if (bit 21 of VAD[0x30] set) → goto MiThreadIdealNode
5. x8 = VAD[0x48] → x8[0] → w8 = [x8+0x38]
6. w8 = ubfx(w8, #20, #7)            ; extract from parent VAD / EPROCESS
7. if (w8 != 0) → return w8 - 1
8. Fallback: call MiThreadIdealNode(NULL, NULL)
```

**Key insight:** The preferred NUMA node is stored in VAD+0x30 bits [12:18] (7-bit field). If not explicitly set, falls back to:
1. Process-wide preferred node (from EPROCESS → VAD chain)
2. Thread ideal node via `MiThreadIdealNode`

### 6.3 MiThreadIdealNode

**Address:** `fffff800'6f589a50`

```armasm
; Input: optional VAD (x0), optional out KNODE** (x1)
; x10 = EPROCESS (from KPRCB+0x988)
1. If VAD provided:
   a. Check VAD[0xB8] flags: if (flags & 0xF) → use thread-based lookup
   b. Check EPROCESS+0x26A (NUMA awareness byte): if == 1 → use VAD-based node
   c. Read VAD[0xB0] → KNODE pointer → KNODE[0x10C] → node number
2. Thread-based lookup:
   a. w9 = EPROCESS[0x26C]  ; processor index
   b. Lookup global processor table (nt!KiZeroedDebugRegs+0x780)
   c. Read [table_entry + 0x1164] → ideal node number
```

**Key offsets:**
- `EPROCESS+0x26A`: NUMA awareness flag (1 = NUMA-aware process)
- `EPROCESS+0x26C`: current processor index (DWORD)
- `EPROCESS+0xB0`: KNODE pointer (when NUMA-aware)
- `KPRCB+0x988`: EPROCESS pointer
- Processor table entry+0x1164: ideal NUMA node number

### 6.4 NUMA-aware memory allocation chain

```
VirtualAllocExNuma(node=N)
  → NtAllocateVirtualMemoryEx (ExtendedParameterType = MemExtendedParameterNumaNode)
  → MmAllocateVirtualMemory
  → MiCreateVad / MiInsertVad
    → VAD[0x30] bits [12:18] = N + 1 (encoded: 0 = default)
  → On page fault:
    → MiComputeFaultNode
      → MiNodeFromFaultPacket (prefetch hint)
      → Reads VAD[0x30] to extract preferred node
      → If not set → MiThreadIdealNode
    → Physical page allocated from preferred node's free list
```

### 6.5 MiComputeFaultNode

**Address:** `fffff800'6f589668`

Multi-layered node resolution for page faults:
1. `MiNodeFromFaultPacket` — checks if fault packet carries node hint
2. If VAD has preferred node (VAD[0x30] bits [12:18]) → use it
3. Falls through to check EPROCESS NUMA state
4. Checks process VAD tree for address range → locates existing VAD
5. Reads `VAD[0x30]` bits [12:18] from found VAD → preferred node
6. If still no node → checks `EPROCESS[0xB8]` NUMA flags
7. Reads EPROCESS+0xB0 → KNODE → KNODE+0x10C for node number
8. Marks `VAD[0x50] |= 0x20000` (NUMA-resolved flag) to avoid re-resolution

---

## 7. SetThreadGroupAffinity / GetThreadGroupAffinity

### 7.1 SetThreadGroupAffinity kernel path

```
SetThreadGroupAffinity → NtSetInformationThread (ThreadGroupInformation)
                       → KeSetUserGroupAffinityThread
```

### 7.2 KeSetUserGroupAffinityThread

**Address:** `fffff800'6f466a48` (0x1A0 bytes)

**Full flow:**
```armasm
1. Allocate 0x110-byte stack frame (local GROUP_AFFINITY + buffer)
2. Zero-fill local buffer (128 bytes = KGROUP_MASK)
3. Call KeVerifyGroupAffinity(input, UserMode=1)
   → Validates GROUP_AFFINITY.Mask and GROUP_AFFINITY.Group
   → Returns BOOLEAN (success/fail)
4. If validation fails → return error code (from data section)

5. Build internal KGROUP_MASK from input:
   a. Read input.Group → index into local mask array
   b. OR input.Mask into local[index]

6. Raise IRQL to DISPATCH_LEVEL (w0 = 2)
7. Load KTHREAD+0x240 → KPROCESS (ETHREAD.ThreadProcess)
8. Acquire ExAcquireSpinLockSharedAtDpcLevel(KPROCESS+0x48)
   → Shared spinlock on process affinity lock
9. Load KPROCESS+0x58 → current system affinity (GROUP_AFFINITY)
10. Call KeIsSubsetAffinityEx(new_affinity, current_system_affinity)
    → Validates new affinity is subset of process-wide system affinity
11. If NOT subset → release lock, lower IRQL, return STATUS_INVALID_PARAMETER

12. Copy affinity → KiCopyAffinityEx
13. Call KiSetAffinityThread(Thread, &old_affinity, &new_affinity, old_irql)
    → Core function: updates KTHREAD affinity fields
    → May trigger thread migration
14. Release ExReleaseSpinLockSharedFromDpcLevel(KPROCESS+0x48)
15. Call KiProcessDeferredReadyList(KPRCB+0x980)
    → Processes any threads now ready to run on new processor
16. Return STATUS_SUCCESS (0)
```

**Key offsets:**
- `KTHREAD+0x240`: KPROCESS pointer (current process)
- `KPROCESS+0x48`: Shared spinlock for affinity changes
- `KPROCESS+0x58`: Current system group affinity (GROUP_AFFINITY)
- `KPRCB+0x980`: Deferred ready thread list header

**Locking model:**
- IRQL raised to DISPATCH_LEVEL (2)
- Shared spinlock acquired on KPROCESS+0x48
- Subset validation ensures thread affinity ⊆ process affinity
- On failure: `STATUS_INVALID_PARAMETER` (0xC0000001)

### 7.3 KeRevertToUserGroupAffinityThread

**Address:** `fffff800'6f4658a0` (~0x120 bytes)

Restores previously saved user affinity. Key operations:
1. Reads `KPROCESS+0x6C` flags (bit 3 = HotPlugProcessor check)
2. Validates previous affinity structure
3. Raises IRQL to DISPATCH_LEVEL
4. Acquires KPROCESS+0x48 shared spinlock
5. Reads KPROCESS+0x470 (saved user affinity chain)
6. Calls KiSetAffinityThread to restore
7. Releases lock, processes deferred ready list

### 7.4 GetThreadGroupAffinity path

```
GetThreadGroupAffinity → NtQueryInformationThread (ThreadGroupInformation)
                       → KeQueryPrimaryGroupThread
```

### 7.5 KeQueryActiveGroupCount

**Address:** `fffff800'6f468450` (3 instructions)

```armasm
nt!KeQueryActiveGroupCount:
    adrp    x8, nt!KeNumberNodes          ; fffff800'70008000
    ldrh    w0, [x8, #0x14]               ; KeNumberNodes + 0x14
    ret
```

Reads `KeNumberNodes+0x14` directly — a single memory load.

### 7.6 KeQueryMaximumGroupCount

**Address:** `fffff800'6f469030` (3 instructions)

```armasm
nt!KeQueryMaximumGroupCount:
    adrp    x8, nt!KeNumberNodes          ; fffff800'70008000
    ldrh    w0, [x8, #0x18]               ; KeNumberNodes + 0x18
    ret
```

Reads `KeNumberNodes+0x18` directly.

---

## 8. KeGetCurrentNodeNumber / KeGetProcessorNodeNumber

### 8.1 KeGetCurrentNodeNumber

**Address:** `fffff800'6f4681c0` (4 instructions)

```armasm
nt!KeGetCurrentNodeNumber:
    mov     x8, xpr                        ; KPRCB (current processor)
    ldr     x8, [x8, #0x1938]             ; KPRCB → CurrentNode (KNODE*)
    ldrh    w0, [x8, #0x10A]              ; KNODE+0x10A → NodeNumber
    ret
```

**Ultra-fast:** 2 memory loads + return. Reads KPRCB+0x1938 → KNODE pointer → KNODE+0x10A.

### 8.2 KeGetProcessorNodeNumber

**Address:** `fffff800'6f4682f8` (3 instructions)

```armasm
nt!KeGetProcessorNodeNumber:
    ldr     x8, [x0, #0xFB8]             ; KPRCB(ProcessorIndex) → via table
    ldrh    w0, [x8, #0x10A]              ; KNODE+0x10A → NodeNumber
    ret
```

Same KNODE+0x10A offset, but takes explicit KPRCB pointer as input.

### 8.3 KeGetProcessorNumberFromIndex

**Address:** `fffff800'6f468320`

Maps linear processor index → `PROCESSOR_NUMBER {Group, Number}`:

```armasm
1. If (index == 0) → return {Group=0, Number=0}
2. Validate: index < total_processor_count (from PpmPolicyConfigTable+0xAE8)
3. Lookup: KiProcessorNumberTable[index] → packed DWORD
4. Extract: Group = ubfx(value, #6, #16)  ; bits [6:21]
5. Extract: Number = value & 0x3F          ; bits [0:5]
6. Store into PROCESSOR_NUMBER structure
```

**Packed processor number format:** `bits[0:5] = Number, bits[6:21] = Group`

---

## 9. KeQueryGroupAffinity

**Address:** `fffff800'6f468540`

```armasm
nt!KeQueryGroupAffinity:
    adrp    x8, HvlpQueryProximityId       ; base of group data
    add     x10, x8, #0xA0                 ; group count header
    ldrh    w8, [x10]                      ; group count
    uxth    w9, w0                         ; group index
    cmp     w9, w8                         ; bounds check
    bhs     return_zero                    ; invalid group
    ldr     x0, [x10 + 8 + w9*8]          ; affinity mask for group
    ret
```

Array of KAFFINITY masks at `HvlpQueryProximityId + 0xA8`, indexed by group number.

---

## 10. KeSelectGroupFromNode

**Address:** `fffff800'6f469300`

Round-robin group selection within a KNODE:

```armasm
1. Read KNODE.GroupSeed (+0x0B)
2. Copy KNODE.ActiveGroups (+0x10, 16 bytes)
3. Call KeFindNextSetRightGroupMask(&ActiveGroups, (GroupSeed + 1) & 0x7F)
   → Finds next set bit in group mask (round-robin)
4. Update KNODE.GroupSeed = selected_group
5. Return selected group number
```

**Purpose:** For multi-group NUMA nodes, this distributes threads across groups in round-robin fashion using the KNODE.GroupSeed counter.

---

## 11. NUMA Initialization Chain

```
KeNumaInitialize (fffff800'6fd47c40)
  → Parses ACPI SRAT (Static Resource Affinity Table)
  → Builds KNODE structures
  → Fills KiNodeGraph
  → MiInitializeNuma (fffff800'6f7d12c8)
    → MiInitializeNumaRangesTemporary
    → MiInitializeNumaRangesPermanent
    → MiInitializeNumaGraph
    → MiComputeNumaCosts
  → MiComputeMemoryNodeProcessorAssignments
  → MiReassignProcessorsToMemoryOnlyNodes
```

**Key globals:**
- `KeRootProcNumaNodes` — root processor NUMA node array
- `KeRootProcNumaNodeLps` — LP counts per node
- `KeRootProcNumaNodesSpecified` — node count
- `KiNodeGraph` — inter-node cost matrix
- `KiNodeInit` — initialization flag

---

## 12. Performance Characteristics

| API | Kernel Function | Instructions | Locking | Memory Access |
|-----|----------------|--------------|---------|---------------|
| GetNumaHighestNodeNumber | KeQueryHighestNodeNumber | 5 | None | 1 load (global) |
| GetCurrentNodeNumber | KeGetCurrentNodeNumber | 3 | None | 2 loads (KPRCB→KNODE) |
| GetActiveGroupCount | KeQueryActiveGroupCount | 3 | None | 1 load (global) |
| GetMaximumGroupCount | KeQueryMaximumGroupCount | 3 | None | 1 load (global) |
| GetGroupAffinity | KeQueryGroupAffinity | 7 | None | 2 loads (table) |
| GetProcessorNodeNumber | KeGetProcessorNodeNumber | 3 | None | 2 loads (KPRCB→KNODE) |
| SetThreadGroupAffinity | KeSetUserGroupAffinityThread | ~80 | Shared SpinLock + IRQL | Multiple |
| GetNumaNodeProcessorMask | ExpQueryNumaProcessorMap | ~40 | None | Per-node iteration |
| VirtualAllocExNuma | MiComputePreferredNode | ~20 | None | VAD + EPROCESS reads |
| GetLogicalProcessorInformationEx | KeQueryLogicalProcessorRelationship | ~600+ | None | Complex topology walk |

**Hot-path observations:**
- Node number queries are ultra-cheap (2-3 instructions, single load chain)
- Group count queries are single global loads
- Affinity changes require DISPATCH_LEVEL + shared spinlock
- Topology queries (GetLogicalProcessorInformationEx) are heavy but infrequent
- NUMA-aware allocations add minimal overhead (bit-field extraction from VAD)

---

## 13. KeNumberNodes Global Layout

```
fffff800'70008000 (KeNumberNodes):
  +0x000: 01 00   HighestNodeNumber = 1 (means: highest index = 0, count = 1)
  +0x002: 01 00   (flags/padding)
  +0x004: 00 01   (reserved)
  +0x006: 30 01   
  +0x008-00F: ... (node pointer array)
  +0x014: ?? ??   ActiveGroupCount
  +0x018: ?? ??   MaximumGroupCount
```

Raw dump:
```
fffff800'70008000  01 00 01 00 00 01 30 01-00 20 a0 c2 81 e5 ff ff
fffff800'70008010  00 00 00 00 01 00 ff 01-01 00 0f 01 00 80 00 00
```

---

## 14. NUMA-Aware Optimization Implications

### For the optimizer:

1. **GetLogicalProcessorInformationEx**: Pure query, no kernel state mutation. Safe to call at any time. The data is cached in kernel — topology doesn't change at runtime.

2. **GetNumaHighestNodeNumber**: 3-instruction leaf. Zero cost. Call freely.

3. **GetNumaNodeProcessorMask/Ex**: Iterates nodes, calls KeQueryNodeActiveAffinity per node. On typical systems (< 8 nodes), this is fast. No locks needed.

4. **VirtualAllocExNuma**: The preferred node hint is encoded in VAD+0x30 bits [12:18]. The memory manager uses this during page fault resolution (MiComputeFaultNode). The hint is advisory, not binding — pages may come from any node if the preferred node is under pressure.

5. **SetThreadGroupAffinity**: Requires shared spinlock acquisition at DISPATCH_LEVEL. Not cheap, but necessary for correctness. The affinity must be a subset of the process-wide affinity mask.

6. **Key NUMA optimization strategy**: Set thread affinity to the same node as the memory it accesses. Use `KeGetCurrentNodeNumber` (3 instructions) to determine current node, then allocate from that node via VirtualAllocExNuma.

### Critical kernel developer findings:

- **No kernel driver needed** for any of these APIs — they are all fully serviced by ntoskrnl
- NUMA topology is static after boot (set by KeNumaInitialize from ACPI SRAT)
- Page fault node resolution is already optimized (VAD cache + fallback to thread ideal node)
- The KNODE.SchedulerSubNodes array provides per-node scheduling domains
- KeSelectGroupFromNode handles automatic group round-robin for multi-group nodes
