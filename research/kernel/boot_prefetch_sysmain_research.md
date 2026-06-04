# Boot / Prefetch / Superfetch (SysMain) — Windows 11 ARM64 Kernel Research

**Target**: Windows 11 Build 26100 (ARM64, Parallels VM)
**Primary Module**: `ntoskrnl.exe`
**Date**: 2026-06-04

---

## 1. Architecture Overview

Windows implements three layered prefetch/superfetch subsystems:

1. **Boot Prefetch (ReadyBoot)** — Prefetches disk I/O during boot using layout files
2. **Application Prefetch (.pf files)** — Tracks and prefetches pages for application launches
3. **Superfetch (SysMain)** — Proactively manages memory: prepopulates working sets, reprioritizes pages, power-boosts for app launches

The kernel component is primarily in `ntoskrnl.exe` with the `Pf` (Prefetch) / `PfSn` (Prefetch Superfetch New) prefix.

---

## 2. Kernel Entry Points

### 2.1 System Information Classes

Superfetch/Prefetch is controlled via `NtQuerySystemInformation` / `NtSetSystemInformation` with classes handled by:

- `PfSnQueryPrefetcherInformation` — Query prefetcher state
- `PfSnSetPrefetcherInformation` — Configure prefetcher behavior
- `PfQuerySuperfetchInformation` — Query SysMain state
- `PfSetSuperfetchInformation` — Configure SysMain

### 2.2 Memory Manager Prefetch APIs

| Function | Purpose |
|----------|---------|
| `MmPrefetchPages` | Prefetch pages by address list |
| `MmPrefetchPagesEx` | Extended prefetch with flags |
| `MmPrefetchVirtualMemory` | Prefetch virtual address ranges |
| `MmPrefetchVirtualAddresses` | Prefetch VA ranges (wrapper) |
| `MmPrefetchForCacheManager` | Cache manager directed prefetch |
| `MmConfigurePrefetchSeekThreshold` | Configure prefetch disk seek threshold |
| `MmWaitForCacheManagerPrefetch` | Wait for cache manager prefetch completion |
| `MmWaitMultipleForCacheManagerPrefetch` | Wait on multiple cache manager prefetches |
| `MiPrefetchVirtualMemory` | Internal VA prefetch implementation |
| `MiPrefetchDriverPages` | Prefetch driver pages at boot |
| `MiPrefetchControlArea` | Prefetch section control area pages |
| `MiPrefetchNormally` | Normal path prefetch |
| `MiPrefetchPagesViable` | Check if prefetch pages are viable |
| `MiPrefetchJumpVad` | Jump VAD for prefetch |
| `MiPrefetchRestOfCluster` | Prefetch rest of cluster |
| `MiPrefetchPreallocatePages` | Preallocate pages for prefetch |
| `MiPrefetchReleasePreallocatedPages` | Release preallocated pages |
| `MiUpdatePfnForPrefetchByPte` | Update PFN for prefetched PTE |
| `MiUpdatePrefetchPriority` | Update PFN prefetch priority |
| `MiSetInPagePrefetchPriority` | Set in-page prefetch priority |
| `MiPfCompletePrefetchIos` | Complete prefetch I/Os |
| `MiLeapPrefetch` | Leap-ahead prefetch |
| `MiReleasePrefetchGapPages` | Release gap pages in prefetch |

### 2.3 Thread-Level Prefetch Tracking

| Function | Purpose |
|----------|---------|
| `PsSetCurrentThreadPrefetching` | Mark current thread as doing prefetch |
| `PsIsCurrentThreadPrefetching` | Check if thread is in prefetch mode |

---

## 3. Superfetch (SysMain) Core — PfSn Functions

### 3.1 Initialization

| Function | Purpose |
|----------|---------|
| `PfInitializeSuperfetch` | Initialize Superfetch subsystem |
| `PfSnInitializePrefetcher` | Initialize prefetcher |
| `PfSnPrefetchCacheCtxInitialize` | Initialize prefetch cache context |
| `PfSnPrefetchCacheCtxStart` | Start prefetch cache |

