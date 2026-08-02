# Windows PE Loader Research: `_LDR_DDAG_NODE` & Dependency Graph Internals (ARM64 / x64)

**Date:** 2026-08-02  
**Target:** Modern Windows PE Loader (`ntdll!Ldrp*`, Windows 10 / 11 24H2)  
**Tools:** WinDbg MCP Server, StructScan v4.5 Engine  
**Workspace:** `/Volumes/External/Code/winDows-Internals/KERNEL/LDR_DDAG_NODE_Research.md`

---

## 1. Architectural Evolution: From Flat Lists to Directed Acyclic Graphs (DAG)

Prior to Windows 10, the PE Loader initialized DLLs via flat double-linked lists (`InLoadOrderModuleList`, `InMemoryOrderModuleList`, `InInitializationOrderModuleList`). 

In modern Windows 10/11 (Build 26100+), Microsoft replaced flat list traversals with a **Directed Acyclic Graph (DAG)** managed by `_LDR_DDAG_NODE`. This architecture solves three critical challenges:
1. **Parallel Multi-Threaded DLL Initialization:** Allows threads to resolve independent dependency sub-graphs concurrently via worker queues (`ntdll!LdrpWorkQueue`).
2. **Circular Dependency Condensation:** Implements **Tarjan's Strongly Connected Components (SCC)** algorithm to detect circular imports (e.g. `DLL_A -> DLL_B -> DLL_A`) and collapse cyclic nodes into a single condensed node (`LdrModulesCondensed`).
3. **Safe Dynamic Unloading:** Tracks reference counts (`LoadCount`, `LoadWhileUnloadingCount`) to prevent race conditions during `FreeLibrary`.

---

## 2. Complete 15-Stage Lifecycle State Machine (`_LDR_DDAG_STATE`)

Extracted directly from symbols via WinDbg (`dt nt!_LDR_DDAG_STATE`):

```cpp
typedef enum _LDR_DDAG_STATE {
    LdrModulesMerged                   = -5, // Cyclic nodes merged into a single condensed node
    LdrModulesInitError                = -4, // DllMain returned FALSE / Initialization failed
    LdrModulesSnapError                = -3, // Import Address Table (IAT) resolution error
    LdrModulesUnloaded                 = -2, // Module unmapped from memory
    LdrModulesUnloading                = -1, // Unload sequence in progress
    LdrModulesPlaceHolder              =  0, // Node created, module memory not yet mapped
    LdrModulesMapping                  =  1, // Section mapping in progress
    LdrModulesMapped                   =  2, // Mapped into Virtual Memory, PE headers parsed
    LdrModulesWaitingForDependencies   =  3, // Resolving dependent imports
    LdrModulesSnapping                 =  4, // Binding IAT entries (Import Snapping)
    LdrModulesSnapped                  =  5, // All IAT imports bound successfully
    LdrModulesCondensed                =  6, // Cyclic SCC group collapsed
    LdrModulesReadyToInit              =  7, // Dependencies initialized, ready for DllMain
    LdrModulesInitializing             =  8, // Currently executing DllMain(DLL_PROCESS_ATTACH)
    LdrModulesReadyToRun               =  9  // Fully initialized & active
} LDR_DDAG_STATE;
```

---

## 3. Structure Layout & Field Semantics (`_LDR_DDAG_NODE`)

```cpp
typedef struct _LDR_DDAG_NODE {
    /* +0x000 */ LIST_ENTRY           Modules;                 // List of _LDR_DATA_TABLE_ENTRY instances in this node
    /* +0x010 */ PVOID                 ServiceTagList;          // Pointer to _LDR_SERVICE_TAG_RECORD
    /* +0x018 */ ULONG                 LoadCount;               // Active reference count (FreeLibrary decrements)
    /* +0x01c */ ULONG                 LoadWhileUnloadingCount; // Lock count protecting against concurrent unload races
    /* +0x020 */ ULONG                 LowestLink;              // Tarjan's SCC: Minimum reachable DFS preorder number
    /* +0x028 */ LDRP_CSLIST           Dependencies;            // Circular single-list of outgoing dependencies
    /* +0x030 */ LDRP_CSLIST           IncomingDependencies;    // Circular single-list of incoming dependencies
    /* +0x038 */ LDR_DDAG_STATE        State;                   // Current lifecycle state (-5 to +9)
    /* +0x040 */ SINGLE_LIST_ENTRY     CondenseLink;            // Single-linked chain for condensed cyclic nodes
    /* +0x048 */ ULONG                 PreorderNumber;          // Tarjan's SCC: Depth-first search visitation index
} LDR_DDAG_NODE, *PLDR_DDAG_NODE;
```

---

## 4. Graph Resolution Mechanics: Tarjan's SCC Algorithm in `ntdll`

When `LdrLoadDll` or `LdrpProcessWork` traverses module dependencies:

1. **DFS Traversal & Preorder Assignment:**
   - As each dependency is visited, `PreorderNumber` is assigned sequentially (`1, 2, 3...`).
   - `LowestLink` is initialized to match `PreorderNumber`.

2. **Cycle Detection (`LowestLink` Update):**
   - If a edge points to an already-visited node on the current DFS stack, `LowestLink` is updated to $\min(\text{LowestLink}, \text{PreorderNumber}_{\text{target}})$.

3. **Node Condensation (`ntdll!LdrpCondenseNode`):**
   - When a strongly connected component is identified ($\text{LowestLink} == \text{PreorderNumber}$), all modules in the cycle are linked via `CondenseLink` (`+0x040`) and their state transitions to `LdrModulesCondensed` (`6`) / `LdrModulesMerged` (`-5`).
   - Their `DllMain` entry points are then invoked in safe topological order.

---

## 5. StructScan v4.5 Verification Output

From live WinDbg inspection of `DdagNode` (`0xffffde055b90e420`):

| Offset | WinDbg PDB Symbol | Inferred Type | Value / Annotation |
| :--- | :--- | :--- | :--- |
| `+0x000` | `Modules` (`_LIST_ENTRY`) | **LIST_ENTRY / Handle** | Head of module entries |
| `+0x010` | `ServiceTagList` | **Integer** | `0x1a7d` |
| `+0x018` | `LoadCount` | **Flags / Integer** | `0x159d5` |
| `+0x028` | `Dependencies` (`_LDRP_CSLIST`) | **Flags / Pointer** | Tail of single-linked dependency list |
| `+0x038` | `State` (`_LDR_DDAG_STATE`) | **Integer** | `0` (`LdrModulesPlaceHolder`) |

---

## 6. Git Commit Record

- Document created: `KERNEL/LDR_DDAG_NODE_Research.md`
- Repository: `/Volumes/External/Code/winDows-Internals`
- Related tools: `structscan` v4.5 (`structscan_arm64.dll`)
