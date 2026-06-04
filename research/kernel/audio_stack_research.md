# Audio Stack — Windows 11 ARM64 Kernel Research

**Target**: Windows 11 Build 26100 (ARM64, Parallels VM)
**Primary Modules**: `ks.sys`, `portcls.sys`, `mmcss.sys`, `drmk.sys`, `HDAudBus.sys`, `HdAudio.sys`
**Date**: 2026-06-04

---

## 1. Architecture Overview

The Windows audio stack spans kernel and user mode:

1. **HD Audio Hardware** — Intel HD Audio / HDA bus controller
2. **HD Audio Bus Driver** (`HDAudBus.sys`) — Bus enumerator for HD Audio codecs
3. **HD Audio Class Driver** (`HdAudio.sys`) — UAA (Universal Audio Architecture) class driver
4. **Kernel Streaming (KS)** (`ks.sys`) — Core streaming framework for audio/video
5. **Port Class Driver** (`portcls.sys`) — Audio adapter framework (wave, topology, MIDI ports)
6. **DRM Kernel** (`drmk.sys`) — Digital Rights Management for protected audio content
7. **MMCSS** (`mmcss.sys`) — Multimedia Class Scheduler Service (kernel scheduler component)
8. **ksthunk** (`ksthunk.sys`) — WoW64 thunk layer for 32-bit KS clients
9. **Software Enumerator** (`swenum.sys`) — Software device enumerator for KS filters
10. **Audio Device Graph** (`audiodg.exe`) — User-mode audio engine host (ISVs, APOs, effects)

---

## 2. Module Inventory (This VM)

### 2.1 Kernel-Mode Modules

| Module | Base Address | Size | Purpose |
|--------|-------------|------|---------|
| `HDAudBus.sys` | fffff800`6c410000 | ~0x2F000 | HD Audio Bus driver |
| `HdAudio.sys` | fffff800`6cc00000 | ~0x78000 | HD Audio UAA class driver |
| `portcls.sys` | fffff800`6c450000 | ~0x6E000 | Port Class driver framework |
| `drmk.sys` | fffff800`6c4d0000 | ~0x1F000 | DRM kernel support |
| `ks.sys` | fffff800`6c500000 | ~0x82000 | Kernel Streaming framework |
| `ksthunk.sys` | fffff800`6cc80000 | ~0x11000 | KS WoW64 thunk |
| `swenum.sys` | fffff800`6cbf0000 | ~0xC000 | Software device enumerator |
| `mmcss.sys` | fffff800`6e0f0000 | ~0x13000 | Multimedia Class Scheduler |

### 2.2 User-Mode Modules (audiodg.exe process)

| Module | Purpose |
|--------|---------|
| `audiodg.exe` | Audio Device Graph — hosts audio engines, APOs, effects |
| `audioeng.dll` | Audio engine (mixing, format conversion, effects) |
| `AudioSes.dll` | Audio session management |
| `WMALFXGFXDSP.dll` | Windows Media Audio LFX/GFX DSP effects |
| `RTWorkQ.dll` | Real-time work queue (thread pool for audio processing) |
| `MMDevAPI.dll` | Multimedia Device API (endpoint enumeration) |
| `AVRT.dll` | Audio/Video Runtime (MMCSS user-mode wrapper) |
| `audiokse.dll` | Audio KS proxy (user-mode KS client) |
| `Windows.Media.Devices.dll` | WinRT media device API |

**audiodg.exe Process**: `PROCESS ffff988bff35c100` — runs as a protected process hosting audio engines and APOs (Audio Processing Objects).

---

## 3. Kernel Streaming (KS) Framework — ks.sys

### 3.1 Device Management

