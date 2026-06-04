# Storage Stack API Research (Windows 11 ARM64 Build 26100)

> Kernel-debugged via WinDbg MCP on ARM64 target (Parallels VM)
> Modules: `storport` (fffff800`69f00000 - fffff800`6a15d000), `CLASSPNP` (fffff800`6afb0000 - fffff800`6b02a000), `disk` (fffff800`6af80000 - fffff800`6afa1000), `storahci` (fffff800`69ec0000), `storqosflt` (fffff800`6da30000), `spaceport` (fffff800`69d00000), `volmgr` (fffff800`69df0000), `volmgrx` (fffff800`69e20000), `volsnap` (fffff800`6ae20000), `storvsp` (fffff800`6c8c0000)

---

## 1. IO Request Flow & Queue Management

### 1.1 Storport Device Queue (RAID_UNIT_DEVICE_QUEUE)

The storage I/O scheduler in storport uses a per-LUN device queue with priority support:

```
RaidInitializeDeviceQueue        - Initialize device queue for LUN
RaidDeleteDeviceQueueEntry       - Delete queue entry
RaidLockDeviceQueue / RaidUnlockDeviceQueue  - Queue-level locking
RaidPauseDeviceQueue / RaidResumeDeviceQueue - Pause/resume I/O
RaidStallDeviceQueue             - Stall queue (wait condition)
RaidRemovePendingDeviceQueue     - Remove pending entry
RaidRemoveIoQueue                - Remove I/O queue

StorPushRequestToDeviceQueue     - Push request to device queue
StorPopRequestFromDeviceQueue    - Pop next request from queue
StorRestartDeviceCommandQueue    - Restart command queue
StorRestartDeviceIoQueue         - Restart I/O queue (3 variants)
StorRestartDeviceLowPriorityIoQueue - Restart low-priority queue
StorRefillShadowQueue            - Refill NVMe shadow queue
```

### 1.2 Deferred Queue (Background Processing)

```
RaidCreateDeferredQueue          - Create deferred work queue
RaidDeleteDeferredQueue          - Delete deferred queue
RaidInitializeDeferredQueue      - Initialize deferred queue
RaidQueueDeferredItem            - Queue item for deferred processing
RaidDeferredQueueDpcRoutine      - DPC for deferred processing
RaidAdjustDeferredQueueDepth     - Adjust deferred queue depth dynamically
RaidQueueWaitCheckDpcRoutine     - Wait check timer DPC
```

### 1.3 Queue Depth Control

```
RaUnitSetQueueDepth              - Set per-LUN queue depth
RaidSetIoQueueDepth              - Set I/O queue depth
RaidUnitSetInitialQueueDepth     - Set initial queue depth at LUN creation
StorPortSetDeviceQueueDepth      - Public API to change device queue depth
NvmeControllerGetPreferredIoQueueDepth - NVMe optimal queue depth query
```

Key globals:
```
storport!DeviceQueueIoWaitThreshold = 300000000 (30 seconds in 100ns units)
storport!LunQueue                   - LUN queue depth control structure
EventSetLunQueueDepthBtl8           - ETW event for queue depth changes
```

---

## 2. IO Priority & Scheduling

### 2.1 Priority Hint Integration

```
storport!_imp_IoGetIoPriorityHint      - Get IRP priority hint (Critical/High/Normal/Low/VeryLow)
CLASSPNP!_imp_IoGetIoPriorityHint       - Same at class driver level
CLASSPNP!_imp_IoRegisterPriorityCallback  - Register for priority change callback
CLASSPNP!_imp_IoUnregisterPriorityCallback - Unregister priority callback
CLASSPNP!_imp_IoGetPagingIoPriority     - Get paging I/O priority
```

### 2.2 IO Boost Mechanism

```
CLASSPNP!ClassIoBoostPriority                - Boost I/O priority for specific request
CLASSPNP!ClasspIoBoostPriorityContext        - Context for priority boost operation
```

### 2.3 NVMe Low Priority IO Queue

```
NvmeProcessPendingLowPriorityIo        - Process low-priority pending I/O
NvmeLowPriorityIoDpcRoutine            - DPC for low-priority processing
StorRestartDeviceLowPriorityIoQueue    - Restart low-priority queue
storport!HiberFileHybridPriority       - Hibernate file hybrid priority setting
```

### 2.4 Storage QoS Filter (storqosflt)

```
storqosflt! (fffff800`6da30000 - fffff800`6da4a000, no symbols)
```

Storage QoS filter minifilter - manages per-disk I/O throttling and minimum/maximum IOPS guarantees.
Key globals:
```
storport!g_QosFlags = 0x00000000  - QoS feature flags (currently disabled)
```

---

## 3. Performance State Management (PoFx)

### 3.1 Performance States Registration

```
RaidRegisterPerfStates               - Register adapter perf states with PoFx
RaidValidatePerfSets                 - Validate perf state sets
RaidValidatePerfSet                  - Validate single perf state set
RaidGetStorPoFxPerfState             - Get current perf state
RaidPreInitializePerfOpts            - Pre-init perf options
RaidInitializePerfOpts               - Initialize perf options
RaidInitializePerfOptsPassive        - Passive-level perf init
RaidAdapterPerfStateCallback         - Adapter perf state change callback
RaidUnitPerfStateCallback            - Unit (LUN) perf state callback
```

### 3.2 NVMe Performance States

```
NvmePreInitializePerfOpts            - Pre-init NVMe perf options
NvmeInitializePerfOpts               - Init NVMe perf options
NvmeControllerInitializePerfOptions  - Controller-level perf init
NvmeRegisterPerfStates               - Register NVMe perf states
NvmeControllerPerfStateTransition    - State transition handler
NvmeAdapterPerfStateCallback         - NVMe adapter callback
NvmeNamespacePerfStateCallback       - NVMe namespace callback
NvmeControllerSetIoQueueCount        - Change I/O queue count per perf state
NvmeControllerSetIoQueueCountCompletion - Completion for queue count change
```

### 3.3 Performance Counter Sets

```
SpPerfAddUnitQueueCounterSet         - Per-unit queue counters
SpPerfAddUnitReadCounterSet          - Per-unit read counters
SpPerfAddUnitWriteCounterSet         - Per-unit write counters
SpPerfAddUnitTransferCounterSet      - Per-unit transfer counters
```

Key globals:
```
storport!g_RaidPerfRedirectRefCount    - Performance redirect reference count
storport!g_RaidPerfRedirectGroupCount  - Number of redirect groups
g_StorpTraceLoggingPerformancePeriod   - Performance telemetry period
g_StorpTraceLoggingPerformanceEnabled  - Performance telemetry enabled flag
```

---

## 4. Completion & DPC Infrastructure

### 4.1 Completion Processing

```
RaidCompletionDpcRoutine             - Main completion DPC
RaidLogMiniportCompletion            - Log miniport completion
RaidCheckPerProcessorCompletions     - Check per-processor completion queues
RaidNotifyPerProcessorCompletions    - Notify processors of completions
RaidpIsPerProcessorCompletionsFlushSet - Check if flush is needed
```

### 4.2 NVMe Completion Queues

```
NvmeCompletionDpcRoutine             - NVMe CQ processing DPC
NvmeStorMQCompletionDpcRoutine       - StorMQ completion DPC (multi-queue)
NvmeIoCompletionRedirectDpcRoutine   - Redirect completion to target CPU
NvmeControllerInitializeCompletionQueueDPC - Initialize CQ DPC
NvmeControllerCompletionQueueInit    - Initialize completion queue
NvmeControllerCompletionQueuePollingQuiesce - Polling mode quiesce
NvmeControllerCompletionDpcQuiesce   - Quiesce completion DPC
NvmeProcessPendingIoInCompletionDpc  - Process pending IO in completion DPC
ProcessNVMeCompletionQueues          - Process all NVMe CQs
```

### 4.3 DPC Redirection & Affinity

```
GetQueueCompletionAffinity           - Get target CPU for completion
InitializeNumaNodeCompletionAffinity - Initialize NUMA-aware completion affinity
RaidpAdapterRedirectDpcRoutine       - Redirect DPC to target processor
g_RaidDPCRedirectionProcessors       - Array of redirect target processors
g_RaidDPCRedirectionInitLock         - Init lock for redirection state
g_RaidPerProcessorState              - Per-processor state array
```

Key global:
```
storport!DpcCompletionLimit = 0x80 (128)  - Max completions per DPC invocation
```

---

## 5. Power Management

### 5.1 Runtime Power (PoFx Integration)

```
RaInitializePower                    - Initialize power management
RaidRegisterForRuntimePowerManagement - Register for runtime PM
RaUnitStorageEnableIdlePower         - Enable idle power management for LUN
RaUnitStoragePowerActive             - Device active (prevent idle)
RaUnitStoragePowerIdle               - Device idle (allow power down)

RaidUnitPoFxIdleComponentFromMiniport - Idle component from miniport
RaidAdapterPoFxIdleComponent          - Adapter idle
RaidAdapterPoFxSetDeviceIdleTimeout   - Set adapter idle timeout
RaidUnitPoFxSetDeviceIdleTimeout      - Set LUN idle timeout
RaidUnitAdaptiveIdleTimeout           - Adaptive idle timeout algorithm
```

### 5.2 NVMe Power

```
NvmeControllerPowerInitialize        - Initialize NVMe power
NvmeControllerPowerUp                - Power up controller
NvmeControllerPowerDown              - Power down controller
NvmeControllerSetPowerState          - Set power state
NvmeControllerGetPowerState          - Query power state
NvmeControllerPowerSetPState         - Set P-State (performance)
NvmeControllerPowerSetFState         - Set F-State (functional)
NvmeControllerValidatePowerStates    - Validate supported power states
NvmeControllerSetFStateIdleTimer     - F-State idle timer
NvmeControllerUpdateResumeLatencyTolerance - Update resume latency
NvmeControllerMaxOperationalPower    - Max operational power setting
NvmeControllerSystemPowerHint        - System power hint handler
NvmeControllerPowerSettingChangeNotification - Power setting callback
NvmeGetAutoPowerStateTransition      - Get auto power state config
NvmeSetAutoPowerStateTransition      - Configure auto power state transition
NvmeSetNonOperationalPowerStatePermissiveMode - Non-op PS permissive mode
```

### 5.3 Power GUIDs (Registry Configurable)

```
GUID_NVME_POWER_IDLE_TIMEOUT1 = {registry-controlled NVMe idle timeout 1}
GUID_NVME_POWER_IDLE_TIMEOUT2 = {registry-controlled NVMe idle timeout 2}
GUID_NVME_POWER_STATE_TRANSITION_LATENCY_TOLERANCE1 = {latency tolerance 1}
GUID_NVME_POWER_STATE_TRANSITION_LATENCY_TOLERANCE2 = {latency tolerance 2}
GUID_NVME_POWER_NOPPME = {NVMe no-PPME override}
GUID_LOW_POWER_EPOCH = {low power epoch notification}
GUID_DISK_MAX_POWER = {max disk power override}
GUID_ACDC_POWER_SOURCE = {AC/DC power source notification}
```

Key globals:
```
storport!KsrPowerDownOptimizationEnabled = 1  (enabled)
storport!RuntimePowerDisabled = 0  (runtime PM enabled)
```

### 5.4 CLASSPNP Power

```
ClassDispatchPower                   - Power IRP dispatch
ClassSpinDownPowerHandler            - Spin-down disk power handler
ClassStopUnitPowerHandler            - STOP UNIT for power down
ClassMinimalPowerHandler             - Minimal power handler
ClasspPowerHandler                   - Full power handler
ClasspPowerUpCompletion / ClasspPowerDownCompletion - Power transition completions
ClasspEnableIdlePower                - Enable idle power management
ClasspPowerIdleDevice                - Mark device idle
ClasspPowerActivateDevice            - Activate device from idle
ClasspPowerSettingCallback           - Power setting change callback
```

---

## 6. NUMA & Processor Topology

### 6.1 NUMA-Affinity for Storage

```
StorIsSoftNumaOptIn                  - Check if soft NUMA is opted in
InitializeNumaNodeCompletionAffinity - Initialize NUMA-aware completion routing
GetQueueCompletionAffinity           - Get completion CPU based on NUMA topology
RaUnitStorageQueryDeviceNumaPropertyIoctl - Query device NUMA proximity domain
NvmeNamespaceStorageQueryDeviceNumaPropertyIoctl - NVMe NUMA query
DEVPKEY_Device_Numa_Proximity_Domain - Device property for NUMA domain
```

### 6.2 Processor Count Globals

```
g_MaximumProcessorCount      - Max processor count
g_RaidNumberProcessors       - Number of processors for RAID
g_ProcessorCountPerGateway   - Processors per NVMe gateway
g_HeterogenousCPU            - Big.LITTLE/heterogeneous CPU flag
g_CpuInfo                    - CPU info structure
```

---

## 7. Latency Monitoring

### 7.1 High Latency Detection

```
storport!HighLatencyIoThreshold = 300000000 (30s in 100ns units)  - Threshold for high-latency I/O detection
storport!PortGetIoLatencyCapValue       - Get I/O latency cap from registry
EventHighLatencyIo                       - ETW event for high-latency I/O
EventNVMeHighLatencyIo                   - ETW event for NVMe high-latency I/O
```

### 7.2 Latency Buckets & Telemetry

```
TraceLoggingResetLatencyBuckets       - Reset latency histogram buckets
StorpTelemetryLogUnitPerfDataCriticalData  - Log critical perf data
StorpTelemetryLogUnitPerfDataMeasures      - Log perf measurements
StorpTelemetrySendUnitPerfData             - Send unit performance data
```

---

## 8. NVMe Multi-Queue Infrastructure

### 8.1 Submission Queues

```
NvmeControllerSubmissionQueueInit     - Init submission queue
NvmeControllerCreateCommandQueue      - Create SQ
NvmeControllerDeleteCommandQueue      - Delete SQ
NvmeControllerSubmissionQueueQuiesce  - Quiesce SQ
NvmeControllerSubmissionQueueCompletionQuiesce - Combined quiesce
NvmeSubmissionQueueReInit             - Re-init after reset
NvmeControllerRequeueSQPendingRequests - Requeue pending requests
```

### 8.2 I/O Queues (Per-Namespace)

```
NvmeNamespaceCreateIoQueue            - Create namespace I/O queue
NvmeNamespaceCreateIoQueue2           - Create I/O queue (v2)
NvmeNamespaceDeleteIoQueue            - Delete namespace I/O queue
NvmeNamespaceDeleteIoQueue2           - Delete I/O queue (v2)
NvmeNamespaceLockIoQueue              - Lock I/O queue
NvmeNamespaceUnlockIoQueue            - Unlock I/O queue
NvmeNamespaceFreeIoQueueResources     - Free I/O queue resources
NvmeNamespaceQueueIo                  - Queue I/O to namespace
NvmeNamespaceProcessQueueRequests     - Process queued requests
```

### 8.3 NVMe Over Fabric (NVMe-oF)

```
NvmeAdapterNvmeConnectFabricControllerQueue     - Connect fabric queue
NvmeAdapterNvmeDisconnectFabricControllerQueue  - Disconnect fabric queue
NvmeAdapterCreateFabricControllerIoQueues       - Create fabric I/O queues
NvmeAdapterCreateFabricControllerQueue          - Create fabric control queue
NvmeAdapterDeleteFabricControllerQueue          - Delete fabric queue
NvmeAdapterSetFabricControllerIoQueueCount      - Set fabric I/O queue count
```

---

## 9. Telemetry & Diagnostics

### 9.1 Performance Telemetry

```
StorpInitializePerfTelemetry          - Initialize perf telemetry system
StorpUninitializePerfTelemetry        - Cleanup perf telemetry
StorpTelemetryCollectPerfData         - Collect performance data
StorpTelemetrySendUnitPerfData        - Send per-unit perf data
StorpTelemetryNvmeSendNamespacePerfData - NVMe namespace perf data
StorpTelemetryCollectNvmePerfData     - Collect NVMe perf
StorpTelemetryLogUnitQosDataMeasures  - Log QoS measurements
StorpTelemetrySendUnitQos             - Send QoS data
StorpTelemetrySendUnitIoSizeDistributionData - I/O size distribution
```

### 9.2 Health Telemetry

```
StorpTelemetryNvmeHealthInfo          - NVMe health info
StorpTelemetrySendAdapterNvmeHealthInfo - Adapter NVMe health
StorpTelemetrySendUnitNvmeHealthInfo   - Unit NVMe health
StorpTelemetryMarkUnitUnresponsive    - Mark unit unresponsive
StorpTelemetryMarkUnitResponsive      - Mark unit responsive
RaidUnitDeviceHealthTelemetrySupported - Check health telemetry support
StorpTelemetrySendUnitErrorDataSummary - Error summary
StorpTelemetrySendUnitUniqueErrorData  - Unique error data
```

---

## 10. Disk Class Driver (disk.sys)

### 10.1 Cache Management

```
DiskSetCacheInformation              - Set disk write cache settings
DiskGetCacheInformation              - Get disk write cache settings
DiskIoctlSetCacheInformation         - IOCTL handler for cache set
DiskIoctlGetCacheInformation         - IOCTL handler for cache get
DiskIoctlSetCacheSetting             - Set cache setting (newer API)
DiskIoctlGetCacheSetting             - Get cache setting
DiskLogCacheInformation              - Log cache info for telemetry
DisableWriteCache                    - Disable write cache
```

### 10.2 SMART / Failure Prediction

```
DiskEnableSmart                      - Enable SMART
DiskPerformSmartCommand              - Execute SMART command
DiskReadSmartLog                     - Read SMART log
DiskWriteSmartLog                    - Write SMART log
DiskDetectFailurePrediction          - Detect failure prediction mode
DiskInitializeFailurePrediction      - Initialize failure prediction
DiskEnableDisableFailurePrediction   - Enable/disable prediction
DiskEnableDisableFailurePredictPolling - Enable/disable polling
DiskSetInfoExceptionInformation      - Set info exception (SMART) settings
DiskGetInfoExceptionInformation      - Get info exception settings
DiskReadFailurePredictData           - Read prediction data
DiskReadFailurePredictStatus         - Read prediction status
DiskReadFailurePredictThresholds     - Read prediction thresholds
DiskSendFailurePredictIoctl          - Send prediction IOCTL
DiskExecuteSmartDiagnostics          - Execute SMART diagnostics
DiskIoctlPredictFailure              - IOCTL handler for predict failure
DiskIoctlEnableFailurePrediction     - IOCTL to enable prediction
```

---

## 11. Optimization APIs Summary

### User-Mode APIs for Storage Optimization

| API | Header | Purpose |
|---|---|---|
| `IOCTL_STORAGE_SET_PROPERTY` | winioctl.h | Set storage properties (power, queue depth, etc.) |
| `IOCTL_STORAGE_GET_PROPERTY` | winioctl.h | Query storage properties |
| `IOCTL_STORAGE_SET_TEMPERATURE_THRESHOLD` | winioctl.h | Set NVMe temperature threshold |
| `IOCTL_STORAGE_GET_DEVICE_NUMBER` | winioctl.h | Get disk device number |
| `IOCTL_DISK_SET_CACHE_INFORMATION` | winioctl.h | Set disk write cache policy |
| `IOCTL_DISK_GET_CACHE_INFORMATION` | winioctl.h | Query disk write cache state |
| `IOCTL_STORAGE_POWER_ACTIVE` | ntddstor.h | Mark storage device as active |
| `IOCTL_STORAGE_POWER_IDLE` | ntddstor.h | Mark storage device as idle |
| `IOCTL_STORAGE_GET_DEVICE_INTERNAL_LOG` | ntddstor.h | Get NVMe internal logs |
| `FSCTL_SET_REPARSE_POINT` | winioctl.h | Storage filter control |
| `StorageQoS` (WMI) | storqos.h | Storage QoS management |

### Kernel Objects for Optimization

| Object | Module | Use |
|---|---|---|
| `RAID_ADAPTER` | storport | Per-adapter state, power, perf states |
| `RAID_UNIT` | storport | Per-LUN state, queue depth, idle power |
| `RAID_DEVICE_QUEUE` | storport | I/O scheduling queue per LUN |
| `DEFERRED_QUEUE` | storport | Background work processing queue |
| `NVME_CONTROLLER` | storport | NVMe controller with multi-queue |
| `NVME_NAMESPACE` | storport | NVMe namespace (LUN equivalent) |
| `FUNCTIONAL_DEVICE_EXTENSION` | CLASSPNP | Class driver per-device extension |
| `PHYSICAL_DEVICE_EXTENSION` | CLASSPNP | Per-path device extension (MPIO) |
| `TRANSFER_PACKET` | CLASSPNP | I/O transfer packet with SRB |

---

## 12. Key Findings for Optimizer

1. **Device Queue Depth is tunable** via `StorPortSetDeviceQueueDepth` and registry. Default `DeviceQueueIoWaitThreshold` = 30 seconds, adjustable per-LUN
2. **IO Priority integration** exists at both storport and CLASSPNP levels via `IoGetIoPriorityHint` and `IoRegisterPriorityCallback` - optimizer can influence I/O scheduling
3. **DPC Completion Limit** (`DpcCompletionLimit = 0x80 = 128`) controls max completions per DPC - tunable for throughput vs latency tradeoff
4. **NUMA-aware completion routing** via `InitializeNumaNodeCompletionAffinity` and `GetQueueCompletionAffinity` ensures storage completions land on correct NUMA node
5. **DPC Redirection** (`g_RaidDPCRedirectionProcessors`) allows steering completion DPCs to specific processors for cache locality
6. **NVMe Performance States** via PoFx - adapter and namespace have independent P-State and F-State control, including `NvmeControllerPowerSetPState` for performance scaling
7. **Idle Power Management** with adaptive timeouts (`RaidUnitAdaptiveIdleTimeout`, `GUID_NVME_POWER_IDLE_TIMEOUT1/2`) - configurable idle timeout per power scenario
8. **Write Cache control** via `IOCTL_DISK_SET_CACHE_INFORMATION` - optimizer can toggle write cache for performance vs safety tradeoff
9. **High Latency I/O detection** at 30-second threshold (`HighLatencyIoThreshold`) with ETW events - optimizer can monitor and react
10. **KsrPowerDownOptimization** is enabled (`= 1`) - kernel shutdown/restart optimization for storage
11. **SMART/Failure Prediction** data accessible via disk.sys IOCTLs - optimizer can read disk health and adjust caching/aggressiveness
12. **NVMe multi-queue** with per-namespace I/O queues and per-CPU completion DPCs provides excellent parallelism on multi-core systems