### 3.2 Boot Prefetch

| Function | Purpose |
|----------|---------|
| `PfSnBeginBootPhase` | Begin a boot phase (for layout-based prefetch) |
| `MiPrefetchDriverPages` | Prefetch boot driver pages |

Boot prefetch uses trace files in `C:\Windows\Prefetch\` (e.g., `NTOSBOOT-B00DFAAD.pf`) to determine which pages to prefetch during boot phases.

### 3.3 Application Launch Prefetch

| Function | Purpose |
|----------|---------|
| `PfSnBeginAppLaunch` | Begin app launch scenario |
| `PfSnAppLaunchScenarioControl` | Control app launch scenario parameters |
| `PfSnBeginScenario` | Begin a prefetch scenario |
| `PfSnCheckScenario` | Check if scenario matches known pattern |
| `PfSnPrefetchScenario` | Execute prefetch for a scenario |
| `PfSnCalculateScenarioNameAndHash` | Calculate scenario identifier |
| `PfSnBuildScenarioEventDescriptors` | Build ETW descriptors for scenario |
| `PfSnScenarioAlloc` / `PfSnScenarioFree` | Scenario allocation |
| `PfpProcessScenarioPhase` | Process a scenario phase |
| `PfpScenCtxPrefetchAbortSet` | Set abort for scenario context |
| `PfpScenCtxPrefetchStateSet` | Set prefetch state for scenario context |
| `PfpScenCtxPrefetchWait` | Wait for scenario prefetch completion |

Scenario types:
- `PfSnAppLaunchScenarioTypePrefix` — Application launch prefix
- `PfSnActivityScenarioTypePrefix` — Activity-based prefix

### 3.4 Prefetch Data Pipeline

```
Page Fault → PfSnLogPageFault → PfSnTraceGetLogEntry → PfTLoggingWorker
    → PfSnLogPageFaultCommon → PfSnLogIdentifier → PfSnLogScenarioMeasures
