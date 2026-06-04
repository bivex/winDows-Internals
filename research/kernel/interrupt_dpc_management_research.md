# Interrupt/DPC Management Research (Windows 11 ARM64 Build 26100)

> Kernel-debugged via WinDbg MCP on ARM64 target (Parallels VM)
> Primary module: `nt` (kernel). ARM64 GICv3 interrupt controller.

---

## 1. Interrupt Architecture (ARM64 GICv3)

### 1.1 HAL Interrupt Layer

On ARM64, the HAL wraps the GICv3 (Generic Interrupt Controller v3):

```
HalpGicRequestInterrupt              - Request interrupt on GIC
HalpGic3RequestInterrupt             - GICv3-specific request
HalpGicWriteEndOfInterrupt           - Write EOI (End of Interrupt)
HalpGic3WriteEndOfInterrupt          - GICv3 EOI
HalpGic3DeactivateInterrupt          - GICv3 deactivate (priority drop)
HalpGicDeactivateInterrupt           - Generic deactivate
```

**GIC Controller Globals:**

| Global | Description |
|---|---|
| `nt!HalpInterruptGicVersion` | GIC version (3 for this target) |
| `nt!HalpInterruptController` | Pointer to active interrupt controller |
| `nt!HalpInterruptControllerCount` | Number of interrupt controllers |
| `nt!HalpRegisteredInterruptControllers` | Registered interrupt controllers |
| `nt!HalpInterruptMsiSupported` | MSI support flag |
| `nt!HalpInterruptHasPriorities` | Interrupt priority support |
| `nt!HalpInterruptMaxProcessors` | Maximum processors for interrupts |

### 1.2 Interrupt Registration & Connection

```
IoConnectInterrupt                    - Driver API: connect ISR to interrupt
IoConnectInterruptEx                  - Extended connect with more options
IoDisconnectInterrupt                 - Disconnect ISR
IoDisconnectInterruptEx               - Extended disconnect
IopConnectInterrupt                   - Internal connect
IopConnectLineBasedInterrupt          - Connect line-based (pin) interrupt
IopConnectMessageBasedInterrupt       - Connect MSI/MSI-X interrupt
IopConnectInterruptFullySpecified     - Connect with fully specified resources
```

**Kernel-level:**

```
KeConnectInterrupt                    - Connect KINTERRUPT to vector
KeDisconnectInterrupt                 - Disconnect KINTERRUPT
KeInitializeInterrupt                 - Initialize KINTERRUPT structure
KeInitializeInterruptEx               - Extended initialization
KeAllocateInterrupt                   - Allocate interrupt object
KeFreeInterrupt                       - Free interrupt object
KeMaskInterrupt                       - Mask (disable) interrupt
KeUnmaskInterrupt                     - Unmask (enable) interrupt
```

### 1.3 Interrupt Dispatch

```
KiInterruptDispatchCommon             - Common interrupt dispatch entry
KiInvokeInterruptServiceRoutine       - Call the ISR
KiCallInterruptServiceRoutine         - Low-level ISR invocation
KiDispatchInterrupt                   - Main dispatch interrupt handler
KiPlayInterrupt                       - Play (replay) pending interrupt
KiReplayInterrupt                     - Replay specific interrupt
KiInsertInterruptObjectOrdered        - Insert into interrupt list by priority
```

**ARM64 Exception Handlers (spectre-mitigated variants):**

```
KiKernelInterruptHandler              - Kernel-mode interrupt
KiUserInterruptHandler                - User-mode interrupt (syscall return)
KiKernelSp0InterruptHandler           - SP0 stack interrupt
KiUser32InterruptHandler              - WoW64 user-mode interrupt
```

### 1.4 Software Interrupts

Software interrupts are used for DPC/APC dispatch and IPI (Inter-Processor Interrupts):

```
HalRequestSoftwareInterrupt           - Request software interrupt on CPU
HalSendSoftwareInterrupt              - Send software interrupt
HalpDispatchSoftwareInterrupt         - Dispatch pending software interrupt
HalpInterruptCheckForSoftwareInterrupt - Check for pending SW interrupts
KiSendSoftwareInterrupt               - Kernel SW interrupt send
KiInitializeSoftwareInterruptBatch    - Batch initialization
KiAddProcessorToSoftwareInterruptBatch - Add CPU to SW interrupt batch
KiFlushSoftwareInterruptBatch         - Flush batched SW interrupts
```

