# C-State / P-State & Idle Management Research (Windows 11 ARM64 Build 26100)

> Kernel-debugged via WinDbg MCP on ARM64 target (Parallels VM)
> Module: `ntoskrnl` (HAL is linked into ntoskrnl on ARM64)

---

## 1. PPM — Processor Power Manager Overview

PPM is the kernel subsystem governing C-states (idle), P-states (performance/frequency), and core parking. On ARM64, idle transitions use PSCI (Power State Coordination Interface) firmware calls.

---

## 2. C-State / Idle Management (PpmIdle)

### 2.1 Idle State Selection & Execution

```
PpmIdleSelectStates                        - Select idle state for processor
PpmIdleExecuteTransition                   - Execute idle state transition
PpmIdlePsciExecute                         - Execute PSCI firmware idle call
PpmIdlePsciPreselect                       - Preselect PSCI idle parameters
PpmIdleEvaluateConstraints                 - Evaluate idle constraints (latency, residency)
PpmIdleCheckProcessorStateEligibility      - Check if processor eligible for given C-state
PpmIdleDefaultExecute                      - Default idle execute (WFI/halt)
PpmIdleDefaultComplete                     - Default idle completion handler
PpmIdleUpdateProcessorLatencyLimit         - Update per-processor latency limit
PpmIdleUpdateSystemLatencyLimit            - Update system-wide latency limit
PpmIdleInstallDefaultStates                - Install default idle state table
PpmIdleRegisterDefaultStates               - Register default idle states with PPM
```

### 2.2 Coordinated Idle States

Multi-core synchronized C-state entry — ensures cores in same power domain idle together.

```
PpmIdleCheckCoordinatedDependency          - Check coordinated idle dependency
PpmIdleCheckCoordinatedStateEligibility    - Check eligibility for coordinated state
PpmIdleCoordinatedMode (global)            - Coordinated mode flags
```

### 2.3 Veto & Watchdog

Veto mechanism prevents deep C-states when latency-sensitive workloads are active.

```
PpmIdlePrevetoWatchdog                     - Pre-veto watchdog for idle state selection
PpmIdleCsVetoAccountingUpdateBlock         - Update CS veto accounting block
PpmIdleUpdateSelectionStatistics           - Update idle state selection statistics
PpmIdleTransitionStall                     - Stall during idle transition
```

### 2.4 PSCI (ARM64 Firmware Idle Interface)

PSCI is the ARM-standard power management firmware interface used for idle state entry.

```
PpmIdlePsciExecute                         - Execute PSCI CPU_SUSPEND call
PpmIdlePsciPreselect                       - Preselect PSCI state parameters
```

**PSCI Functions (via HAL/ntoskrnl on ARM64):**

| PSCI Function | Purpose |
|---|---|
| `PSCI_CPU_SUSPEND` | Enter idle state with wakeup latency |
| `PSCI_CPU_OFF` | Power off CPU (used for hotplug) |
| `PSCI_SYSTEM_SUSPEND` | System-wide suspend |

### 2.5 Idle Globals

| Global | Value | Description |
|---|---|---|
| `PpmIdleDurationExpirationTimeoutMs` | 4 | Idle duration expiration timeout (ms) |
| `PpmIdleMaxUnexpectedInterrupt` | 5 | Max unexpected interrupts before re-evaluation |
| `PpmIdleDynamicHintAdjustEnabled` | 1 | Dynamic idle hint adjustment enabled |
| `PpmIdleCoordinatedMode` | 0x100 | Coordinated idle mode flags |
| `PpmIdleClusterIdleMitigation` | 0 | Cluster idle mitigation disabled |
| `PpmIdleDisableStatesAtBoot` | 0 | No idle states disabled at boot |
| `PpmIdleDurationExpirationTimeout` | (large) | Full expiration timeout value |

**Key Observations:**
- Dynamic hint adjustment is enabled (`PpmIdleDynamicHintAdjustEnabled = 1`) — the kernel adjusts idle hints based on workload patterns
- Coordinated mode is active (`PpmIdleCoordinatedMode = 0x100`) — multi-core idle coordination is configured
- Cluster idle mitigation is disabled — individual cores can idle independently of cluster state
- No idle states are disabled at boot — all firmware C-states are available

---

## 3. P-State / Performance Management (PpmPerf)

### 3.1 Performance State Selection & Application