| Function | Purpose |
|----------|---------|
| `KsCreateDevice` | Create KS device object |
| `KsInitializeDevice` | Initialize KS device |
| `KsTerminateDevice` | Terminate KS device |
| `KsDeviceGetFirstChildFilterFactory` | Enumerate filter factories |
| `KsDeviceGetNextSiblingFilterFactory` | Next filter factory sibling |
| `KsFilterFactoryGetFirstChildFilter` | Get first filter from factory |
| `KsFilterFactoryGetNextSiblingFilter` | Next filter sibling |
| `KsFilterFactoryAddCreateItem` | Add create item to factory |
| `KsFilterFactorySetDeviceClassesState` | Set device class state |
| `KsFilterFactoryUpdateCacheData` | Update filter factory cache |
| `KsGetDevice` | Get KS device from object |
| `KsGetFilterFromIfileObject` | Get filter from file object |
| `KsGetPinFromIfileObject` | Get pin from file object |

### 3.2 Filter Management

| Function | Purpose |
|----------|---------|
| `KsCreateFilterFactory` | Create filter factory |
| `KsFilterCreateNode` | Create node on filter |
| `KsFilterCreatePinFactory` | Create pin factory on filter |
| `KsFilterGetFirstChildPin` | Get first pin on filter |
| `KsFilterGetNextSiblingPin` | Next pin sibling |
| `KsFilterGetOuterUnknown` | Get outer IUnknown |
| `KsFilterRegisterPowerCallbacks` | Register filter power callbacks |
| `KsFilterRegisterAssetCallbacks` | Register asset callbacks |
| `KsFilterTryAcquireControl` | Try acquire filter control mutex |
| `KsFilterAcquireControl` | Acquire filter control mutex |
| `KsFilterReleaseControl` | Release filter control mutex |
| `KsFilterAddTopologyConnections` | Add topology connections |
| `KsFilterGetAndGate` | Get filter AND gate |

### 3.3 Pin Management

| Function | Purpose |
|----------|---------|
| `KsPinGetConnectedPinFileObject` | Get connected pin file object |
| `KsPinGetConnectedPinInterface` | Get connected pin interface |
| `KsPinGetConnectedFilterInterface` | Get connected filter interface |
| `KsPinGetReferenceClockInterface` | Get pin reference clock |
| `KsPinAttachAndGate` | Attach AND gate to pin |
| `KsPinAttachOrGate` | Attach OR gate to pin |
| `KsPinAcquireProcessingMutex` | Acquire pin processing mutex |
| `KsPinReleaseProcessingMutex` | Release pin processing mutex |
| `KsPinAttemptProcessing` | Attempt pin processing |
| `KsPinGetCopyRelationships` | Get pin copy relationships |
| `KsPinRegisterFrameReturnCallback` | Register frame return callback |
| `KsPinRegisterIrpCompletionCallback` | Register IRP completion callback |
| `KsPinRegisterHandshakeCallback` | Register handshake callback |
| `KsPinRegisterPowerCallbacks` | Register pin power callbacks |
| `KsPinSetPinClockTime` | Set pin clock time |
| `KsPinGetFirstCloneStreamPointer` | Get first clone stream pointer |
| `KsPinGetNextCloneStreamPointer` | Get next clone stream pointer |

### 3.4 Stream Pointers

| Function | Purpose |
|----------|---------|
| `KsStreamPointerGetNextClone` | Get next clone stream pointer |
| `KsStreamPointerClone` | Clone a stream pointer |
| `KsStreamPointerDelete` | Delete a stream pointer |
| `KsStreamPointerAdvance` | Advance stream pointer |
| `KsStreamPointerAdvanceOffsets` | Advance stream pointer offsets |
| `KsStreamPointerGetIRP` | Get IRP from stream pointer |
| `KsStreamPointerGetMdl` | Get MDL from stream pointer |
| `KsStreamPointerGetStreamHeader` | Get stream header |
| `KsStreamPointerLock` | Lock stream pointer |
| `KsStreamPointerUnlock` | Unlock stream pointer |
| `KsStreamPointerSetStatusCode` | Set status code |
| `KsStreamPointerTimestamp` | Set timestamp |
| `KsStreamPointerGetInlineIrp` | Get inline IRP |

### 3.5 Property / Method / Event Handling