### 1.5 Secondary Interrupts (Message-Signaled)

Secondary interrupts support virtual/message-signaled interrupts:

```
KeInitializeSecondaryInterruptServices - Init secondary interrupt framework
KeDispatchSecondaryInterrupt           - Dispatch secondary interrupt
HalpEnableSecondaryInterrupt           - Enable secondary interrupt
HalpDisableSecondaryInterrupt          - Disable secondary interrupt
HalpInterruptRequestSecondaryInterrupt - Request secondary interrupt
HalpAllocateGsivForSecondaryInterrupt  - Allocate GSIV for secondary
KiConnectSecondaryInterrupt            - Connect secondary interrupt
KiDisconnectSecondaryInterrupt         - Disconnect secondary interrupt
```

| Global | Value | Description |
|---|---|---|
| `nt!KiSecondaryInterruptServicesEnabled` | 0 | Secondary interrupt services NOT enabled |

---

## 2. DPC (Deferred Procedure Call) System

### 2.1 DPC Core Functions

```
KeInitializeDpc                       - Initialize DPC object
KeInsertQueueDpc                      - Queue DPC for execution
KeRemoveQueueDpc                      - Remove DPC from queue
KeRemoveQueueDpcEx                    - Extended remove
KeSetImportanceDpc                    - Set DPC priority (low/medium/high)
KeSetTargetProcessorDpc               - Target DPC to specific CPU
KeSetTargetProcessorDpcEx             - Extended targeting
KeFlushQueuedDpcs                     - Flush all pending DPCs
KeIsExecutingDpc                      - Check if currently executing DPC
KeGenericCallDpc                      - Broadcast DPC to all CPUs
KeGenericCallDpcEx                    - Extended broadcast DPC
KeSignalCallDpcSynchronize            - Synchronize broadcast DPC
KeSignalCallDpcDone                   - Signal broadcast DPC completion
```

### 2.2 DPC Execution Engine

```
KiRetireDpcList                       - Main DPC list retirement loop
KiExecuteAllDpcs                      - Execute all pending DPCs
KiExecuteDpc                          - Execute single DPC
KiInsertQueueDpc                      - Internal queue insertion
KiSetDpcRequestFlag                   - Set DPC pending flag in PRCB
KiTryToEndDpcProcessing               - Attempt to end DPC processing
KiEnterLongDpcProcessing              - Enter long DPC processing mode
KiExitLongDpcProcessing               - Exit long DPC processing mode
KiInsertNewDpcRuntime                  - Record DPC runtime
```

### 2.3 Threaded DPC

Threaded DPCs execute at PASSIVE_LEVEL in a dedicated thread instead of DISPATCH_LEVEL:

```
KeInitializeThreadedDpc               - Initialize threaded DPC object
KeIsThreadedDpcThread                 - Check if current thread is DPC thread
KiAllocateDpcDelegateThread           - Allocate delegate thread for DPCs
KiStartDpcThread                      - Start DPC worker thread
KiExecuteDpcDelegate                  - Execute DPC via delegate thread
KiSelectDpcData                       - Select DPC data for execution
```

| Global | Value | Description |
|---|---|---|
| `nt!KeThreadDpcEnable` | 1 | Threaded DPC is enabled |
| `nt!KiDpcDelegateThreadName` | (ptr) | Name string for DPC delegate thread |

### 2.4 DPC Watchdog

The DPC watchdog monitors for DPCs that take too long, causing system stalls:

```
KiDpcWatchdog                         - Core watchdog timer handler
KiDpcWatchdogCaptureStack             - Capture stack of long-running DPC
KiDpcWatchdogCounterReset             - Reset watchdog counter
KiLogSingleDpcSoftTimeoutEvent        - Log single DPC timeout event
KiInitDpcThresholds                   - Initialize DPC thresholds
KiValidateDpcWatchdogConfiguration    - Validate watchdog config
KiResetGlobalDpcWatchdogProfiler      - Reset global watchdog profiler
KiForceBugcheckForDpcWatchdog         - Force bugcheck on DPC timeout
```

**DPC Watchdog Globals:**

