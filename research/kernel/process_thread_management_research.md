# Process / Thread Management — Windows 11 ARM64 Kernel Research

**Target**: Windows 11 Build 26100 (ARM64, Parallels VM)
**Primary Module**: `ntoskrnl.exe`
**Date**: 2026-06-04

---

## 1. Architecture Overview

Windows process/thread management spans:
- **Process lifecycle** — creation, termination, notification callbacks
- **Thread scheduling** — priority, quantum, affinity, ready queues
- **Job objects** — resource limits, scheduling classes, rate control
- **Priority system** — base priority, boosts, I/O priority, page priority
- **Affinity management** — CPU sets, group affinity, NUMA
- **Scheduler assists** — yield boosting, foreground boost, heterogeneous scheduling

---

## 2. Process Creation / Termination

### 2.1 Creation Pipeline

```
NtCreateUserProcess → PspCreateProcess
    → PspBuildCreateProcessContext
    → PspCaptureCreateInfo
    → PspValidateCreateProcessProtection
    → PspCreateUserContext
    → PspCreateUserProcessEcp
    → PspUpdateCreateInfo
    → PspCheckCpuPartitionCreateAccess
    → PspCreateObjectHandle
    → PspInvokeCreateCallback
```

| Function | Purpose |
|----------|---------|
| `PspCreateProcess` | Core process creation |
| `PspCreateThread` | Core thread creation |
| `PspCreatePicoProcess` | Create Pico process (WSL) |
| `PspCreatePicoThread` | Create Pico thread |
| `PspCreateSecureSystemProcess` | Create secure system process |
| `PspCreatePartitionSystemProcess` | Create partition system process |
| `PspCreateSecureThread` | Create secure thread |
| `PspCreateActivityReference` | Create activity reference |

### 2.2 Notification Callbacks

| Global | Purpose |
|--------|---------|
| `PspCreateProcessNotifyRoutine` | Array of process creation callbacks |
| `PspCreateThreadNotifyRoutine` | Array of thread creation callbacks |
| `PspCreateProcessNotifyRoutineCount` | Count of process notify routines |
| `PspCreateProcessNotifyRoutineExCount` | Count of extended process notify routines |
| `PspCreateThreadNotifyRoutineCount` | Count of thread notify routines |
| `PspCreateThreadNotifyRoutineNonSystemCount` | Non-system thread notify count |
| `PspSetCreateProcessNotifyRoutine` | Register/remove process callback |
| `PspSetCreateThreadNotifyRoutine` | Register/remove thread callback |

### 2.3 Termination

| Function | Purpose |
|----------|---------|
| `PsTerminateProcess` | Terminate a process |
| `PspTerminateProcess` | Internal process termination |
| `PspTerminateThreadByPointer` | Terminate thread by pointer |
| `PspTerminateAllThreads` | Terminate all threads in process |
| `PspInvokeTerminateCallback` | Invoke termination callbacks |
| `PsTerminateSystemThread` | Terminate system thread |
| `PspTerminatePicoProcess` | Terminate Pico process |
| `PspTerminateSiloSubsystemProcesses` | Terminate silo subsystem processes |
| `PspTerminateAllProcessesInJobHierarchy` | Terminate all in job hierarchy |

---

## 3. Priority System

### 3.1 Thread Priority APIs

| Function | Purpose |
|----------|---------|
| `KeSetPriorityThread` | Set thread base priority |
| `KeSetBasePriorityThread` | Set thread base priority |
| `KeSetActualBasePriorityThread` | Set actual base priority |
| `KeQueryPriorityThread` | Query thread priority |
| `KeQueryBasePriorityThread` | Query base priority |
| `KeQueryEffectivePriorityThread` | Query effective priority (with boosts) |
| `KeQueryEffectiveBasePriorityThread` | Query effective base priority |
| `KeBoostPriorityThread` | Apply priority boost |
| `KeSetPriorityBoost` | Enable/disable priority boosting |
| `KeAdjustPriorityFloor` | Adjust priority floor |
| `KeSetPriorityAndQuantumProcess` | Set process priority and quantum |
| `KeSetQuantumProcess` | Set process quantum |
| `KeSetDisableQuantumProcess` | Disable quantum for process |
| `KeSetDisableQuantumThread` | Disable quantum for thread |

### 3.2 Process Priority

| Function | Purpose |
|----------|---------|
| `PsSetProcessPriorityClass` | Set process priority class |
| `PsGetProcessPriorityClass` | Get process priority class |
| `PsSetProcessPriorityByClass` | Set priority by class enum |
| `PspSetProcessPriorityClass` | Internal set priority class |
| `PspSetProcessPriorityByClass` | Internal set by class |