| Function | Purpose |
|----------|---------|
| `KsPropertyHandler` | Handle KS property request |
| `KsPropertyHandlerWithAllocator` | Property handler with allocator |
| `KsMethodHandler` | Handle KS method request |
| `KsMethodHandlerWithAllocator` | Method handler with allocator |
| `KsEventHandler` | Handle KS event request |
| `KsEventHandlerAdd` | Add event handler |
| `KsEventHandlerRemove` | Remove event handler |
| `KsEnableEvent` | Enable KS event |
| `KsEnableEventWithAllocator` | Enable event with allocator |
| `KsBasicTicker` | Basic ticker for event notification |
| `KsGenerateEvent` | Generate KS event |
| `KsGenerateEventList` | Generate event from list |
| `KsSynchronousIoControlDevice` | Synchronous IO control |
| `KsDiscardEvent` | Discard KS event |

### 3.6 Object Management

| Function | Purpose |
|----------|---------|
| `KsAllocateObjectCreateItem` | Allocate create item |
| `KsFreeObjectCreateItem` | Free create item |
| `KsAllocateObjectBag` | Allocate object bag |
| `KsFreeObjectBag` | Free object bag |
| `KsAddObjectCreateItemToDeviceHeader` | Add create item to device header |
| `KsAddObjectCreateItemToObjectHeader` | Add create item to object header |
| `KsCopyObjectBag` | Copy object bag |
| `KsMergeObjectBag` | Merge object bags |
| `KsMoveIrpsOnCancelable` | Move IRPs on cancelable list |
| `KsRemoveIrpsFromCancelable` | Remove IRPs from cancelable |
| `KsUnserializeObjectBackward` | Unserialize backward |

### 3.7 Clock / Timer Management

| Function | Purpose |
|----------|---------|
| `KsAllocateDefaultClock` | Allocate default KS clock |
| `KsFreeDefaultClock` | Free default clock |
| `KsCreateDefaultClock` | Create default clock |
| `KsCreateDefaultSecurity` | Create default security |
| `KsGetDefaultClockTime` | Get default clock time |
| `KsSetDefaultClockTime` | Set default clock time |
| `KsGetExternalTime` | Get external time |
| `KsGetTimeInterval` | Get time interval |
| `KsGetTimeoutPkt` | Get timeout packet |

### 3.8 Bus / Enumeration

| Function | Purpose |
|----------|---------|
| `KsCreateBusEnumObject` | Create bus enumerator object |
| `KsGetBusEnumIdentifier` | Get bus enumerator ID |
| `KsGetBusEnumParentFDO` | Get parent FDO |
| `KsGetBusEnumPnpDeviceObject` | Get PnP device object |
| `KsIsBusEnumChildDevice` | Check if bus enum child |
| `KsInstallBusEnumInterface` | Install bus enum interface |
| `KsRemoveBusEnumInterface` | Remove bus enum interface |
| `KsServiceBusEnumPnpRequest` | Service bus enum PnP request |
| `KsServiceBusEnumCreateRequest` | Service bus enum create request |
| `KsLoadResource` | Load KS resource |
| `KsMapModuleName` | Map module name |
| `KsOpenDevice` | Open KS device |

### 3.9 Verifier Support

| Function | Purpose |
|----------|---------|
| `KsVerifyCallbacks` | Verify callback setup |
| `KsVerifyAndAllocateWaitSource` | Verify and allocate wait source |
| `KsNullDriverUnload` | Null driver unload (verifier) |

### 3.10 Thermal / Power

| Function | Purpose |
|----------|---------|
| `KsFilterRegisterPowerCallbacks` | Register filter power callbacks |
| `KsPinRegisterPowerCallbacks` | Register pin power callbacks |
| `KsDeviceThermalNotification` | Device thermal notification |

---

## 4. Port Class Driver — portcls.sys

### 4.1 Adapter Initialization

| Function | Purpose |
|----------|---------|
| `PcInitializeAdapterDriver` | Initialize audio adapter driver |
| `PcAddAdapterDevice` | Add adapter device to stack |
| `PcNewPort` | Create new port driver instance |
| `PcNewDmaChannel` | Create new DMA channel |
| `PcNewMiniport` | Create new miniport driver |
| `PcNewServiceGroup` | Create new service group |
| `PcNewRegistryKey` | Create registry key for adapter |
| `PcNewResourceList` | Create new resource list |
| `PcNewResourceSublist` | Create resource sub-list |
| `PcGetContentRights` | Get content rights (DRM) |