| Global | Value | Description |
|---|---|---|
| `nt!KeDpcTimeoutMs` | 0 | Hard DPC timeout (0 = disabled) |
| `nt!KeDpcSoftTimeoutMs` | 0 | Soft DPC timeout for logging (0 = disabled) |
| `nt!KeDpcCumulativeSoftTimeoutMs` | 0 | Cumulative soft timeout (0 = disabled) |
| `nt!KeDpcWatchdogPeriodMs` | 0 | Watchdog check period (0 = disabled) |
| `nt!KiForceBugcheckForDpcWatchdog` | (flag) | Bugcheck on timeout flag |
| `nt!KeDpcWatchdogProfileSingleDpcThresholdMs` | (ptr) | Single DPC profile threshold |
| `nt!KeDpcWatchdogProfileCumulativeDpcThresholdMs` | (ptr) | Cumulative DPC profile threshold |

**Long DPC Detection:**

| Global | Value | Description |
|---|---|---|
| `nt!KiLongDpcQueueThreshold` | 3 | Queue depth threshold for long DPC |
| `nt!KiLongDpcRuntimeThreshold` | 100 | Runtime threshold (units) for long DPC |
| `nt!KiLongDpcRuntimeThresholdCycles` | (ptr) | Runtime threshold in CPU cycles |

**DPC Rate Control:**

| Global | Value | Description |
|---|---|---|
| `nt!KiMaximumDpcQueueDepth` | 4 | Max DPCs before requesting DPC interrupt |
| `nt!KiMinimumDpcRate` | 3 | Minimum DPC dispatch rate |
| `nt!KiIdealDpcRate` | 20 | Ideal DPC dispatch rate |
| `nt!KiAdjustDpcThreshold` | 20 | DPC adjustment threshold |

**ETW Events for DPC Monitoring:**

```
CPU_STARVATION_EVENT_SINGLE_DPC_SOFT_TIMEOUT       - Single DPC exceeded soft timeout
CPU_STARVATION_EVENT_CUMULATIVE_DPC_SOFT_TIMEOUT   - Cumulative DPC time exceeded
CPU_STARVATION_EVENT_DPC_PROFILING_STACK           - DPC profiling stack capture
CPU_STARVATION_EVENT_DPC_PROFILING_STACK_BEGIN     - DPC profiling start
CPU_PARTITION_EVENT_DPC_SCHEDULING_VIOLATION       - CPU partition DPC violation
CPU_PARTITION_EVENT_GENERIC_DPC_VIOLATION          - Generic DPC violation
```

### 2.5 DPC Runtime History

```
KiDpcRuntimeHistoryHashTableAllocate         - Allocate runtime history table
KiDpcRuntimeHistoryHashTableDeallocate        - Deallocate runtime history table
KiDpcRuntimeHistoryHashTableGrowIfNeeded      - Grow table if needed
KiInitializeDpcRuntimeHistoryHashTables       - Initialize all hash tables
KiInitializeSingleDpcRuntimeHistoryHashTable  - Initialize single table
KiQueryDpcRuntimeHistory                       - Query DPC runtime history
KiDpcRuntimeHistoryHashTableCleanupDpcRoutine - Cleanup routine
KiDpcRuntimeHistoryHashTableCleanupTimerCallback - Timer cleanup callback
```

### 2.6 DPC Gang (Multi-CPU DPC)

DPC Gangs execute DPCs simultaneously on multiple CPUs:

```
MiInitializeDpcGang                   - Initialize DPC gang
MiStartDpcGang                        - Start DPC gang execution
MiDpcGangTarget                       - Target CPU in DPC gang
MiInsertDpcGang                       - Insert into DPC gang
MiComputeIdealDpcGang                 - Compute ideal gang configuration
MiInitializeDpcGroupAffinity          - Initialize group affinity for gang
```

### 2.7 Passive-Level Interrupt

Passive-level interrupt handling for devices that require IRQL = PASSIVE_LEVEL:

```
IopInitializePassiveInterruptServices  - Initialize passive interrupt framework
IopAllocatePassiveInterruptBlock       - Allocate passive interrupt block
IopInsertPassiveInterruptBlock         - Insert into passive list
IopFindPassiveInterruptBlock           - Find passive interrupt block
IopFindPassiveInterruptBlockLocked     - Find under lock
IopDestroyPassiveInterruptBlock        - Destroy block
IopDereferencePassiveInterruptBlock    - Dereference block
IoProcessPassiveInterrupts             - Process passive interrupts
IopPassiveInterruptDpc                 - DPC for passive interrupt
IopPassiveInterruptWorker              - Worker thread for passive interrupts
IopPassiveInterruptRealtimeWorker      - Real-time priority worker
IopCreatePassiveInterruptRealtimeThreads - Create RT worker threads
IopQueryPassiveInterruptRegistryOptions - Query registry settings
```

| Global | Description |
|---|---|
| `nt!PassiveInterruptList` | Global passive interrupt list |
| `nt!PassiveInterruptListLock` | Lock for passive interrupt list |
| `nt!PassiveInterruptRealtimeWorkQueue` | Real-time work queue |
| `nt!PassiveInterruptRealtimeWorkerPriority` | Worker thread priority |
| `nt!PassiveInterruptRealtimeWorkerCount` | Number of RT workers |

---

## 3. Interrupt Steering (IntSteer)

### 3.1 Overview

Interrupt steering distributes device interrupts across CPUs to balance load and avoid bottlenecks. On this ARM64 system, it's currently **disabled**.

### 3.2 Steering Functions

```
KiIntSteerInit                        - Initialize interrupt steering
KiIntSteerEnable                      - Enable steering
KiIntSteerDisable                     - Disable steering
KiIntSteerConnect                     - Hook into interrupt connection
KiIntSteerDistributeInterrupts        - Distribute interrupts across CPUs
KiIntSteerSetDestination              - Set steering destination CPU
KiIntSteerVerifyDestination           - Verify destination is valid
KiIntSteerGetNextProcessorTarget      - Get next CPU target (round-robin)
KiIntSteerChooseInitialTargetProcessors - Choose initial targets
KiIntSteerCalculateUniformDistribution - Uniform distribution algorithm
KiIntSteerCalculateFallbackDistribution - Fallback distribution
KiIntSteerComputeCpuSet               - Compute CPU set for steering
KiIntSteerClearCpuSetAssignment       - Clear CPU set assignment
KiIntSteerComputeRelevanceForTriageDumps - Compute relevance for triage
KiIntSteerAddLoadToProcessorAndCheckThreshold - Add load and check threshold
KiIntSteerDetermineSteeringEnabled    - Determine if steering is enabled
```

**Public APIs:**

```
KeIntSteerGetSteeringMode             - Get current steering mode
KeIntSteerIsSteeringEnabled           - Query if steering is enabled
KeIntSteerAssignCpuSet                - Assign CPU set for interrupt
KeIntSteerAssignCpuSetForGsiv         - Assign CPU set for specific GSIV
KeIntSteerSnapPerf                    - Snapshot performance counters
```

**ETW/GUID Control:**

```
GUID_INTSTEER_MODE                    - Steering mode GUID
GUID_INTSTEER_TIME_UNPARK_TRIGGER    - Time-based unpark trigger GUID
GUID_INTSTEER_LOAD_PER_PROC_TRIGGER  - Per-processor load trigger GUID
```

### 3.3 Steering Globals

| Global | Value | Description |
|---|---|---|
| `nt!KiIntSteerEnabled` | 0 | Interrupt steering NOT enabled |
| `nt!KiInterruptSteeringFlags` | 0 | Steering flags (0 = none) |
| `nt!PpmIntSteerDisabled` | 0 | PPM steering disabled flag |
| `nt!PpmIntSteerMode` | 0 | Steering mode (0 = disabled) |
| `nt!PpmIntSteerLoadMax` | 0x32 (50) | Maximum load % before steering |
| `nt!PpmIntSteerTriggerMax` | 0x64 (100) | Maximum trigger threshold |

### 3.4 Steering Integration with PPM

```
PopIntSteerSetMode                    - Set interrupt steering mode
PopIntSteerSetTimeUnparkTrigger       - Set time-based unpark trigger
PopIntSteerSetPerProcTrigger          - Set per-processor trigger
PopInterruptSteeringEnabled           - Check if steering is enabled
```

---

## 4. Interrupt Time & Clock

### 4.1 Interrupt Time Management