```

Key logging/tracking functions:

| Function | Purpose |
|----------|---------|
| `PfSnLogPageFault` | Log a page fault for pattern tracking |
| `PfSnLogPageFaultCommon` | Common page fault logging |
| `PfSnLogIdentifier` | Log scenario identifier |
| `PfSnLogScenarioMeasures` | Log scenario performance measures |
| `PfSnLogScenarioDecision` | Log scenario decision (prefetch/not) |
| `PfSnLogForegroundProcess` | Log foreground process change |
| `PfSnLogStreamCreate` | Log stream creation |
| `PfSnLogStreamDelete` | Log stream deletion |
| `PfSnLogVolumeCreate` | Log volume creation |
| `PfSnLogOpenVolumesForPrefetch` | Log volume open for prefetch |
| `PfSnLogPrefetchMetadata` | Log prefetch metadata |
| `PfSnLogPrefetchSectionsStart` | Log section prefetch start |
| `PfSnLogPrefetchSectionsStop` | Log section prefetch stop |
| `PfSnLogGetReadListsStart` | Log read list generation start |
| `PfSnLogAsyncWorker` | Async logging worker |
| `PfSnLogEndTrace` | End trace logging |
| `PfSnCheckLoggingForThread` | Check if thread should be logged |
| `PfSnNameQueryWorker` | Name resolution worker |
| `PfLogFileDataAccess` | Log file data access |
| `PfLogEvent` | Generic log event |
| `PfHardFaultLog` | Hard fault logging |
| `PfFbLogEntryReserve` / `PfFbLogEntryComplete` | Feedback log entries |
| `PfpLogApplicationEvent` | Log application event |
| `PfpLogPageAccess` | Log page access pattern |
| `PfpLogEventRequest` | Log event request |
| `PfpStartLoggingHardFaultEvents` | Start hard fault event logging |
| `PfpPartitionIterateAndCheckCanAnyDoAccessLogging` | Check partition logging capability |

### 3.5 Prefetch Execution

| Function | Purpose |
|----------|---------|
| `PfpPrefetchFiles` | Prefetch files by path list |
| `PfpPrefetchFilesTrickle` | Trickle-prefetch files (low priority) |
| `PfpPrefetchEntireDirectory` | Prefetch entire directory contents |
| `PfpPrefetchRequest` | Issue prefetch request |
| `PfpPrefetchRequestPerform` | Execute prefetch request |
| `PfpPrefetchRequestPatchOffsets` | Patch offsets in prefetch request |
| `PfpPrefetchPrivatePages` | Prefetch private pages |
| `PfpPrefetchVolumesCleanup` | Cleanup volume prefetch handles |
| `PfSnPrefetchSections` | Prefetch section data |
| `PfSnPrefetchSectionsCleanup` | Cleanup section prefetch |
| `PfSnPrefetchScenario` | Prefetch entire scenario |
| `PfSnPrefetchFileMetadata` | Prefetch file metadata |
| `PfSnPrefetchMetadata` | Prefetch metadata |
| `PfSnGetPrefetchInstructions` | Get prefetch instructions from trace |
| `PfSnPrefetchCacheEntryGet` | Get prefetch cache entry |
| `PfSnPrefetchCacheEntryUpdate` | Update prefetch cache entry |
| `PfpFileBuildReadList` | Build file read list for prefetch |
| `PfpFileBuildReadSupport` | Build file read support structures |
| `PfpFileSetupObjectAttributes` | Setup file object attributes |
| `PfpFileCheckAttributesForPrefetch` | Check file attributes for prefetch eligibility |
| `PfpQueryFileExtentsRequest` | Query file extents for prefetch |
| `PfpCheckPrefetchAbort` | Check if prefetch should be aborted |
| `PfpVolumePrefetchMetadata` | Volume-level metadata prefetch |
| `PfSnOpenVolumesForPrefetch` | Open volumes for prefetch operations |
| `PfSnIsSectionPrefetchedAfterPhase` | Check if section was prefetched |
| `PfVolumeSupportedForPrefetch` | Check if volume supports prefetch |
| `PfPrefetchRequestVerify` | Verify prefetch request |
| `PfPrefetchRequestVerifyRanges` | Verify prefetch ranges |
| `PfPrefetchRequestVerifyPath` | Verify prefetch path |
| `PfPrefetchRequestPrepareForVerify` | Prepare request for verification |
| `PfSnPreallocatePrefetchHeader` | Preallocate prefetch header |
| `PfSnCleanupPrefetchHeader` | Cleanup prefetch header |
| `PfSnCleanupPrefetchSectionInfo` | Cleanup section info |

### 3.6 Async Prefetch Worker

| Function | Purpose |
|----------|---------|
| `PfSnAsyncPrefetchWorker` | Async prefetch worker thread |
| `PfSnAsyncPrefetchStep` | Single step of async prefetch |

### 3.7 Shared Prefetch

| Function | Purpose |
|----------|---------|
| `PfpPrefetchSharedInitialize` | Initialize shared prefetch |
| `PfpPrefetchSharedStart` | Start shared prefetch |
| `PfpPrefetchSharedCleanup` | Cleanup shared prefetch |
| `PfpPrefetchSharedDeref` | Dereference shared prefetch |
| `PfpPrefetchSharedConflictNotifyStart` | Shared conflict notification start |
| `PfpPrefetchSharedConflictNotifyEnd` | Shared conflict notification end |

---

## 4. Power Boost (Superfetch Priority Boost)

Superfetch implements "power boost" — a mechanism to quickly prioritize prefetch I/O when an app launch is detected:

| Function | Purpose |
|----------|---------|
| `PfSnPowerBoost` | Initiate power boost |
| `PfSnPowerBoostDpc` | DPC for power boost |
| `PfSnPowerBoostUpdate` | Update power boost state |
| `PfSnPowerBoostWorker` | Power boost worker thread |

Power boost raises I/O priority for prefetch operations during app launches to reduce cold-start latency.

---

## 5. PFN Priority Management

Superfetch uses PFN (Page Frame Number) priority to manage working sets:

| Function | Purpose |
|----------|---------|
| `MiUpdatePfnPriority` | Update PFN priority |
| `MiGetPfnPriority` | Get current PFN priority |
| `MiLockSetPfnPriority` | Set PFN priority under lock |
| `PfTSetTraceWorkerPriority` | Set trace worker priority |
| `PfTSetTracingPriority` | Set tracing priority |

PFN priorities (from memory management):
- 0 = Invalid
- 1 = Standby (lowest)
- 2-4 = Standby priority tiers
- 5 = Modified/ModifiedPageWriter
- 6 = Active (in working set)
- 7 = Superfetch priority boost

---

## 6. ETW / Trace Providers

| GUID/Provider | Description |
|---------------|-------------|
| `MS_Kernel_Prefetch_Provider` | Main kernel prefetch ETW provider |
| `GUID_SYSMAIN_SDB` | SysMain SDB (Smart Database) GUID |
| `POP_ETW_EVENT_SUPERFETCH_START` | Superfetch start event |
| `POP_ETW_EVENT_SUPERFETCH_STOP` | Superfetch stop event |
| `PfSnEvt_PrefetchMetadata_Start/Stop` | Prefetch metadata trace |
| `PfSnEvt_PrefetchSections_Start/Stop` | Section prefetch trace |
| `PfSnEvt_ScenarioDecision_Info` | Scenario decision trace |
| `PfSnEvt_SyncPrefetchingDone_Info` | Sync prefetch done trace |
| `WNF_SEB_APP_LAUNCH_PREFETCH` | WNF event for app launch prefetch |
| `PopDiagTraceSuperfetchNotification` | Power diag trace for superfetch |
| `Kd_PREFETCHER_Mask` | KD debug mask for prefetcher |

---

## 7. Feature Flags

| Feature | Description |
|---------|-------------|
| `Feature_PrefetchQueueWorkerProactivelyForProcessExit` | Queue prefetch worker proactively when process exits (prefetch next app) |

---

## 8. Key Globals

| Global | Purpose |
|--------|---------|
| `ExpPrefetchPushLock` | Push lock protecting prefetch operations |
| `PfSnAppLaunchScenarioTypePrefix` | String prefix for app launch scenarios |
| `PfSnActivityScenarioTypePrefix` | String prefix for activity scenarios |
| `Kd_PREFETCHER_Mask` | Debug print mask for prefetcher |
| `CcNumberAsyncReadPrefetches` | Cache manager async read prefetch counter |

---

## 9. Virtual Memory Prefetch (VM Prefetch)

| Function | Purpose |
|----------|---------|
| `VmPrefetchVirtualAddresses` | VM-level address prefetch |
| `VmpPrefetchVirtualAddresses` | Internal VM address prefetch |
| `VmpPrefetchForVirtualFault` | Prefetch on virtual fault |
| `VmpPrefetchWorker` | VM prefetch worker thread |

---

## 10. Cache Manager Integration

| Function | Purpose |
|----------|---------|
| `CcAsyncReadPrefetch` | Cache manager async read prefetch |
| `CcAsyncReadPrefetch$fin$0` | Exception handler for async prefetch |
| `CmSiPrefetchVirtualMemoryRange` | Registry CM prefetch VA range |
| `RtlPrefetchMemoryNonTemporal` | Non-temporal prefetch (SSE/ARM NEON) |
| `RtlpHpLfhSubsegmentPrefetch` | Heap LFH subsegment prefetch |
| `RtlpHpLfhSubsegmentPrefetchRange` | Heap subsegment range prefetch |
| `SmmupV2QcInitDefaultPrefetchSettings` | SMMU prefetch settings initialization |

---

## 11. Verifier Support

- `VerifierMmPrefetchPages` — Driver Verifier hook for MmPrefetchPages

---

## 12. Key Findings for Optimizer

1. **Prefetch Files Location**: `%SystemRoot%\Prefetch\*.pf` — Layout files for boot and app launch traces. Optimizer can clean stale entries, optimize trace retention.

2. **Boot Phase Prefetch**: `PfSnBeginBootPhase` marks boot phases for layout-based prefetch. Reducing boot prefetch trace size can speed up boot on slow disks.

3. **Power Boost for App Launches**: `PfSnPowerBoost` mechanism elevates I/O priority during app launches. An optimizer could trigger power boost for specific apps.

4. **PFN Priority Tuning**: Superfetch uses PFN priorities to manage working set prepopulation. An optimizer could adjust working set trimming behavior.

5. **Scenario Tracking**: `PfSnBeginAppLaunch` / `PfSnBeginScenario` track app launches and activities. The prefetch database in `C:\Windows\Prefetch\` is the persistent store.

6. **Foreground Process Tracking**: `PfLogForegroundProcess` logs foreground process changes — SysMain uses this to deprioritize background app pages.

7. **Cache Manager Integration**: `CcAsyncReadPrefetch` enables async read-ahead. An optimizer could tune cache manager prefetch aggressiveness.

8. **Trickle Prefetch**: `PfpPrefetchFilesTrickle` does low-priority background prefetch — can be throttled on battery/power-saver.

9. **Heap Prefetch**: `RtlpHpLfhSubsegmentPrefetch` prefetches heap subsegments — part of segment pool (Segment Pool / V2 heap).

10. **SysMain Service Control**: SysMain (formerly Superfetch) service (`sysmain.dll` in svchost) calls into kernel via `PfSnSetPrefetcherInformation` / `PfSetSuperfetchInformation`. Disabling SysMain service disables proactive memory management but keeps basic prefetch.

11. **VM Prefetch Worker**: `VmpPrefetchWorker` handles VM-level prefetch operations — virtualization-based prefetch for Hyper-V scenarios.

12. **Prefetch Abort**: `PfpCheckPrefetchAbort` allows aborting in-progress prefetch if higher-priority I/O arrives — important for system responsiveness.

---

## 13. Registry Keys

```
HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters
  - EnablePrefetcher                  (REG_DWORD: 0=Disabled, 1=App, 2=Boot, 3=Both)
  - EnableSuperfetch                  (REG_DWORD: 0=Disabled, 1=App, 2=Boot, 3=Both)
  - ScCenterDbFile                    (REG_SZ: path to scenario center database)
  - ScHostDbFile                      (REG_SZ: path to scenario host database)