### 4.2 Physical Connections

| Function | Purpose |
|----------|---------|
| `PcRegisterPhysicalConnection` | Register physical connection between pins |
| `PcRegisterPhysicalConnectionFromExternal` | Register external-to-filter connection |
| `PcRegisterPhysicalConnectionToExternal` | Register filter-to-external connection |
| `PcRegisterPhysicalConnectionWithExternal` | Register bidirectional external connection |
| `PcGetExternalDeviceCount` | Get external device count |

### 4.3 Stream Resource Management

| Function | Purpose |
|----------|---------|
| `PcStreamResourceType` | Stream resource type identifier |
| `PcAddStreamResource` | Add stream resource (interrupt/thread) |
| `PcRemoveStreamResource` | Remove stream resource |
| `PcCreateStreamResourceRegister` | Register stream resource |
| `PcCreateStreamResourceSet` | Set stream resource properties |

Stream resources are the core mechanism for audio glitch resilience — they register interrupts and threads with the audio stack so the scheduler can prioritize them appropriately.

### 4.4 Power Management

| Function | Purpose |
|----------|---------|
| `PcRegisterAdapterPowerManagement` | Register adapter power management |
| `PcRequestNewPowerState` | Request new power state |
| `PcPowerFxActivate` | Power FX activate |
| `PcPowerFxDeactivate` | Power FX deactivate |
| `PcPowerFxNotify` | Power FX notification |
| `PcPowerFxApplyGpoConfig` | Apply GPO power configuration |

### 4.5 DRM (Digital Rights Management)

| Function | Purpose |
|----------|---------|
| `PcForwardContentToInterface` | Forward DRM content to interface |
| `PcForwardContentToDeviceObject` | Forward DRM content to device |
| `PcForwardContentToFileObject` | Forward DRM content to file |
| `PcCreateContentMixed` | Create mixed DRM content ID |
| `PcDestroyContent` | Destroy DRM content |
| `PcGetContentRights` | Get DRM content rights |

### 4.6 Property / I/O

| Function | Purpose |
|----------|---------|
| `PcCompleteIrp` | Complete IRP |
| `PcCompletePendingPropertyRequest` | Complete pending property |
| `PcDispatchIrp` | Dispatch IRP |
| `PcHandlePropertyWithAdapter` | Handle property with adapter |
| `PcHandlePropertyWithNodes` | Handle property with topology nodes |
| `PcCallPropertyCommon` | Common property call |
| `PcGetDeviceProperty` | Get device property |
| `PcWaitForSingleObject` | Wait for sync object |

### 4.7 WMI

| Function | Purpose |
|----------|---------|
| `PcWmiRegister` | Register WMI data provider |
| `PcWmiUnregister` | Unregister WMI provider |
| `PcWmiGetInstance` | Get WMI instance |
| `PcWmiSetInstance` | Set WMI instance |
| `PcWmiSetItem` | Set WMI item |

### 4.8 Verifier Support

| Function | Purpose |
|----------|---------|
| `PcVerifyAndAllocateObject` | Verify and allocate PortCLS object |
| `PcVerifyObjectType` | Verify object type |

---

## 5. HD Audio Bus / Class Driver

### 5.1 HDAudBus.sys

HD Audio Bus driver enumerates codecs on the HD Audio controller bus. It provides:
- Bus enumeration for audio codecs
- Command/response communication with codecs
- DMA engine management for audio streams
- Interrupt handling for stream status

### 5.2 HdAudio.sys

HD Audio UAA class driver provides:
- Wave and topology port instantiation via PortCLS
- Codec-specific pin configuration
- Audio endpoint enumeration
- Format negotiation (sample rates, bit depths, channels)
- Power management for codecs

---

## 6. DRM Kernel — drmk.sys

