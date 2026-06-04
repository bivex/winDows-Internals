# GPU Scheduler & Dxgkrnl API Research (Windows 11 ARM64 Build 26100)

> Kernel-debugged via WinDbg MCP on ARM64 target (Parallels VM)
> Modules: `dxgkrnl` (fffff800`68e20000 - fffff800`692dd000), `dxgmms2` (fffff800`6d600000 - fffff800`6d70c000), `cdd` (fffff800`6d740000), `monitor` (fffff800`6d710000), `BasicRender`, `BasicDisplay`

---

## 1. GPU Scheduling Priority System

### 1.1 Priority Classes (D3DKMT_SCHEDULINGPRIORITYCLASS)

The GPU scheduler uses a multi-tier priority system with per-process and per-context granularity:

| API | Kernel Path | Description |
|---|---|---|
| `D3DKMTSetProcessSchedulingPriorityClass` | `DxgkSetProcessSchedulingPriorityClass` -> `DXGPROCESS::SetProcessSchedulingPriorityClass` | Set process-wide GPU priority |
| `D3DKMTGetProcessSchedulingPriorityClass` | `DxgkGetProcessSchedulingPriorityClass` -> `DXGPROCESS::GetProcessSchedulingPriorityClass` (read EPROCESS field) | Query process GPU priority |
| `D3DKMTSetContextSchedulingPriority` | `DxgkSetContextSchedulingPriority` -> `DXGCONTEXT::SetSchedulingPriority` | Set per-context priority |
| `D3DKMTGetContextSchedulingPriority` | `DxgkGetContextSchedulingPriority` -> `DXGCONTEXT::SetInProcessSchedulingPriority` | Query per-context priority |
| `D3DKMTSetContextInProcessSchedulingPriority` | `DxgkSetContextInProcessSchedulingPriority` | Relative priority within process |
| `D3DKMTGetContextInProcessSchedulingPriority` | `DxgkGetContextInProcessSchedulingPriority` | Query relative context priority |

### 1.2 Priority Bands (D3DKMT_SCHEDULING_PRIORITYBAND)

WDDM 2.4+ introduced priority bands for finer-grained control:

| API | Kernel Path | Description |
|---|---|---|
| `D3DKMTSetProcessSchedulingPriorityBand` | `DxgkSetProcessSchedulingPriorityBand` -> `DXGPROCESS::SetProcessSchedulingPriorityBand` | Set process to a priority band |
| `D3DKMTGetProcessSchedulingPriorityBand` | `DxgkGetProcessSchedulingPriorityBand` -> `DXGPROCESS::GetProcessSchedulingPriorityBand` | Query current priority band |

### 1.3 Priority Tables (dxgmms2 Globals)

Critical global variables controlling GPU scheduling behavior:

```
dxgmms2!gulPriorityMapping                    - Maps user priority to internal priority
dxgmms2!gulPublicPriorityToSchedulingPriority - Public API priority to scheduler internal
dxgmms2!gulPriorityToPriorityClass            - Priority value to class mapping
dxgmms2!gulPriorityToYieldPriorityBand        - Priority to yield behavior band
dxgmms2!gulQuantumMultiplierTableByPriorityClass        - Time quantum multiplier per class
dxgmms2!gulPreemptionQuantumMultiplierTableByPriorityClass - Preemption quantum per class
```

### 1.4 Internal Scheduling Functions (dxgmms2)

```
VidSchiComputePriority                    - Core priority computation
VidSchiUpdatePriorityTables               - Update all priority data structures
VidSchiSelectContextFromThisPriority       - Select next context from priority level
VidSchiRun_PriorityTable                  - Run the priority-based scheduler
VidSchiUpdateReadyBitsInNewPriority       - Update ready bitmask for priority change
VidSchiStartExecutionTimeAtThisPriority   - Begin execution time accounting
VidSchiStopExecutionTimeAtThisPriority    - End execution time accounting
VidSchiStartExecutionTimeAtThisPriorityBand - Per-band execution time
VidSchiAdjustWorkerThreadPriority         - Adjust scheduler worker thread OS priority
VidSchiAdjustWorkerThreadPriorityDirectSubmitAware - Same, for direct-submit mode
VidSchiComputeWorkerThreadPriority        - Compute target worker thread priority
VidSchiCheckProcessGPUPriorityPrivilege   - Verify process has GPU priority privilege
VidSchiUpdateDdiHwContextPriority         - Notify driver of HW context priority change
VidSchiNotifyContextPriorityChange        - Propagate priority change notification
```

