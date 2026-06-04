# DWM / Desktop Composition — Windows 11 ARM64 Kernel Research

**Target**: Windows 11 Build 26100 (ARM64, Parallels VM)
**Primary Modules**: `win32kbase` (fffff800`6dc20000–6df17000), `win32kfull` (fffff800`6d200000–6d5c4000), `win32k` (fffff800`68d30000–68de0000)
**Date**: 2026-06-04

---

## 1. Architecture Overview

DWM (Desktop Window Manager) is the compositor responsible for rendering the Windows desktop. On modern Windows 11, DWM uses a composition-based rendering pipeline where:

1. Applications render to their own surfaces (swap chains / composition surfaces)
2. DWM composites all windows into a single frame via DirectComposition
3. The composed frame is presented to the display via FlipManager / DXGI

The kernel-side infrastructure lives primarily in **win32kbase.sys**, which provides:
- **DirectComposition** — kernel marshaler framework for visual trees, transforms, brushes, animations
- **FlipManager** — frame presentation pipeline (IFlip, posted presents, confirmed, signaled, skipped)
- **VSync** — display synchronization (PDEVOBJ::vSync, DxgkModifyVSyncWaiterInternal)
- **DWM process management** — startup/shutdown, LPC messaging

---

## 2. DWM Process State (This VM)

```
gDWMCapable = 0
g_bDwmIsShuttingDown = 0
```

**Note**: On this Parallels ARM64 VM, DWM is not active (gDWMCapable=0), meaning the display driver likely runs in a basic mode without full composition. This is common for VM environments without GPU paravirtualization. Production systems would have gDWMCapable != 0.

---

## 3. DirectComposition Kernel Framework

### 3.1 Nt* Syscalls (Kernel Entry Points)

DirectComposition exposes ~50+ Nt* syscalls in win32kbase for user-mode (dwm.exe, applications) to interact with the composition engine:

| Syscall | Purpose |
|---------|---------|
| `NtDCompositionCreateConnection` | Establish DComp connection for a process |
| `NtDCompositionDestroyConnection` | Teardown DComp connection |
| `NtDCompositionCreateChannel` | Create composition channel (command buffer) |
| `NtDCompositionGetConnectionBatch` | Retrieve batch of composition commands |
| `NtDCompositionProcessChannelBatchBuffer` | Process composition batch |
| `NtDCompositionCommitChannel` | Commit pending composition changes |
| `NtDCompositionWaitForChannel` | Wait for channel processing |
| `NtDCompositionSynchronize` | Synchronize composition state |
| `NtDCompositionBeginFrame` | Begin a new composition frame |
| `NtDCompositionConfirmFrame` | Confirm frame for presentation |
| `NtDCompositionGetFrameId` | Get current frame identifier |
| `NtDCompositionGetFrameStatistics` | Get frame timing statistics |
| `NtDCompositionGetTargetStatistics` | Get per-target (monitor) stats |
| `NtDCompositionGetFrameSurfaceUpdates` | Get surface update info for a frame |
| `NtDCompositionWaitForCompositorClock` | Wait for compositor VBlank |
| `NtDCompositionBoostCompositorClock` | Request compositor clock boost (for pen/ink) |
| `NtDCompositionEnableMMCSS` | Enable Multimedia Class Scheduler for DComp |
| `NtDCompositionCreateSharedResourceHandle` | Create cross-process shared handle |
| `NtDCompositionDuplicateHandleToProcess` | Duplicate DComp handle to another process |
| `NtDCompositionCreateAndBindSharedSection` | Shared memory section for batch data |
| `NtDCompositionSetBlurredWallpaperSurface` | Set acrylic/mica blurred background |
| `NtDCompositionAddCrossDeviceVisualChild` | Cross-device visual tree support |
| `NtDCompositionRegisterVirtualDesktopVisual` | Register visual for virtual desktop switch |
| `NtDCompositionRegisterThumbnailVisual` | Live thumbnail (TaskView, Alt-Tab) |
| `NtDCompositionNotifySuperWetInkWork` | Priority notification for ink input |
| `NtDCompositionTelemetrySetApplicationId` | Associate app ID for telemetry |
| `NtDCompositionSendDwmLpcMessage` | LPC message to DWM process |
| `NtDCompositionSuspendAnimations` | Suspend all animations (power saving) |
| `NtDCompositionCreateSynchronizationObject` | Create DComp fence/sync object |
| `NtDCompositionCreateBufferCollection` | Buffer collection for shared surfaces |

### 3.2 Resource Marshaler Hierarchy

DirectComposition uses a marshaler pattern where each composition resource type has a corresponding kernel marshaler class:

**Visual Tree Types**:
- `CVisualMarshaler` — base visual (transform, clip, opacity, offset, mode)
- `CSpriteVisualMarshaler` — visual with brush/content (most common)
- `CWindowNodeMarshaler` — window-backed visual (FlipEx surface, clip)
- `CHostVisualMarshaler` — host visual for cross-process composition
- `CRedirectVisualMarshaler` — redirect visual (capture, virtual monitor)
- `CGlobalDCompVisualMarshaler` — global DComp visual
- `CVisualGroupMarshaler` — visual group container

**Surface / Brush Types**:
- `CCompositionSurfaceBitmapMarshaler` — composition surface bitmap
- `CVisualSurfaceMarshaler` — virtual surface from visual subtree
- `CCompositionMipmapSurfaceMarshaler` — mipmap surface for LOD
- `CSurfaceBrushMarshaler` — surface brush (stretch, alignment, interpolation)
- `CYCbCrSurfaceMarshaler` — YCbCr color space surface
- `CSceneSurfaceMaterialInputMarshaler` — Scene3D surface material input

**Render Target Types**:
- `CLegacyRenderTargetMarshaler` — legacy (GDI-compatible) render target
- `CLegacyStereoRenderTargetMarshaler` — stereo (3D) legacy target
- `CDDisplayRenderTargetMarshaler` — DirectDisplay render target (HDR)
- `CRemoteAppRenderTargetMarshaler` — remote app (RDP) render target
- `CCaptureRenderTargetMarshaler` — capture render target (screen capture)
- `CVirtualMonitorCaptureRenderTargetMarshaler` — virtual monitor capture

**Transform Types**:
- `CMatrixTransformMarshaler` — 2D matrix transform
- `CMatrixTransform3DMarshaler` — 3D matrix transform
- `CManipulationTransformMarshaler` — manipulation (touch) transform
- `CInteractionMarshaler` — interaction handler

**Effect / Lighting Types**:
- `CCompositionAmbientLightMarshaler`
- `CCompositionDistantLightMarshaler`
- `CCompositionPointLightMarshaler`
- `CCompositionSpotLightMarshaler`
- `CProjectedShadowCasterMarshaler`

**Other**:
- `CAnimationBinding` — animation binding to visual property
- `CPrimitiveGroupMarshaler` — primitive group (vector graphics)
- `CPrimitiveColorMarshaler` — color primitive
- `CRegionGeometryMarshaler` — region geometry
- `CCompositionLightMarshaler` — composition light (targets, exclusions)
- `CHolographicInteropTextureMarshaler` — holographic/VR interop

### 3.3 Visual Properties (Set*Property on CVisualMarshaler)

Each visual supports setting properties via:
- `SetIntegerProperty` — offsets, modes, flags
- `SetFloatProperty` — opacity, transform values
- `SetBufferProperty` — transform matrices, clip rectangles
- `SetReferenceProperty` — link to brush, clip, transform resources
- `SetHandleProperty` — handle-based resources

Key visual operations:
- `EmitClip` — emit clip region
- `EmitBlurredWallpaperSurface` — emit acrylic/mica blur
- `EmitBlurredWallpaperSurfaceRect` — blur rect region
- `SetHeatMapColorHelper` — heat map debugging visualization

### 3.4 Composition Channel / Batch Processing

```
CApplicationChannel -> CreateChannel -> GetConnectionBatch -> ProcessChannelBatchBuffer -> CommitChannel
```

The batch processing pipeline:
1. Application creates a channel via `NtDCompositionCreateChannel`
2. Writes commands into shared section (created by `NtDCompositionCreateAndBindSharedSection`)
3. Commits via `NtDCompositionCommitChannel`
4. Kernel processes batch: each marshaler's `EmitUpdateCommands` / `EmitCreationCommand` generates MILCMD (MIL Command) packets
5. DWM reads completed frames via `NtDCompositionGetFrameStatistics` / `NtDCompositionGetFrameSurfaceUpdates`

---

## 4. FlipManager — Frame Presentation Pipeline

FlipManager manages the lifecycle of presented frames from application to display. It tracks each present through stages:

### 4.1 Present Lifecycle

```
PresentPosted → PresentProcessed → PresentConfirmed → PresentSignaled
                  ↘ PresentSkipped
                  ↘ PresentDeferred
                  ↘ PresentCanceled → CanceledPresentShown
```

### 4.2 FlipManager Events (ETW)

All FlipManager operations have corresponding ETW trace points:

| Event | Description |
|-------|-------------|
| `FlipManagerCreate` | FlipManager instance created |
| `FlipManagerDestroy` | FlipManager destroyed |
| `FlipManagerPresentPosted` | Present submitted by application |
| `FlipManagerPresentProcessed` | Present processed by compositor |
| `FlipManagerPresentConfirmed` | Present confirmed (display acknowledged) |
| `FlipManagerPresentSignaled` | Present signaled back to app |
| `FlipManagerPresentSkipped` | Present skipped (replaced by newer) |
| `FlipManagerPresentDeferred` | Present deferred (waiting for vsync) |
| `FlipManagerPresentCanceled` | Present canceled |
| `FlipManagerCanceledPresentShown` | Canceled present was actually shown |
| `FlipManagerContentFlip` | Content flip executed |
| `FlipManagerNoOpPresent` | No-op present (no actual content change) |

### 4.3 IFlip (Independent Flip)

IFlip bypasses DWM composition for fullscreen/borderless windows:

- `FlipManagerPresentIFlipSubmitted` — IFlip present submitted
- `FlipManagerPresentIFlipCompleted` — IFlip present completed
- `FlipManagerPresentIFlipPurgePreviousPresents` — Purge queued presents during IFlip

### 4.4 Content Binding

| Event | Description |
|-------|-------------|
| `FlipManagerBindingStart` | Bind content to flip chain |
| `FlipManagerBindingStop` | Unbind content from flip chain |
| `FlipManagerBindingInfo` | Binding info recorded |
| `FlipManagerAddContent` | Add content to FlipManager |
| `FlipManagerRemoveContent` | Remove content from FlipManager |
| `FlipManagerContentRebind` | Rebind content (resize/etc) |
| `FlipManagerContentUnbind` | Unbind content |
| `FlipManagerProducerSetContent` | Producer sets content |

### 4.5 Buffer Management

| Event | Description |
|-------|-------------|
| `FlipManagerAddBuffer` | Add buffer to flip pool |
| `FlipManagerRemoveBuffer` | Remove buffer from flip pool |
| `FlipManagerBufferAvailable` | Buffer became available |

### 4.6 Frame Synchronization

| Event | Description |
|-------|-------------|
| `FlipManagerStartCompleteToken` | Start token for frame completion |
| `FlipManagerStopCompleteToken` | Stop token |
| `FlipManagerStartTokenReleaseToFrame` | Release start token to frame |
| `FlipManagerStopTokenReleaseToFrame` | Release stop token to frame |
| `FlipManagerUpdateExpectedConsumerPresentId` | Update expected present ID |
| `FlipManagerPresentQueueDepth` | Queue depth tracking |
| `FlipManagerFlipAwayFenceCreate` | Flip-away fence created |
| `FlipManagerFlipAwayFenceDestroy` | Flip-away fence destroyed |
| `FlipManagerWaitForFrameFlipAway` | Wait for frame flip-away |
| `FlipManagerWaitForFrameRenderingComplete` | Wait for render completion |
| `FlipManagerLost` | FlipManager lost (device lost) |

---

## 5. VSync / Display Synchronization

### 5.1 VSync Functions

- `PDEVOBJ::vSync` — PDEV-level VSync handler
- `DxgkModifyVSyncWaiterInternal` — DXGK VSync waiter modification

### 5.2 Compositor Clock

- `NtDCompositionWaitForCompositorClock` — Wait for next VBlank with compositor
- `NtDCompositionBoostCompositorClock` — Boost compositor clock priority (ink latency)
- `NtDCompositionEnableMMCSS` — Enable MMCSS scheduling for compositor thread

### 5.3 Render Target Refresh Rate

- `CLegacyRenderTargetMarshaler::EmitUpdateRefreshRate` — Update legacy render target refresh
- `CDDisplayRenderTargetMarshaler::EmitUpdateRefreshRate` — Update DD render target refresh

---

## 6. DWM Process Management

### 6.1 DWM Lifecycle Functions

| Function | Purpose |
|----------|---------|
| `xxxDwmProcessStartup` | DWM process initialization |
| `xxxDwmProcessShutdown` | DWM process teardown |
| `xxxDwmControl` | DWM control messages |
| `IsDwmActive` | Check if DWM compositor is running |
| `IsProcessDwm` | Check if current process is dwm.exe |
| `GreDxgkRegisterDwmProcess` | Register DWM process with DXGK |

### 6.2 DWM Globals

| Global | Value (This VM) | Description |
|--------|------------------|-------------|
| `gDWMCapable` | 0 | Whether DWM composition is supported/active |
| `g_bDwmIsShuttingDown` | 0 | DWM shutdown flag |

### 6.3 DWM-to-Kernel Communication

- `NtDCompositionSendDwmLpcMessage` — LPC message from kernel to DWM
- `NtDCompositionDuplicateSwapchainHandleToDwm` — Duplicate swapchain handle into DWM process
- `NtDCompositionSetChannelCommitCompletionEvent` — Set completion event for DWM channel commit

---

## 7. Blurred Wallpaper / Acrylic / Mica

The kernel supports blurred wallpaper surfaces for transparency effects (Acrylic, Mica):

- `NtDCompositionSetBlurredWallpaperSurface` — Set blurred wallpaper surface for a visual
- `DirectComposition::CConnection::SetBlurredWallpaperSurface` — Connection-level API
- `DirectComposition::CConnection::SetBlurredWallpaperSurfaceInternal` — Internal implementation
- `DirectComposition::CConnection::EmitSetBlurredWallpaperSurface` — Emit blur command to batch
- `CVisualMarshaler::EmitBlurredWallpaperSurface` — Visual-level blur emit
- `CVisualMarshaler::EmitBlurredWallpaperSurfaceRect` — Region-specific blur

---

## 8. Feature Flags

| Feature | Description |
|---------|-------------|
| `Feature_DesktopDWMCursor` | DWM cursor rendering feature |
| `Feature_DCompkRacyAccess` | DComp thread safety debugging feature |
| `Feature_CompSwapchainRenderAndPresentSync` | Composition swapchain render/present synchronization |
| `Feature_DolbyVisionForcePresent` | Force DolbyVision present |
| `Feature_DolbyVisionAndUpdatedHdrUx` | DolbyVision + updated HDR UX |

---

## 9. Screen Capture / Composition Capture

- `CCaptureRenderTargetMarshaler` — Capture render target
- `CVirtualMonitorCaptureRenderTargetMarshaler` — Virtual monitor capture
- Capture properties: `EmitFlipManager`, `EmitDirtyRegionMode`, `EmitFrameRequest`, `EmitVisualsToExclude`, `EmitMinUpdateInterval`, `EmitAdapterLUID`, `EmitSDRBoost`, `EmitPreferReferenceVisual`

---

## 10. Key Findings for Optimizer

1. **DWM Composition Bypass (IFlip)**: Borderless/windowed fullscreen apps can use IFlip to bypass DWM, reducing compositor overhead. FlipManager IFlip events track this.

2. **Compositor Clock Boost**: `NtDCompositionBoostCompositorClock` allows boosting compositor priority for latency-sensitive scenarios (pen/ink). An optimizer could expose this as a gaming/input latency tuning option.

3. **VSync Control**: VSync waiter modification (`DxgkModifyVSyncWaiterInternal`) and compositor clock wait (`NtDCompositionWaitForCompositorClock`) are the kernel-level hooks for VSync management.

4. **FlipManager Queue Depth**: `FlipManagerPresentQueueDepth` tracks present queue depth — excessive queuing indicates GPU bottleneck; insufficient queuing indicates CPU bottleneck.

5. **Animation Suspension**: `NtDCompositionSuspendAnimations` can suspend all composition animations — useful for power saving or performance modes.

6. **MMCSS Integration**: `NtDCompositionEnableMMCSS` enables Multimedia Class Scheduler Service integration for DComp, ensuring compositor thread gets appropriate CPU scheduling priority.

7. **Blurred Wallpaper Overhead**: Acrylic/Mica effects use `NtDCompositionSetBlurredWallpaperSurface` which requires GPU blur computation — disabling can reduce GPU load on low-end systems.

8. **DWM Capability Check**: `gDWMCapable` indicates whether hardware composition is available. On VMs without GPU PV, DWM runs in software mode with higher CPU overhead.

9. **Present Latency Tracking**: The full present lifecycle (Posted→Processed→Confirmed→Signaled) can be monitored via ETW to identify composition bottlenecks.

10. **CompSwapchain Sync**: `Feature_CompSwapchainRenderAndPresentSync` controls render/present synchronization for composition swapchains — tuning this can reduce latency vs. throughput tradeoffs.

11. **DolbyVision/HDR**: `Feature_DolbyVisionForcePresent` and `Feature_DolbyVisionAndUpdatedHdrUx` control HDR presentation paths — these have different performance characteristics than SDR.

12. **Capture Overhead**: Screen capture (`CCaptureRenderTargetMarshaler`) adds overhead to the composition pipeline; dirty region mode and frame request throttling can mitigate this.

---

## 11. Registry / Configuration Keys

```
HKLM\SOFTWARE\Microsoft\Windows\DWM
  - Composition                        (REG_DWORD: 0=disabled, 1=enabled)
  - EnableAeroPeek                     (REG_DWORD)
  - AlwaysHibernateThumbnails          (REG_DWORD)
  - ColorPrevalence                    (REG_DWORD)

HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers
  - EnableVirtualRefreshRateEdid       (REG_DWORD)
```

---

## 12. User-Mode APIs for DWM Interaction

| API | Purpose |
|-----|---------|
| `DwmEnableComposition` | Enable/disable DWM composition (deprecated, always enabled on Win8+) |
| `DwmFlush` | Flush DWM composition batch |
| `DwmSetWindowAttribute` | Set window attributes (non-client rendering, flip model, etc.) |
| `DwmGetWindowAttribute` | Query window rendering attributes |
| `DwmExtendFrameIntoClientArea` | Extend glass/frame into client area |
| `DwmEnableMMCSS` | Enable MMCSS for DWM scheduling |
| `DwmRegisterThumbnail` | Register live thumbnail |
| `DwmUpdateThumbnailProperties` | Update thumbnail properties |
| `DwmSetPresentParameters` | Set present parameters for a window |
| Windows.UI.Composition | WinRT composition API (backed by DirectComposition) |

---

## 13. ETW Provider

DWM/Composition events are available under the following ETW providers:
- **Microsoft-Windows-Dwm-Core** — DWM core composition events
- **Microsoft-Windows-DxgKrnl** — DXGI/kernel graphics (FlipManager events)
- **Microsoft-Windows-DirectComposition** — DirectComposition events
- **Microsoft-Windows-Win32k** — win32k operations

Example collection:
```powershell
# Collect DWM composition events
wpr -start DWM -start Composition
# ... reproduce scenario ...
wpr -stop C:\dwm_trace.etl

# Or via tracelog
tracelog -start DwmTrace -guid #{dwm_provider_guid} -f C:\dwm.etl -level 5 -flags 0xFFFFFFFF
```