| Function | Purpose |
|----------|---------|
| `DrmAddContentHandlers` | Add DRM content handlers |
| `DrmCreateContentMixed` | Create mixed content ID |
| `DrmDestroyContent` | Destroy content |
| `DrmForwardContentToDeviceObject` | Forward to device |
| `DrmForwardContentToFileObject` | Forward to file |
| `DrmForwardContentToInterface` | Forward to interface |
| `DrmGetContentRights` | Get content rights |

DRM kernel ensures protected audio content (DRM-encrypted) flows only through trusted components. Each audio driver in the chain must handle DRM content forwarding via `PcForwardContent*` or `DrmForwardContent*`.

---

## 7. MMCSS Scheduler Internals — mmcss.sys

MMCSS (Multimedia Class Scheduler Service) is the kernel scheduler component that ensures multimedia threads get appropriate CPU scheduling. It manages thread priorities, deadlines, and system responsiveness.

### 7.1 Scheduler Core

| Function | Purpose |
|----------|---------|
| `CiSchedulerInitialize` | Initialize scheduler instance |
| `CiSchedulerTerminate` | Terminate scheduler |
| `CiSchedulerThreadFunction` | Main scheduler thread loop |
| `CiSchedulerSleep` | Put scheduler to sleep |
| `CiSchedulerPoke` | Wake up scheduler |
| `CiSchedulerWait` | Wait for scheduler event |
| `CiSchedulerSetPriority` | Set scheduler priority |
| `CiSchedulerUpdateTimer` | Update scheduler timer |
| `CiSchedulerCommitPriority` | Commit priority changes |
| `CiSchedulerProcessDeadlines` | Process thread deadlines |
| `CiSchedulerSetMultimediaMode` | Set multimedia mode |
| `CiSchedulerDeepSleep` | Enter deep sleep (power saving) |
| `CiSchedulerUpdateTaskIndexPriorities` | Update all task index priorities |
| `CiSchedulerUpdateSuspendState` | Update suspend state |
| `CiSchedulerCancelTaskIndexYield` | Cancel task index yield |
| `CiSchedulerRefreshTaskIndexQosProperties` | Refresh QoS properties |
| `CiSchedulerRemoveTaskIndex` | Remove task index |
| `CiSchedulerRemoveDeadline` | Remove deadline |
| `CiSchedulerRemoveThread` | Remove thread from scheduler |
| `CiSchedulerAddThread` | Add thread to scheduler |
| `CiSchedulerInitialize` | Initialize scheduler |
| `CiSchedulerTerminate` | Terminate scheduler |
| `CiSchedulerIdleCycleBitMask` | Idle cycle detection |

### 7.2 Thread Management

| Function | Purpose |
|----------|---------|
| `CiThreadCreate` | Create scheduler thread |
| `CiThreadCleanup` | Cleanup thread |
| `CiThreadSetRelativePriority` | Set thread relative priority |
| `CiThreadUpdatePriorities` | Update thread priorities |
| `CiThreadNotification` | Thread notification callback |
| `CiThreadLocate` | Locate thread in tree |
| `CiThreadInsertInTree` | Insert thread in priority tree |
| `CiThreadRemoveFromTree` | Remove thread from tree |
| `CiThreadIncrementScheduledCount` | Increment scheduled count |
| `CiThreadDecrementScheduledCount` | Decrement scheduled count |
| `CiThreadReferenceTaskIndex` | Reference task index |
| `CiThreadDereferenceTaskIndex` | Dereference task index |
| `CiThreadJoin` | Thread join |
| `CiThreadLeave` | Thread leave |

### 7.3 Process Management

| Function | Purpose |
|----------|---------|
| `CiProcessCreate` | Create process context |
| `CiProcessDereference` | Dereference process |
| `CiProcessLocate` | Locate process |
| `CiProcessAddThread` | Add thread to process |
| `CiProcessRemoveThread` | Remove thread from process |
| `CiProcessSuspend` | Suspend process |
| `CiProcessComparer` | Process comparison function |
| `CiProcessNotification` | Process notification callback |

### 7.4 Task Index Management