### 1.5 Context/HW Queue Priority Management

```
VidSchSetPriorityContext                  - Set context priority
VidSchGetPriorityContext                  - Get context priority
VidSchSetInProcessPriorityContext         - Set in-process relative priority
VidSchGetInProcessPriorityContext         - Get in-process relative priority
VidSchSetAbsolutePriorityContext          - Set absolute priority
VidSchSetPriorityHwContext                - Set HW queue priority
VidSchSetInProcessPriorityHwContext       - Set in-process HW queue priority
VidSchSetAbsolutePriorityHwContext        - Set absolute HW queue priority
VidSchSyncPriorityDevice                  - Synchronize device priority state
VidSchSetPriorityClassProcess             - Set process priority class (scheduler level)
VidSchGetPriorityClassProcess             - Get process priority class
```

---

## 2. GPU Memory Management (VIDMM)

### 2.1 Video Memory Reservation & Budget

| API | Kernel Path | Description |
|---|---|---|
| `D3DKMTChangeVideoMemoryReservation` | `DxgkChangeVideoMemoryReservation` -> `DxgkChangeVideoMemoryReservationInternal` | Reserve/release VRAM for process |
| `D3DKMTQueryVideoMemoryInfo` | `DxgkQueryVideoMemoryInfo` | Query current VRAM usage/budget |
| `D3DKMTSetMemoryBudgetTarget` | `DxgkSetMemoryBudgetTarget` | Set target VRAM budget |
| `D3DKMTGetMemoryBudgetTarget` | `DxgkGetMemoryBudgetTarget` | Query current budget target |

### 2.2 Allocation Priority

| API | Kernel Path | Description |
|---|---|---|
| `D3DKMTSetAllocationPriority` | `DxgkSetAllocationPriority` -> `DXGDEVICE::SetAllocationPriority` | Set allocation priority |
| `D3DKMTGetAllocationPriority` | `DxgkGetAllocationPriority` -> `DXGDEVICE::GetAllocationPriority` | Query allocation priority |

Internal priority class mapping:
```
GetAllocationPriorityClassFromPriority - Maps ULONG priority to VIDMM_ALLOCATION_PRIORITY_CLASS
VIDMM_GLOBAL::SetAllocationPriority   - Set allocation priority in VidMm
VIDMM_GLOBAL::UpdateAllocationPriority - Update allocation priority dynamically
VIDMM_GLOBAL::GetAllocationPriority   - Query current allocation priority
VidMmSetAllocationPriority            - Public VidMm priority setter
VidMmGetAllocationPriority            - Public VidMm priority getter
VidMmiSetPriorityForMemoryPages       - Set memory page priority (standby list integration)
```

### 2.3 VidMm Core Functions

```
VIDMM_EXPORT::VidMmPinAllocation         - Pin allocation in VRAM (prevent eviction)
VIDMM_EXPORT::VidMmUnpinAllocation       - Unpin allocation
VIDMM_EXPORT::VidMmEvictAllocation       - Evict allocation from VRAM to system memory
VIDMM_EXPORT::VidMmGetTotalSegmentSize   - Get total VRAM segment sizes
VIDMM_EXPORT::VidMmQueryAllocationResidency - Check if allocation is resident
VIDMM_EXPORT::VidMmMapGpuVirtualAddress  - Map GPU VA
VIDMM_EXPORT::VidMmFreeGpuVirtualAddress - Free GPU VA
VIDMM_EXPORT::VidMmReserveGpuVirtualAddress - Reserve GPU VA range
VIDMM_EXPORT::VidMmMapCpuVA              - Map allocation for CPU access
VIDMM_EXPORT::VidMmUnmapCpuVA            - Unmap CPU VA
VIDMM_EXPORT::VidMmInvalidateAllocation  - Invalidate allocation (flush)
VIDMM_EXPORT::VidMmInvalidateCache       - Invalidate GPU cache
VIDMM_EXPORT::VidMmQuerySegmentStatistics - Query segment usage statistics
```