```
KeQueryInterruptTimePrecise           - Query precise interrupt time
KeQueryUnbiasedInterruptTime          - Query unbiased interrupt time
KeQueryUnbiasedInterruptTimePrecise   - Query precise unbiased time
KeRebaselineInterruptTime             - Rebaseline interrupt time counter
KeAdjustInterruptTime                 - Adjust interrupt time
KiComputeNewInterruptTime             - Compute new interrupt time value
KeAreInterruptsEnabled                - Check if interrupts are enabled
```

| Global | Description |
|---|---|
| `nt!KiInterruptTimeErrorAccumulator` | Interrupt time error accumulator |

### 4.2 Clock Interrupt

```
HalpTimerClockInterrupt               - Main clock interrupt handler
HalpTimerClockInterruptWork           - Clock interrupt work routine
HalpTimerAlwaysOnClockInterrupt       - Always-on timer clock interrupt
HalpTimerPrepareClockInterrupt        - Prepare clock interrupt
HalpTimerConfigureInterrupt           - Configure timer interrupt
HalpTimerStartProfileInterrupt        - Start profiling interrupt
HalpTimerStopProfileInterrupt         - Stop profiling interrupt
KeClockInterruptNotify                - Clock interrupt notification
KiSendClockInterruptToTargetProcessor - Send clock to specific CPU
KiTimerExpirationDpc                  - Timer expiration DPC
```

---

## 5. Notable DPC Routines (System DPCs)

### 5.1 Timer/Time DPCs

```
ExpTimerDpcRoutine                    - Executive timer DPC
ExpTimeRefreshDpcRoutine              - Time refresh DPC
ExpCenturyDpcRoutine                  - Century rollover DPC
ExpTimeZoneDpcRoutine                 - Timezone change DPC
ExpNextYearDpcRoutine                 - Year change DPC
KiTimerExpirationDpc                  - Kernel timer expiration DPC
```

### 5.2 Registry/Configuration DPCs

```
CmpLazyFlushDpcRoutine                - Registry lazy flush
CmpEnableLazyFlushDpcRoutine          - Enable lazy flush
CmpLazyCommitDpcRoutine               - Registry lazy commit
CmpFreezeThawDpcRoutine               - Registry freeze/thaw
CmpDelayFreeRMDpcRoutine              - Delayed resource descriptor free
```

### 5.3 Memory Management DPCs

```
MiStartDpcZeroingRound                - Start zero-page DPC round
MiZeroPageCalibrateDpc                - Calibrate zero page DPC
MiDemoteSlabEntriesDpc                - Demote slab entries
MiChangeSlabIdentitiesDpc             - Change slab identities
MiFreeUnusedPfnPagesDpc               - Free unused PFN pages
MiFreedUnusedPfnPagesDpc              - Post-free notification
MiSpecialPurposeMemoryCacheUpdateDpc  - SP memory cache update
MiUpdatePageThresholdsDpc             - Page threshold update
MiApplyImageHotPatchDpc               - Apply hot patch via DPC
```

### 5.4 Power Management DPCs

```
PopThermalZoneDpc                     - Thermal zone monitoring
PopWatchdogDpc                        - Power watchdog DPC
PopCoalesingTimerDpcCallback          - Coalescing timer DPC
PopAwayModeUserPresenceDpc            - Away mode user presence
PopBatteryWakeDpc                     - Battery wake DPC
PopPepIdleTimeoutDpcRoutine           - PEP idle timeout DPC
PopRefreshEstimateAfterSpoilingDpc    - Refresh estimate after spoiling
PopFxResidentTimeoutDpcRoutine        - FX resident timeout
PopFxIdleTimeoutDpcRoutine            - FX idle timeout DPC
```

### 5.5 Scheduler DPCs

```
KiTriggerForegroundBoostDpc           - Foreground boost trigger
KiSoftParkElectionDpcRoutine          - Soft park election DPC
KiForceParkDutyCycleDpcCallback       - Force park duty cycle DPC
KiForceIdleStartDpcRoutine            - Force idle start
KiForceIdleStopDpcRoutine             - Force idle stop
KiForceIdleParkUnparkDpcRoutine       - Force idle park/unpark
KiEntropyDpcRoutine                   - Entropy gathering DPC
KiSlistRollbackDpc                    - SLIST rollback DPC
KiMakeSecureKernelNonGlobalDpc        - Secure kernel non-global DPC
KiEpfCompletionDpcRoutine             - EPF completion DPC
```

---