```
PpmPerfSelectProcessorState                - Select target P-state for processor
PpmPerfApplyProcessorState                 - Apply selected P-state to processor
PpmPerfApplyProcessorStates                - Apply P-states to multiple processors
PpmPerfSelectDomainStates                  - Select P-states for performance domain
PpmPerfApplyDomainState                    - Apply P-state to entire domain
PpmPerfApplyLatencyHint                    - Apply latency hint (boost)
PpmPerfApplyLatencyHints                   - Apply all pending latency hints
PpmPerfArbitratorApplyProcessorState       - Arbitrator: resolve conflicting P-state requests
```

### 3.2 QoS (Quality of Service) Performance

```
PpmPerfCalculateQosClassPolicies           - Calculate QoS class policies
PpmPerfRecordUtility                       - Record processor utility value
PpmPerfSnapUtility                         - Snapshot current utility
PpmPerfSnapDeliveredPerformance            - Snapshot delivered performance level
PpmPerfAction                              - PPM performance action handler
PpmPerfQueueAction                         - Queue performance action
PpmPerfCommitPerformance                   - Commit performance state change
PpmPerfCheckRequired                       - Check if perf state change required
```

### 3.3 Feedback & Monitoring

```
PpmPerfGetCurrentState                      - Get current P-state
PpmPerfGetCurrentFrequency                  - Get current frequency (MHz)
PpmPerfFeedbackCounterRead                  - Read hardware feedback counter
PpmPerfReadFeedback                         - Read all feedback counters
```

### 3.4 Initialization & Registration

```
PpmPerfInitialize                          - Initialize PPM performance subsystem
PpmPerfRegisterNativePerfStates            - Register native P-states from firmware
```

### 3.5 Performance Globals

| Global | Value | Description |
|---|---|---|
| `PpmPerfQosEnabled` | 0xC0000100 | QoS performance enabled flags |
| `PpmPerfBoostAtGuaranteed` | 0 | No boost above guaranteed frequency |
| `PpmPerfDomainCount` | 0 | Performance domain count (0 = single domain) |
| `PpmPerfStatesRegistered` | 0x00200001 | P-states registered flags |
| `PpmPerfMaxOverrideEnabled` | 0 | Max frequency override disabled |
| `PpmPerfSingleStepSize` | 5 | Single step size for P-state transitions (%) |

**Key Observations:**
- QoS performance is partially enabled (`PpmPerfQosEnabled = 0xC0000100`) — QoS class-based boosting is active
- No boost above guaranteed frequency (`PpmPerfBoostAtGuaranteed = 0`) — turbo/overclock is not engaged by PPM
- Single-step P-state transitions use 5% increments — conservative frequency ramping
- Performance domain count is 0 (likely a single domain covering all cores on this VM)

---

## 4. Core Parking (PpmPark)

### 4.1 Parking Policy & Calculation

```
PpmParkCalculateCoreParkingMask            - Calculate core parking bitmask
PpmParkComputeUnparkMask                   - Compute which cores to unpark
PpmParkApplyPolicy                         - Apply parking policy
PpmParkApplyPolicyEx                       - Extended parking policy application
PpmParkDistributeUtility                   - Distribute utility across unparked cores
PpmParkDistributeAllUtility                - Distribute all utility values
PpmParkReportParkedCore                    - Report core as parked
PpmParkReportUnparkedCore                  - Report core as unparked
```

### 4.2 Initialization & Registration

```
PpmParkRegisterParking                     - Register parking support
PpmParkRegisterParkingEx                   - Extended parking registration
PpmParkInitialize                          - Initialize core parking subsystem
```

### 4.3 Soft Parking

Soft parking marks cores as available but prevents new threads from being scheduled to them.

```
PpmParkSoftParkElectionDpcRoutine          - DPC routine for soft parking election
```

### 4.4 Forced Parking & LPI

```
PpmParkApplyForcedMask                     - Apply forced parking mask (override policy)
PpmParkClearForcedMask                     - Clear forced parking mask
PpmParkSetLpiCap                           - Set LPI (Low Power Idle) capacity cap
PpmParkBlockIdle                           - Block idle transitions
PpmParkUnblockIdle                         - Unblock idle transitions
```

### 4.5 SMT & Topology

```
PpmParkEvalualteSmtUnparkPolicy            - Evaluate SMT (hyperthreading) unpark policy
PpmParkFindOverUtilizedProcessors          - Find processors over utilization threshold
PpmParkParkingAvailable                    - Check if parking is available for processor
PpmParkMaximumCoresParked                  - Get maximum cores that can be parked
```

### 4.6 Interrupt Steering Integration

```
PpmParkSteerInterrupts                     - Steer interrupts away from parked cores
```

### 4.7 Parking Globals