---

## 3. GPU Scheduler Core (VIDSCH)

### 3.1 Scheduler Operations

```
VIDSCH_EXPORT::VidSchSubmitCommand           - Submit DMA command to GPU
VIDSCH_EXPORT::VidSchSubmitCommandToHwQueue   - Submit to HW queue (WDDM 2.4+)
VIDSCH_EXPORT::VidSchFlushContext             - Flush pending commands for context
VIDSCH_EXPORT::VidSchFlushPendingCommand      - Flush single pending command
VIDSCH_EXPORT::VidSchFlushQueuePackets        - Flush queued packets by type
VIDSCH_EXPORT::VidSchSwitchFromContext        - Context switch away from context
VIDSCH_EXPORT::VidSchCreateContext            - Create scheduling context
VIDSCH_EXPORT::VidSchCreateHwQueue            - Create HW queue
VIDSCH_EXPORT::VidSchDdiNotifyDpc             - DPC notification from driver
VIDSCH_EXPORT::VidSchEnableLatencyToleranceTimer - Enable/disable latency timer
```

### 3.2 Sync Object Management

```
VIDSCH_EXPORT::VidSchWaitForSingleSyncObject    - Wait on GPU sync object
VIDSCH_EXPORT::VidSchSignalSyncObjectsFromCpu   - Signal sync from CPU
VIDSCH_EXPORT::VidSchSubmitWaitToHwQueue         - Submit wait to HW queue
VIDSCH_EXPORT::VidSchSetHwQueueProgressFenceObject - Set progress fence
```

### 3.3 VSync Control

```
VIDSCH_EXPORT::VidSchControlVSyncAdapter   - Enable/disable VSync per adapter
VIDSCH_EXPORT::VidSchControlVSyncDevice     - Enable/disable VSync per device
VIDSCH_EXPORT::VidSchIsVSyncEnabled         - Query VSync state
```

---

## 4. TDR (Timeout Detection and Recovery)

### 4.1 TDR Configuration Globals

```
dxgkrnl!g_TdrConfig                  - TDR configuration structure
dxgkrnl!g_TdrDebugMode               - Debug mode flag
dxgkrnl!g_TdrForceTimeout             - Force timeout flag
dxgkrnl!g_TdrForceDodPresentTimeout   - Force display-only present timeout
dxgkrnl!g_TdrForceDodVSyncTimeout     - Force display-only VSync timeout
dxgkrnl!g_TdrRecoveryInProgress       - Recovery in progress flag
dxgkrnl!g_TdrRecoveryToDebug          - Recovery to debug flag
dxgkrnl!g_TdrTimedOpToDebug           - Timed operation to debug flag
dxgkrnl!g_TdrHistory                  - TDR history ring buffer
```

### 4.2 TDR Flow

```
TdrInit                              - Initialize TDR subsystem
TdrIsEnabled                         - Check if TDR is active
TdrCreateRecoveryContext              - Create recovery context for timeout
TdrIsRecoveryRequired                - Check if recovery is needed
TdrResetFromTimeout                  - Reset GPU from timeout state
TdrResetFromTimeoutAsync             - Async reset (work item)
TdrResetFromTimeoutWorkItem          - Work item callback
TdrCompleteRecoveryContext           - Complete recovery
TdrCollectDbgInfoStage1              - Collect debug info (stage 1)
TdrCollectDbgInfoStage2              - Collect debug info (stage 2)
TdrHistoryUpdate                     - Update TDR history
TdrHistoryIsLimitExhausted           - Check if TDR limit is reached
TdrValidateDebugMode                 - Validate debug mode
TdrBugcheckOnTimeout                 - Bugcheck on timeout (if enabled)
TdrTimedOperationStart               - Start timed operation
TdrTimedOperationDelay               - Delay in timed operation
TdrTimedOperationWaitForSingleObject - Wait in timed operation
TdrAllowToDebugEngineTimeout         - Allow debugging on engine timeout
TdrAllowToDebugTimeout               - Allow debugging on timeout
```