| Function | Purpose |
|----------|---------|
| `CiTaskAllocate` | Allocate task |
| `CiTaskLocate` | Locate task |
| `CiTaskIndexCreate` | Create task index |
| `CiTaskIndexRemove` | Remove task index |
| `CiTaskIndexLocate` | Locate task index |
| `CiTaskIndexReference` | Reference task index |
| `CiTaskIndexDereference` | Dereference task index |
| `CiTaskIndexGetNewIndexValue` | Get new index value |
| `CiTaskIndexYield` | Yield task index |
| `CiTaskDump` | Dump task info |

### 7.5 Configuration

| Function | Purpose |
|----------|---------|
| `CiConfigInitialize` | Initialize config from defaults |
| `CiConfigInitializeFromRegistry` | Load config from registry |
| `CiConfigReadDWORD` | Read DWORD from registry |
| `CiConfigQueryValue` | Query config value |
| `CiConfigQueryTaskFromRegistry` | Query task configuration |
| `CiConfigTaskPolicy` | Get task policy |
| `CiConfigTaskValues` | Get task values |

### 7.6 System-Level

| Function | Purpose |
|----------|---------|
| `CiSystemInitialize` | Initialize MMCSS system |
| `CiSystemTerminate` | Terminate MMCSS system |
| `CiSystemAcquireSpinLock` | Acquire system spin lock |
| `CiSystemAcquirePushLock` | Acquire system push lock |
| `CiSystemUpdateMediaBufferingState` | Update media buffering state |
| `CiSystemUpdateThreadTag` | Update thread tag |
| `CiSystemResponsiveness` | Get/set system responsiveness |

### 7.7 Network Throttling

| Function | Purpose |
|----------|---------|
| `CiNdisThrottle` | NDIS network throttling |
| `CiNdisCleanupThrottle` | Cleanup throttle state |
| `CiNdisOpenDevice` | Open NDIS device for throttle |
| `CiNdisUpdateThrottleState` | Update throttle state |

MMCSS throttles network I/O during multimedia playback to prevent network-induced audio glitches. `CiNdisThrottle` communicates with NDIS to reduce receive throughput during active audio/video sessions.

### 7.8 Logging / Diagnostics

| Function | Purpose |
|----------|---------|
| `CiLogTurboEngaged` | Log turbo mode engaged |
| `CiLogThreadBuffering` | Log thread buffering state |
| `CiLogTaskIndexYield` | Log task index yield |
| `CiLogThreadJoin` | Log thread join |
| `CiLogThreadLeave` | Log thread leave |
| `CiLogSchedulerEvent` | Log scheduler event |
| `CiLogSchedulerSleep` | Log scheduler sleep |
| `CiLogSchedulerWakeup` | Log scheduler wakeup |

### 7.9 Key Scheduler Globals

| Global | Purpose |
|--------|---------|
| `CiSchedulerPeriod` | Scheduler tick period (ms) |
| `CiSchedulerTimerResolution` | Timer resolution for scheduler |
| `CiSchedulerLazyModeTimeout` | Timeout before entering lazy mode |
| `CiSchedulerDisallowLazyMode` | Flag to disallow lazy mode |
| `CiSystemResponsiveness` | System responsiveness value (0-100) |
| `CiNetworkThrottlingIndex` | Network throttle index |
| `CiScheduledThreadCount` | Count of scheduled threads |
| `CiTotalThreads` | Total managed threads |
| `CiMaxThreadsPerProcess` | Max MMCSS threads per process |
| `CiMaxThreadsTotal` | Max total MMCSS threads |
| `CiSchedulerInLazyMode` | Whether scheduler is in lazy mode |
| `CiSchedulerIdleCycleBitMask` | Idle cycle detection mask |
| `CiPotentiallyStarvedProcessors` | Processors at risk of starvation |
| `CiThreadsMovedUp` | Threads moved up in priority |
| `CiCurrentMediaBufferingState` | Current media buffering state |
| `CiTotalTasksBuffering` | Tasks currently buffering |
| `CiTotalTasksDeadlineExpired` | Tasks with expired deadlines |

### 7.10 Registry Configuration Value Names