HKLM\SYSTEM\CurrentControlSet\Services\SysMain
  - Start                             (REG_DWORD: 2=Auto, 3=Manual, 4=Disabled)
```

---

## 14. User-Mode APIs

| API | Purpose |
|-----|---------|
| `PrefetchVirtualMemory` | User-mode prefetch of virtual address ranges |
| `NtQuerySystemInformation(SystemSuperfetchInformation)` | Query SysMain state |
| `NtSetSystemInformation(SystemSuperfetchInformation)` | Control SysMain |
| `RtlPrefetchMemoryNonTemporal` | Non-temporal memory prefetch |
| `powercfg /query` | Query power scheme (affects SysMain behavior) |

---

## 15. Service Architecture

```
svchost.exe -k LocalSystemNetworkRestricted
  └── sysmain.dll (SysMain service)
       ├── PfSnSetPrefetcherInformation  (configure prefetch)
       ├── PfSetSuperfetchInformation     (configure SysMain)
       ├── PfSnQueryPrefetcherInformation (query state)
       └── PfQuerySuperfetchInformation   (query SysMain state)
```

The SysMain service:
1. Monitors page fault patterns via `PfSnLogPageFault`
2. Builds scenario databases (app launch traces)
3. Proactively prefetches pages via `PfpPrefetchFiles` / `PfSnPrefetchScenario`
4. Manages PFN priorities for working set optimization
5. Uses Power Boost to accelerate prefetch during app launches