### 4.3 Scheduler Recovery

```
ADAPTER_RENDER::ResetSchedulerFromTDR    - Reset scheduler after TDR
ADAPTER_RENDER::RestartSchedulerFromTDR  - Restart scheduler after TDR
ADAPTER_RENDER::SuspendScheduler         - Suspend scheduler
ADAPTER_RENDER::ResumeScheduler          - Resume scheduler
ADAPTER_RENDER::FlushScheduler           - Flush all scheduler queues
```

---

## 5. DXGPROCESS Structure (EPROCESS Integration)

### 5.1 Process-Object Link

```
EPROCESS -> PsSetProcessDxgProcess  -> DXGPROCESS  (kernel callback)
EPROCESS -> PsGetProcessDxgProcess  -> DXGPROCESS*  (query back-pointer)
```

Key DXGPROCESS methods:
```
DXGPROCESS::GetCurrent              - Get current process DXGPROCESS
DXGPROCESS::GetProcessName          - Get process name string
DXGPROCESS::GetProcessID            - Get process ID
DXGPROCESS::GetHostProcess          - Get host process (for VM)
DXGPROCESS::GetVidSchProcess        - Get VIDSCH_PROCESS for adapter
DXGPROCESS::GetVidMmProcess         - Get VIDMM_PROCESS for adapter
DXGPROCESS::GetGpuPreferenceDListState - Get GPU preference
DXGPROCESS::IsHighPriorityProcess   - Check if high GPU priority
DXGPROCESS::SendWnfNotification     - Send WNF notification
DXGPROCESS::NotifyProcessFreeze     - Handle process freeze
DXGPROCESS::NotifyProcessThaw       - Handle process thaw
DXGPROCESS::EvictAllResources       - Evict all GPU resources
DXGPROCESS::FlushAllDevice          - Flush all devices for adapter
```

---

## 6. HW Queue Infrastructure (WDDM 2.4+)

### 6.1 Creation & Destruction

```
DxgkCreateHwQueue         -> DXGCONTEXT::CreateHwQueue  -> DXGHWQUEUE::Initialize
DxgkDestroyHwQueue         -> DXGCONTEXT::DestroyHwQueue -> DXGHWQUEUE::DestroyCoreState
```

### 6.2 Doorbell Mechanism

```
DXGHWQUEUE::CreateDoorbell  - Create user-mode doorbell for submission
DXGHWQUEUE::DestroyDoorbell - Destroy doorbell
DXGHWQUEUE::InitializeForUserModeSubmission - Setup user-mode submission path
DXGHWQUEUE::InitializeOnHost - Setup host-side HW queue
```

### 6.3 Submission Path

```
DxgkSubmitCommandToHwQueue -> DXGHWQUEUE::SubmitCommand -> VIDSCH_EXPORT::VidSchSubmitCommandToHwQueue
DxgkSubmitPresentToHwQueue - Present via HW queue
```

---

## 7. Virtual GPU (Parallels/VMBus)

The target VM uses Parallels virtual GPU with VMBus communication:

```
DXG_GUEST_VIRTUALGPU_VMBUS::VmBusSendSetContextSchedulingPriority   - Guest -> Host priority
DXG_GUEST_VIRTUALGPU_VMBUS::VmBusSendGetContextSchedulingPriority   - Guest -> Host query
DXG_GUEST_VIRTUALGPU_VMBUS::VmBusSendChangeVideoMemoryReservation   - Guest VRAM reservation
DXG_GUEST_VIRTUALGPU_VMBUS::VmBusSendQueryVideoMemoryInfo           - Guest VRAM query
DXG_GUEST_VIRTUALGPU_VMBUS::VmBusSendCreateHwQueue                 - Guest HW queue create
DXG_HOST_VIRTUALGPU_VMBUS::VmBusSetContextSchedulingPriority        - Host-side priority set
DXG_HOST_VIRTUALGPU_VMBUS::VmBusGetContextSchedulingPriority        - Host-side priority get
```

---

## 8. Optimization APIs Summary

### User-Mode APIs for GPU Optimization

| API | Header | Purpose |
|---|---|---|
| `D3DKMTSetProcessSchedulingPriorityClass` | d3dkmthk.h | Set process GPU priority (Idle/BelowNormal/Normal/AboveNormal/High/Realtime) |
| `D3DKMTSetContextSchedulingPriority` | d3dkmthk.h | Set context GPU priority (-7 to +7 range) |
| `D3DKMTSetContextInProcessSchedulingPriority` | d3dkmthk.h | Relative context priority |
| `D3DKMTSetProcessSchedulingPriorityBand` | d3dkmthk.h | WDDM 2.4+ priority band |
| `D3DKMTSetAllocationPriority` | d3dkmthk.h | Set allocation residency priority |
| `D3DKMTChangeVideoMemoryReservation` | d3dkmthk.h | Reserve VRAM budget |
| `D3DKMTSetMemoryBudgetTarget` | d3dkmthk.h | Set target VRAM budget |
| `D3DKMTQueryVideoMemoryInfo` | d3dkmthk.h | Query VRAM usage |
| `D3DKMTQueryStatistics` | d3dkmthk.h | Query GPU statistics per process/adapter |

### Kernel Objects for Optimization

| Object | Module | Use |
|---|---|---|
| `DXGPROCESS` | dxgkrnl | Per-process GPU state, priority class |
| `DXGCONTEXT` | dxgkrnl | Per-context priority, scheduling state |
| `DXGDEVICE` | dxgkrnl | Per-device priority class, allocation management |
| `DXGHWQUEUE` | dxgkrnl | HW queue with doorbell submission |
| `VIDSCH_GLOBAL` | dxgmms2 | Scheduler global state per adapter |
| `VIDSCH_NODE` | dxgmms2 | Per-engine node scheduling |
| `VIDSCH_CONTEXT` | dxgmms2 | Scheduled context state |
| `VIDMM_GLOBAL` | dxgmms2 | Video memory manager global state |
| `VIDMM_PROCESS` | dxgmms2 | Per-process VidMm state |
| `VIDMM_ALLOC` | dxgmms2 | Allocation tracking |

---

## 9. Key Findings for Optimizer

1. **GPU Priority is fully controllable** via `D3DKMTSetProcessSchedulingPriorityClass` (6 levels) and `D3DKMTSetContextSchedulingPriority` (per-context)
2. **Priority Bands** (WDDM 2.4+) offer even finer control with `D3DKMTSetProcessSchedulingPriorityBand`
3. **VRAM Budget** can be queried and influenced via `D3DKMTQueryVideoMemoryInfo` and `D3DKMTSetMemoryBudgetTarget`
4. **Allocation Priority** controls residency - higher priority allocations stay in VRAM longer
5. **TDR** can be configured/monitored via globals - optimizer could adjust timeout thresholds
6. **The priority quantum tables** (`gulQuantumMultiplierTableByPriorityClass`, `gulPreemptionQuantumMultiplierTableByPriorityClass`) are writable globals that control how long each priority level gets GPU time
7. **HW Queues** with doorbells enable ultra-low-latency GPU submission (bypass kernel transition for command submission)
8. **DXGPROCESS** is stored in EPROCESS via `PsSetProcessDxgProcess` - can be cross-referenced with CPU scheduler priority