| Global | Value | Description |
|---|---|---|
| `PpmParkSoftParkingEnabled` | 0x10000000 | Soft parking enabled flags |
| `PpmParkUseWholeNumaNode` | 0x00013001 | Use whole NUMA node for parking |
| `PpmParkMultiparkGranularity` | 8 | Multi-park granularity |
| `PpmParkNumNodes` | 1 | Number of parking nodes |
| `PpmParkGranularity` | 1 | Parking granularity (1 = per-core) |
| `PpmParkLpiEngaged` | present | LPI engaged state |
| `PpmParkLpiCap` | present | LPI capacity cap |

**Key Observations:**
- Parking granularity is 1 (per-core parking) — finest granularity available
- Single NUMA node (`PpmParkNumNodes = 1`) — expected for a 4-core VM
- Soft parking is configured with flags `0x10000000`
- Multi-park granularity is 8 — can park up to 8 cores at once (more than the 4 available)
- LPI (Low Power Idle) is tracked with engaged state and capacity caps

---

## 5. QoS & Latency Hint System

### 5.1 Latency Hints

Latency hints are per-processor requests to maintain higher P-states for responsiveness.

```
PpmPerfApplyLatencyHint                    - Apply single latency hint
PpmPerfApplyLatencyHints                   - Apply all pending latency hints
```

**Latency Hint Sources:**
- Multimedia playback (audio/video)
- Window manager (DWM) composition
- Game mode / full-screen applications
- Input device activity (mouse/keyboard)

### 5.2 QoS Class-Based Performance

QoS classes map application priority to performance policy:

```
PpmPerfCalculateQosClassPolicies           - Calculate QoS class performance policies
PpmPerfQosEnabled (global)                 - QoS performance enabled flags
```

**QoS Classes (Windows):**

| Class | Boost Behavior |
|---|---|
| High Priority / Realtime | Max frequency, disable deep C-states |
| Multimedia | Sustained high frequency |
| Normal | Default policy |
| Background | Allow deep C-states, lower frequency |
| EcoQoS | Minimize energy, deep C-states allowed |

---

## 6. ARM64-Specific: PSCI Idle Flow

### 6.1 Idle Entry Sequence (ARM64)

```
1. Processor becomes idle (no runnable threads)
2. PpmIdleSelectStates() — select target C-state
3. PpmIdleEvaluateConstraints() — check latency/residency constraints
4. PpmIdleCheckCoordinatedStateEligibility() — check coordinated dependency
5. PpmIdlePsciPreselect() — prepare PSCI parameters
6. PpmIdlePsciExecute() — call PSCI_CPU_SUSPEND via firmware
7. [Firmware enters low-power state]
8. [Interrupt wakes processor]
9. PpmIdleDefaultComplete() — post-idle completion
10. PpmIdleUpdateSelectionStatistics() — record idle statistics
```

### 6.2 PSCI State Mapping

On ARM64, ACPI LPI (Low Power Idle) states map to PSCI idle parameters:

| ACPI LPI State | Description | Typical Latency |
|---|---|---|
| LPI 0 (WFI) | Wait-For-Interrupt | < 1us |
| LPI 1 (Retention) | Core retention (state preserved) | 10-100us |
| LPI 2 (PowerGate) | Core power gate (state lost) | 100-1000us |
| LPI 3 (Cluster PG) | Cluster power gate | 1-10ms |

---

## 7. Kernel Objects for C-State/P-State

| Object | Use |
|---|---|
| `PPM_IDLE_STATE` | Per-C-state metadata (latency, residency, hint) |
| `PPM_PERF_STATE` | Per-P-state metadata (frequency, voltage) |
| `PPM_PROCESSOR_STATE` | Per-processor PPM state (current C/P state) |
| `PPM_PERF_DOMAIN` | Performance domain (shared frequency/voltage) |
| `PPM_PARKING_NODE` | Parking node (group of parkable cores) |
| `PPM_COORDINATED_STATE` | Coordinated idle state (multi-core dependency) |
| `PPM_QOS_POLICY` | QoS class performance policy |
| `PPM_VETO_BLOCK` | C-state veto tracking |

---

## 8. User-Mode / Registry APIs for Optimization

### Power Plan APIs

| API / Setting | Purpose |
|---|---|
| `powercfg /setactive SCHEME_MIN` | High performance power plan |
| `powercfg /setactive SCHEME_MAX` | Power saver plan |
| `powercfg /setactive SCHEME_BALANCED` | Balanced plan |
| `powercfg /setacvalueindex` | Set AC power setting |
| `powercfg /setdcvalueindex` | Set DC power setting |
| `PowerSetActiveOverlayScheme` | Set power overlay (Better Performance / Better Battery) |
| `PowerGetActiveOverlayScheme` | Query active overlay |