| Value Name | Purpose |
|------------|---------|
| `CiSchedulerPeriodName` | Registry name for scheduler period |
| `CiSchedulerTimerResolutionName` | Registry name for timer resolution |
| `CiLazyModeTimeoutName` | Registry name for lazy mode timeout |
| `CiLazyModeDisallowedName` | Registry name for lazy mode disallow |
| `CiSystemResponsivenessName` | Registry name for responsiveness |
| `CiNetworkThrottlingIndexName` | Registry name for network throttle |
| `CiMaxThreadsPerProcessName` | Registry name for max threads/process |
| `CiMaxThreadsTotalName` | Registry name for max threads total |
| `PriorityValues` | Priority value table |
| `BackgroundPriorityValues` | Background priority values |
| `BackgroundOnlyValues` | Background-only values |
| `AffinityValues` | Affinity configuration |
| `ClockRateValues` | Clock rate values |
| `LatencySensitiveValues` | Latency sensitivity values |
| `SchedulingCategoryValues` | Scheduling category values |
| `PriorityWhenYieldedValues` | Priority when yielded |

---

## 8. ksthunk — WoW64 Support

`ksthunk.sys` provides thunking for 32-bit applications accessing 64-bit KS drivers. It translates KS property/method/event calls between 32-bit and 64-bit structures.

---

## 9. Software Enumerator — swenum.sys

`swenum.sys` is the software device enumerator that creates KS filter instances for software audio devices (software synthesizers, virtual audio cables, etc.).

---

## 10. Key Findings for Optimizer

1. **MMCSS Thread Priority**: `CiSchedulerSetPriority` and `CiThreadSetRelativePriority` control multimedia thread priorities. The optimizer can ensure audio threads get proper MMCSS scheduling via `AvSetMmThreadPriority`.

2. **Network Throttling During Audio**: `CiNdisThrottle` / `CiNetworkThrottlingIndex` control network receive throttling during multimedia playback. This prevents audio glitches but can reduce network throughput. The optimizer can tune `CiNetworkThrottlingIndex` for better network performance when audio glitches are acceptable.

3. **System Responsiveness**: `CiSystemResponsiveness` controls the percentage of CPU time reserved for non-multimedia tasks (default ~20%). Lowering this gives more CPU to multimedia but can make the system feel sluggish.

4. **Lazy Mode**: `CiSchedulerLazyModeTimeout` controls when MMCSS enters low-power lazy mode. Shorter timeouts save power but increase audio startup latency.

5. **Stream Resource Management**: `PcAddStreamResource` registers audio interrupts and threads with the scheduler. This is critical for glitch-free audio — the optimizer should ensure audio drivers properly register stream resources.

6. **Power FX**: `PcPowerFxActivate` / `PcPowerFxDeactivate` manage audio power states. Power FX allows fine-grained power management of audio components.

7. **Audio Device Graph (audiodg.exe)**: Hosts all audio processing (APOs, effects, mixing). High CPU usage in audiodg.exe indicates expensive audio effects. Disabling audio enhancements reduces audiodg.exe CPU usage.

8. **Max Threads Limits**: `CiMaxThreadsPerProcess` and `CiMaxThreadsTotal` limit MMCSS-managed threads. Increasing these can help apps with many audio threads but risks priority inversion.

9. **Deadline Scheduling**: `CiSchedulerProcessDeadlines` manages real-time deadlines for audio threads. Threads with expired deadlines (`CiTotalTasksDeadlineExpired`) indicate CPU overload.

10. **DMA Channel Management**: `PcNewDmaChannel` allocates DMA channels for audio streaming. DMA latency directly affects audio buffer size and latency.

11. **DRM Forwarding Chain**: Protected audio must flow through `PcForwardContent*` / `DrmForwardContent*` chain. Breaking this chain (e.g., custom audio drivers) can prevent DRM content playback.

12. **Thermal Throttling**: `KsDeviceThermalNotification` notifies KS devices of thermal conditions. Audio devices may reduce quality/sample rate under thermal pressure.