## 6. Optimization APIs Summary

### Registry/Power Settings for Interrupt/DPC Tuning

| Setting | Path | Purpose |
|---|---|---|
| `DpcTimeout` | `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\kernel` | DPC watchdog timeout (ms) |
| `DpcWatchdogProfileOffset` | Same | Profile offset for DPC watchdog |
| `InterruptSteeringMode` | PPM GUID | Interrupt steering mode (0=off, 1=performance) |
| `ThreadDpcEnable` | Kernel global | Enable threaded DPC (default: 1) |
| `RssBaseCpu` | NDIS | Base CPU for RSS/interrupt distribution |
| `MaxNumRssCpus` | NDIS | Max CPUs for RSS interrupt handling |

### Kernel Objects for Interrupt/DPC Management

| Object | Use |
|---|---|
| `KINTERRUPT` | Interrupt object (ISR, DPC, vector, affinity) |
| `KDPC` | DPC object (routine, importance, target CPU) |
| `KPRCB` (per-CPU) | DPC list, interrupt time, dispatch state |
| `INTERRUPT_CONNECTION_DATA` | Interrupt connection metadata |
| `IO_INTERRUPT_STRUCTURE` | I/O interrupt block (passive + DPC) |

### Exported APIs for Drivers

| API | Purpose |
|---|---|
| `IoConnectInterrupt` / `IoConnectInterruptEx` | Register device ISR |
| `IoDisconnectInterrupt` / `IoDisconnectInterruptEx` | Unregister ISR |
| `KeInitializeDpc` / `KeInsertQueueDpc` | Initialize and queue DPC |
| `KeInitializeThreadedDpc` | Initialize threaded (passive-level) DPC |
| `KeSetTargetProcessorDpc` / `KeSetTargetProcessorDpcEx` | Target DPC to CPU |
| `KeSetImportanceDpc` | Set DPC priority |
| `KeFlushQueuedDpcs` | Flush all pending DPCs |
| `KeQueryInterruptTimePrecise` | Query precise interrupt time |
| `KeAreInterruptsEnabled` | Check interrupt state |
| `NdisMQueueDpc` / `NdisMQueueDpcEx` | Queue NDIS DPC (RSS-targeted) |
| `HalRequestSoftwareInterrupt` | Request software interrupt |
| `KeRegisterObjectDpc` | Register object-based DPC |

---

## 7. Key Findings for Optimizer

1. **DPC Watchdog is fully disabled** (`KeDpcTimeoutMs=0`, `KeDpcSoftTimeoutMs=0`, `KeDpcCumulativeSoftTimeoutMs=0`) — no DPC timeout monitoring is active; enabling it can detect driver-caused stalls
2. **Interrupt Steering is disabled** (`KiIntSteerEnabled=0`, `PpmIntSteerMode=0`) — all device interrupts likely hit CPU 0; enabling can spread load
3. **Threaded DPC is enabled** (`KeThreadDpcEnable=1`) — DPCs can execute at passive level, reducing DISPATCH_LEVEL contention
4. **Secondary Interrupt Services are disabled** (`KiSecondaryInterruptServicesEnabled=0`) — not using message-signaled secondary interrupts
5. **DPC queue thresholds** (`KiMaximumDpcQueueDepth=4`, `KiAdjustDpcThreshold=20`) control when DPC scheduling becomes aggressive — these are tunable
6. **Long DPC detection** thresholds (`KiLongDpcQueueThreshold=3`, `KiLongDpcRuntimeThreshold=100`) define when a DPC is considered "long" for ETW logging
7. **Passive-level interrupts** have a full infrastructure with dedicated worker threads — important for devices that cannot handle interrupts at DISPATCH_LEVEL
8. **DPC Gangs** enable synchronized multi-CPU DPC execution (used by memory manager for zeroing)
9. **ARM64 GICv3** provides hardware interrupt prioritization — `HalpInterruptHasPriorities` indicates priority support
10. **DPC runtime history** hash tables track per-DPC execution times — queryable via `KiQueryDpcRuntimeHistory`
11. **MSI support** is determined at boot (`HalpInterruptMsiSupported`, `HalpInterruptMsiSupportDetermined`) — affects interrupt delivery efficiency
12. **CPU starvation events** are traced via ETW when DPCs run too long — monitorable for system responsiveness issues