### Registry Keys for C-State / P-State Tuning

| Key | Path | Description |
|---|---|---|
| `ProcessorIdleThreshold` | `SYSTEM\CurrentControlSet\Control\Power` | C-state entry threshold |
| `IdleResiliencyPeriod` | `SYSTEM\CurrentControlSet\Control\Power` | Idle resiliency period |
| `CsEnabled` | `SYSTEM\CurrentControlSet\Control\Power` | Connected Standby enable |
| `HibernateEnabled` | `SYSTEM\CurrentControlSet\Control\Power` | Hibernate enable |
| `HibernateFastStartup` | `SYSTEM\CurrentControlSet\Control\Session Manager\Power` | Fast startup (hiberboot) |
| `CoreParkingDisabled` | via `powercfg` | Disable core parking |
| `MaxCores` | via `powercfg` | Maximum unparked cores |
| `MinCores` | via `powercfg` | Minimum unparked cores |
| `PerfEnableThreshold` | via `powercfg` | P-state increase threshold |
| `PerfDisableThreshold` | via `powercfg` | P-state decrease threshold |
| `PerfIncreaseTime` | via `powercfg` | P-state increase time (ms) |
| `PerfDecreaseTime` | via `powercfg` | P-state decrease time (ms) |
| `PerfIncreasePolicy` | via `powercfg` | P-state ramp policy (ideal/ideal-aggressive/single) |
| `PerfDecreasePolicy` | via `powercfg` | P-state down policy |
| `LatencyHint` | via `powercfg` | Latency hint timeout values |

### powercfg Subcommands for Core Parking

| Command | GUID | Description |
|---|---|---|
| `powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMAX 100` | Processor throttling max | Maximum processor frequency (%) |
| `powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMIN 5` | Processor throttling min | Minimum processor frequency (%) |
| `powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR CPMINCORES 100` | Minimum cores | Min unparked cores (%) |
| `powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR CPMAXCORES 100` | Maximum cores | Max unparked cores (%) |
| `powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSER IDLEDISABLE 1` | Idle disable | Disable C-states (1=disable) |
| `powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PERFAUTONOMOUS 0` | Autonomous mode | Hardware autonomous P-state control |

### Win32 APIs

| API | Purpose |
|---|---|
| `SetProcessInformation(..., PowerThrottling)` | Set power throttling for process (EcoQoS) |
| `PowerSetThrottleState` | Set throttle state |
| `CallNtPowerInformation` | Query/set power information |
| `SetWaitableTimerEx` | Set wake timer with C-state hint |

---

## 9. Key Findings for Optimizer

1. **C-states are fully controllable** — can be disabled entirely via `IDLEDISABLE=1` for latency-sensitive workloads, or tuned with latency hints for partial control
2. **Dynamic hint adjustment is enabled** (`PpmIdleDynamicHintAdjustEnabled = 1`) — kernel adapts idle state selection to workload patterns
3. **Coordinated idle mode is active** (`PpmIdleCoordinatedMode = 0x100`) — cores coordinate C-state entry for cluster-level power savings
4. **Core parking granularity is per-core** (`PpmParkGranularity = 1`) — finest-grain parking control available
5. **Soft parking is configured** (`PpmParkSoftParkingEnabled = 0x10000000`) — cores can be soft-parked (available but not scheduled)
6. **QoS-based performance is active** (`PpmPerfQosEnabled = 0xC0000100`) — QoS class policies influence P-state selection
7. **No turbo boost override** (`PpmPerfBoostAtGuaranteed = 0`) — PPM doesn't request boost above guaranteed frequency
8. **Conservative P-state stepping** (`PpmPerfSingleStepSize = 5%`) — frequency changes in small increments
9. **Interrupt steering from parked cores** (`PpmParkSteerInterrupts`) — interrupts are redirected away from parked cores
10. **PSCI firmware idle** (`PpmIdlePsciExecute`) — ARM64 uses standard PSCI calls for C-state transitions
11. **Idle duration expiration is 4ms** (`PpmIdleDurationExpirationTimeoutMs = 4`) — short idle residency before re-evaluation
12. **Max 5 unexpected interrupts** (`PpmIdleMaxUnexpectedInterrupt = 5`) before idle state re-evaluation
13. **LPI (Low Power Idle) tracking** is active with engaged state and capacity caps — indicates ACPI LPI states are registered
14. **Power plan overlay** (`PowerSetActiveOverlayScheme`) is the user-mode API for switching between performance/battery modes
15. **EcoQoS / Power Throttling** (`SetProcessInformation` with `PowerThrottling`) can tag background processes for reduced performance