### 3.3 I/O Priority

| Function | Purpose |
|----------|---------|
| `PsSetIoPriorityThread` | Set thread I/O priority |
| `PsBoostThreadIoQoS` | Boost thread I/O QoS |
| `PsBoostThreadOutstandingIoQoS` | Boost outstanding I/O QoS |
| `KeAbProcessBaseIoPriorityChange` | Process base I/O priority change |
| `KeAbProcessEffectiveIoPriorityChange` | Effective I/O priority change |

### 3.4 Page Priority

| Function | Purpose |
|----------|---------|
| `PsSetPagePriorityThread` | Set page priority |
| `PsSetSystemPagePriorityThread` | Set system page priority |

---

## 4. Quantum / Time Slice

| Global/Function | Purpose |
|-----------------|---------|
| `KeQuantumEndTimerIncrement` | Timer increment at quantum end |
| `KeSetQuantumProcess` | Set per-process quantum |
| `KeSetDisableQuantumProcess` | Disable quantum (realtime-like) |
| `KeSetDisableQuantumThread` | Disable quantum per-thread |

Windows scheduling quanta vary by priority class and foreground/background status. Realtime priority threads have quantums disabled.

---

## 5. Affinity Management

### 5.1 Thread Affinity

| Function | Purpose |
|----------|---------|
| `KeSetAffinityThread` | Set thread affinity |
| `KeSetAffinityProcess` | Set process affinity |
| `KeQueryAffinityProcess` | Query process affinity |
| `KeSetSystemAffinityThread` | Set system affinity |
| `KeSetSystemAffinityThreadEx` | Extended system affinity set |
| `KeSetSystemGroupAffinityThread` | Set group-aware affinity |
| `KeSetSystemMultipleGroupAffinityThread` | Multi-group affinity |
| `KeSetUserAffinityThread` | Set user affinity |
| `KeSetUserGroupAffinityThread` | Set user group affinity |
| `KeRevertToUserAffinityThread` | Revert to user affinity |
| `KeRevertToUserAffinityThreadEx` | Extended revert |
| `KeRevertToUserMultipleGroupAffinityThread` | Multi-group revert |
| `KeQueryUserAffinityThread` | Query user affinity |
| `KeQueryPrimaryGroupAffinityThread` | Query primary group affinity |
| `KeQueryPrimaryGroupAffinityProcess` | Query primary group affinity |

### 5.2 Process Affinity

| Function | Purpose |
|----------|---------|
| `PspSetProcessAffinityUpdateMode` | Set affinity update mode |
| `PspSetProcessAffinitySafe` | Safe affinity change |
| `PspUpdateSingleProcessAffinity` | Update single process affinity |
| `KeRecomputeCpuSetAffinityProcess` | Recompute CPU set affinity |
| `KiRemoveForceParkedProcessorsFromAffinity` | Remove parked cores from affinity |

### 5.3 Affinity Utility Functions

| Function | Purpose |
|----------|---------|
| `KeInitializeAffinityEx` | Initialize KAFFINITY_EX |
| `KeAddProcessorAffinityEx` | Add processor to affinity |
| `KeRemoveProcessorAffinityEx` | Remove processor |
| `KeAndAffinityEx` / `KeOrAffinityEx` / `KeXorAffinityEx` | Bitwise operations |
| `KeComplementAffinityEx` | Complement affinity |
| `KeIsEqualAffinityEx` | Compare affinities |
| `KeIsSubsetAffinityEx` | Check subset |
| `KeIsEmptyAffinityEx` | Check empty |
| `KeCountSetBitsAffinityEx` | Count set bits |
| `KeFindFirstSetLeftAffinityEx` | Find first set bit left |
| `KeFindFirstSetRightAffinityEx` | Find first set bit right |
| `KeCheckProcessorAffinityEx` | Check processor in affinity |
| `KeQueryActiveProcessorAffinity` | Query active processor affinity |
| `KeQueryNodeActiveAffinity` | Query NUMA node affinity |
| `KeSelectNodeForAffinity` | Select NUMA node for affinity |

---

## 6. Scheduler

### 6.1 Dispatcher / Ready Queue