13. **Buffering State Tracking**: `CiCurrentMediaBufferingState` and `CiTotalTasksBuffering` track whether media is buffering — MMCSS boosts priority during buffering to reduce startup latency.

---

## 11. Registry Keys

```
HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile
  - SystemResponsiveness                (REG_DWORD: 0-100, % CPU for non-MM tasks)
  - NetworkThrottlingIndex              (REG_DWORD: network throttle value)

HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile\Tasks\*
  - (Per-task MMCSS profiles: Audio, Games, Pro Audio, etc.)
  - Affinity                            (REG_DWORD: CPU affinity)
  - Background Only                     (REG_DWORD: background-only mode)
  - Clock Rate                          (REG_DWORD: clock rate)
  - GPU Priority                        (REG_DWORD: GPU priority)
  - Latency Sensitive                   (REG_DWORD: latency sensitivity)
  - Priority                            (REG_DWORD: thread priority)
  - Scheduling Category                 (REG_SZ: "High", "Medium", "Low")
  - SFIO Priority                       (REG_SZ: "High", "Normal", "Low")

HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices
  - (Audio endpoint device configuration)

HKLM\SYSTEM\CurrentControlSet\Services\Audiosrv
  - Start                               (REG_DWORD: 2=Auto, 3=Manual, 4=Disabled)

HKLM\SYSTEM\CurrentControlSet\Services\MMCSS
  - Start                               (REG_DWORD: 2=Auto, 3=Manual, 4=Disabled)
```

---

## 12. User-Mode APIs

| API | Purpose |
|-----|---------|
| `AvSetMmThreadPriority` | Set MMCSS thread priority |
| `AvRevertMmThreadCharacteristics` | Remove MMCSS scheduling |
| `AvSetMmThreadCharacteristics` | Set thread MMCSS characteristics |
| `AvSetMmMaxThreadCharacteristics` | Set max thread characteristics |
| `AvGetMmThreadPriority` | Query MMCSS thread priority |
| `waveOutOpen` / `waveInOpen` | Legacy wave audio API |
| `IMMDeviceEnumerator` | Enumerate audio endpoints |
| `IAudioClient` | Core Audio client (WASAPI) |
| `IAudioRenderClient` | Audio render client |
| `IAudioCaptureClient` | Audio capture client |
| `ISimpleAudioVolume` | Simple volume control |
| `IAudioSessionManager` | Audio session management |
| `IAudioEndpointVolume` | Endpoint volume control |
| `XAudio2Create` | XAudio2 API (game audio) |
| `MFCreateMediaType` | Media Foundation type creation |

---

## 13. MMCSS Task Profile Structure

Each task profile under `...\SystemProfile\Tasks\` defines:

```
[Audio]
  Affinity = 0 (no special affinity)
  Background Only = 0 (foreground capable)
  Clock Rate = 10000
  GPU Priority = 8
  Latency Sensitive = 1 (yes)
  Priority = 6 (high)
  Scheduling Category = "High"
  SFIO Priority = "High"

[Games]
  Affinity = 0
  Background Only = 0
  Clock Rate = 10000
  GPU Priority = 8
  Latency Sensitive = 1
  Priority = 2
  Scheduling Category = "Medium"
  SFIO Priority = "Normal"

[Pro Audio]
  Affinity = 0
  Background Only = 0
  Clock Rate = 10000
  GPU Priority = 8
  Latency Sensitive = 1
  Priority = 6
  Scheduling Category = "High"
  SFIO Priority = "High"
```

---

## 14. ETW Providers

| Provider | Description |
|----------|-------------|
| `Microsoft-Windows-Audio` | Audio engine events |
| `Microsoft-Windows-AudioPlayback` | Audio playback events |
| `Microsoft-Windows-AudioCapture` | Audio capture events |
| `Microsoft-Windows-MMCSS` | MMCSS scheduler events (CiLog*) |
| `Microsoft-Windows-Kernel-Streaming` | KS framework events |

Example collection:
```powershell
wpr -start Audio -start MMCSS
# ... reproduce scenario ...
wpr -stop C:\audio_trace.etl
```