| Function | Purpose |
|----------|---------|
| `KiExitDispatcher` | Exit dispatcher (reschedule) |
| `KiInsertQueue` | Insert into dispatcher queue |
| `KiInsertQueueInternal` | Internal queue insert |
| `KiQueueReadyThread` | Queue ready thread |
| `KiDeferredReadyThread` | Deferred ready thread |
| `KiDeferredReadySingleThread` | Deferred ready single |
| `KiFastReadyThread` | Fast ready thread |
| `KiPrepareReadyThreadForRescheduling` | Prepare thread for reschedule |
| `KiInswapAndReadyThread` | Inswap and ready |
| `KiReadyOutSwappedThreads` | Ready out-swapped threads |
| `KiScanSharedReadyThreads` | Scan shared ready queue |
| `KiUpdateSharedReadyQueueAffinityThread` | Update shared queue affinity |
| `KiInsertSchedulingGroupQueue` | Insert scheduling group |
| `KiInsertQueueApc` | Insert APC into queue |
| `KiInsertQueueDpc` | Insert DPC into queue |

### 6.2 Scheduler Assists (User-Mode Scheduling)

| Global | Purpose |
|--------|---------|
| `KiSchedulerAssistYieldBoostPeriod` | Yield boost period |
| `KiSchedulerAssistYieldCounterThreshold` | Yield counter threshold |
| `KiSchedulerForegroundBoostDecayPolicy` | Foreground boost decay |
| `KiSchedulerAssistThreadFlagEnabled` | Scheduler assist enabled flag |
| `KiSchedulerAssistThreadFlagOverride` | Override flag |
| `KiHeteroSchedulerOptions` | Heterogeneous scheduling options (big.LITTLE) |
| `KiHeteroSchedulerOptionsMask` | Hetero options mask |
| `KiMaxReadyThreadsPerInterruptMask` | Max ready threads per interrupt |

| Function | Purpose |
|----------|---------|
| `KiSchedulerApc` | Scheduler APC |
| `KiSetSchedulerAssistPriority` | Set scheduler assist priority |
| `KiReadGuestSchedulerAssistPriority` | Read guest priority (virtualization) |
| `KiAllocateSchedulerSubNode` | Allocate scheduler sub-node |
| `KiEnumerateNextSchedulerSubNodeInSystem` | Enumerate sub-nodes |

### 6.3 Scheduler Shared Data

| Function | Purpose |
|----------|---------|
| `PspSchedulerSharedDataRegionCreate` | Create shared data region |
| `PspSchedulerSharedDataRegionDelete` | Delete shared data region |
| `PspSchedulerSharedDataRegionSlotAllocate` | Allocate slot |
| `PspSchedulerSharedDataRegionSlotRetrieve` | Retrieve slot |
| `PspSchedulerSharedDataRegionSlotFree` | Free slot |

---

## 7. Job Objects

### 7.1 Job Management

| Function | Purpose |
|----------|---------|
| `PspAssignProcessToJob` | Assign process to job |
| `PspAssignProcessToJobList` | Assign to job list |
| `PspImplicitAssignProcessToJob` | Implicit assignment |
| `PspRemoveProcessFromJobChain` | Remove from chain |
| `PspUnlinkJobProcess` | Unlink process from job |
| `PspEstablishJobHierarchy` | Establish hierarchy |
| `PspInitializeJobStructures` | Initialize job structures |
| `PspJobDelete` | Delete job |
| `PspJobClose` | Close job handle |

### 7.2 Job Limits and Rate Control

| Function | Purpose |
|----------|---------|
| `PspSetEffectiveJobLimits` | Set effective limits |
| `PspValidateJobChainLimits` | Validate chain limits |
| `PspSetCpuRateControlJobPreCallback` | CPU rate control pre-callback |
| `PspSetCpuRateControlJobPostCallback` | CPU rate control post-callback |
| `PspSetJobRateControl` | Set rate control |
| `PspSetEffectiveRateControlJob` | Set effective rate control |
| `PspSetJobIoRateControl` | Set I/O rate control |
| `PspSetJobIoRateControlForVolume` | Per-volume I/O rate control |
| `PspJobIoRateControlDisable` | Disable I/O rate control |
| `PspJobIoRateVolumeEntryInsert` | Insert rate volume entry |
| `PspJobIoRateVolumeEntryRemove` | Remove rate volume entry |
| `PspJobIoRateQueryHistory` | Query rate history |

### 7.3 Job Scheduling

| Global | Purpose |
|--------|---------|
| `PspJobSchedulingClasses` | Job scheduling class table |
| `PspUseJobSchedulingClasses` | Flag: use scheduling classes |
| `PspSetProcessSchedulingGroup` | Set process scheduling group |
| `PspAddSchedulingGroupToJobChain` | Add scheduling group |

### 7.4 Job Notifications

| Function | Purpose |
|----------|---------|
| `PspSendJobNotification` | Send job notification |
| `PspRequestDeferredJobNotification` | Request deferred notification |
| `PspJobNotificationWorker` | Notification worker |
| `PspEvaluateAndNotifyEmptyJob` | Notify empty job |
| `PspChargeJobWakeCounter` | Charge wake counter |

### 7.5 Key Job Globals

| Global | Purpose |
|--------|---------|
| `PspJobList` | Global job list |
| `PspJobListLock` | Job list lock |
| `PspJobAssignmentLock` | Assignment lock |
| `PspJobNoWakeChargeLimit` | No-wake charge limit |
| `PspJobNotificationList` | Notification list |
| `PspJobTimeLimitsWorkItem` | Time limits work item |
| `PspJobTimeLimitsCount` | Time limits count |
| `PspJobTimeLimitsPeriodSeconds` | Time limits period |
| `PspUniqueJobIdTable` | Unique job ID table |

---

## 8. Key Findings for Optimizer

1. **Priority Boosting**: `KeBoostPriorityThread` and `KeSetPriorityBoost` control priority boosting. The `Feature_Servicing_InlinePreWakeupPriorityBoosting` feature flag enables inline pre-wakeup boosting for reduced latency.

2. **Foreground Boost Decay**: `KiSchedulerForegroundBoostDecayPolicy` controls how foreground priority boosts decay over time — directly affects app responsiveness.

3. **Quantum Control**: `KeSetQuantumProcess` / `KeSetDisableQuantumProcess` allow per-process quantum tuning. Longer quantums reduce context switches for CPU-bound workloads.

4. **Job Scheduling Classes**: `PspJobSchedulingClasses` and `PspUseJobSchedulingClasses` enable scheduling class differentiation within jobs — useful for QoS.

5. **Heterogeneous Scheduling**: `KiHeteroSchedulerOptions` / `KiHeteroSchedulerOptionsMask` control ARM big.LITTLE scheduling policy — critical for ARM64 optimization.

6. **CPU Set Affinity**: `KeRecomputeCpuSetAffinityProcess` enables soft CPU assignment via CPU Sets API, allowing fine-grained core allocation without hard affinity.

7. **Scheduler Assist Yield Boosting**: `KiSchedulerAssistYieldBoostPeriod` and `KiSchedulerAssistYieldCounterThreshold` control user-mode scheduling yield boost behavior.

8. **Shared Ready Queue**: `KiScanSharedReadyThreads` implements shared ready queue scanning — important for SMT/core scheduling efficiency.

9. **I/O QoS Boosting**: `PsBoostThreadIoQoS` allows boosting thread I/O priority for QoS scenarios (MMCSS, foreground apps).

10. **Process Creation Callbacks**: `PspCreateProcessNotifyRoutine` array allows monitoring all process creation/exit — useful for optimizer process tracking.

11. **Force Parked Processor Removal**: `KiRemoveForceParkedProcessorsFromAffinity` removes parked cores from affinity masks — ensures processes don't stall waiting for parked cores.

12. **Pico Processes (WSL)**: `PspCreatePicoProcess` / `PspCreatePicoThread` handle WSL process/thread creation — these have special scheduling and affinity rules.

---

## 9. User-Mode APIs

| API | Purpose |
|-----|---------|
| `SetPriorityClass` | Set process priority class |
| `SetThreadPriority` | Set thread priority |
| `SetThreadPriorityBoost` | Enable/disable priority boosting |
| `SetProcessAffinityMask` | Set process CPU affinity |
| `SetThreadAffinityMask` | Set thread CPU affinity |
| `SetProcessInformation` | Set process info (CPU sets, scheduling class) |
| `SetThreadInformation` | Set thread info (ideal processor, priority) |
| `CreateJobObject` | Create job object |
| `SetInformationJobObject` | Set job limits/rate control |
| `AssignProcessToJobObject` | Assign process to job |
| `NtCreateUserProcess` | Native process creation |
| `NtCreateThreadEx` | Native thread creation |
| `RtlCreateUserProcess` | Runtime layer process creation |
| `SetProcessWorkingSetSizeEx` | Set working set limits |

---

## 10. Registry Keys

```
HKLM\SYSTEM\CurrentControlSet\Control\PriorityControl
  - Win32PrioritySeparation             (REG_DWORD: quantum/priority config)
    Bits 0-2: Short(2)/Long(6) vs Variable quantums
    Bits 3-5: Foreground boost (0=none, 1=+1, 2=+2)
    Bit 6:    Foreground and background equal priority

HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\kernel
  - GlobalTimerResolutionRequests       (REG_DWORD)
  - DisableExceptionChainValidation     (REG_DWORD)
```
