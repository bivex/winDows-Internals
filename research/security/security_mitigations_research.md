# Security Mitigations Overhead Research (Windows 11 ARM64 Build 26100)

> Kernel-debugged via WinDbg MCP on ARM64 target (Parallels VM)
> Module: `ntoskrnl` (HAL linked into ntoskrnl on ARM64)

---

## 1. Overview

Windows implements multiple security mitigations at the kernel level to protect against speculative execution attacks, control-flow hijacking, and memory disclosure. Each mitigation has a performance cost. This document catalogs all mitigations discovered via kernel debugging.

---

## 2. Speculative Execution Mitigations (ARM64 SPC)

### 2.1 SPC — Speculative Policy Controller

ARM64 uses PSCI (Power State Coordination Interface) and HVC/SMC calls for firmware-level speculative execution mitigations.

```
SpcCallPsci                              - Call PSCI for speculative control
SpcCallHvc                               - Call HVC (Hypervisor Call) for control
SpcCallSmc                               - Call SMC (Secure Monitor Call) for control
SpcFlushBranchPredictorCache             - Flush branch predictor (performance cost)
SpcDetectBtiBhbMitigationNeeds           - Detect BTI/BHB mitigation requirements
SpcDetectKvaMitigationNeeds              - Detect KVA shadow mitigation requirements
SpcDetectFwBtiBhbSsbMitigationSupport    - Detect firmware BTI/BHB/SSB support
```

**SPC Global State:**

| Global | Value | Description |
|---|---|---|
| `SpcSpeculationPsciState` | 0x00000003 | Speculation control PSCI state (active mitigations) |

---

### 2.2 BTI — Branch Target Injection (Spectre Variant 2)

Branch target injection mitigation on ARM64 uses alternative exception vectors and firmware calls.

**Detection Functions:**

```
KiDetectBtiBhbMitigationNeeds            - Kernel: detect BTI/BHB mitigation needs
SpcDetectBtiBhbMitigationNeeds           - SPC: detect BTI/BHB mitigation needs
```

**Alternative Exception Vectors (ARM64):**

```
KiArm64ExceptionKvaVectors               - Base KVA vectors (no BTI)
KiArm64ExceptionKvaBtiSmcVectors         - KVA + BTI via SMC vectors
KiArm64ExceptionKvaBtiHvcVectors         - KVA + BTI via HVC vectors
KiArm64ExceptionKvaBhbSbVectors          - KVA + BHB SB (Speculation Barrier) vectors
KiArm64ExceptionKvaBhbClrVectors         - KVA + BHB CLRM (Clear Register) vectors
KiArm64ExceptionKvaBhbDsbIsbVectors      - KVA + BHB DSB+ISB vectors
```

**Performance Impact:**
- Alternative exception vectors add overhead to every system call, interrupt, and exception
- BHB clearing vectors (CLRM, DSB+ISB) add pipeline stalls on entry/exit
- The most expensive vector set is `KiArm64ExceptionKvaBhbDsbIsbVectors` (DSB + ISB barrier on every exception)

---

### 2.3 KVA Shadow — Kernel Virtual Address Shadow

KVA shadow (Meltdown mitigation) separates user and kernel page tables on exception entry/exit.

```
KiDetectKvaMitigationNeeds               - Detect KVA shadow requirements
KiCompleteKvaInitialization              - Complete KVA shadow setup
KiKvaShadowPageAllocate                  - Allocate KVA shadow page
KiKvaShadowMapSinglePage                 - Map single page in shadow
KiKvaShadowPageTableAllocate             - Allocate shadow page table
KeQueryKvaShadowInformation              - Query KVA shadow state
```

**KVA Shadow System Service Path:**

```
KiKvaExceptionCodeStart                  - KVA exception entry code start
KiKvaExceptionCodeEnd                    - KVA exception entry code end
KiKvaSystemServiceExit                   - KVA system service exit path
```

**TLB Flush for KVA Shadow:**

```
KiTbFlushKvaShadowAsid = 0               - KVA shadow ASID flush (0 = no flush needed)
KiTbFlushProcessBroadcast                - Flush process TLB on all CPUs
KiTbFlushProcessByAsidBroadcast          - Flush process TLB by ASID broadcast
KiTbFlushSingleAllAsidBroadcast          - Flush single entry all ASIDs broadcast
KiTbFlushSingleByAsidBroadcast           - Flush single entry by ASID broadcast
KiTbFlushRangeAllAsidBroadcast           - Flush range all ASIDs broadcast
```

**Performance Impact:**
- KVA shadow requires page table switch on every user/kernel transition
- TLB flushes are needed when switching between processes with different KVA states
- On ARM64, hardware is typically not vulnerable to Meltdown, so KVA shadow is often NOT active (confirmed: `KiTbFlushKvaShadowAsid = 0`)

---

### 2.4 SSB — Speculative Store Bypass

```
KiDetectSsbMitigationNeeds               - Detect SSB mitigation requirements
SpcDetectFwBtiBhbSsbMitigationSupport    - Detect firmware SSB support
```

---

### 2.5 Cache Prefetch Mitigation

```
KiDetectCachePrefetchMitigationNeeds     - Detect cache prefetch mitigation requirements
```

---

## 3. CET — Control-flow Enforcement Technology (Shadow Stacks)

### 3.1 CET Capabilities

| Global | Value | Description |
|---|---|---|
| `KiCetCapable` | 0x20000100 | CET capability flags (shadow stack + indirect branch tracking) |

**CET Query Functions:**

```
KeIsCetCapable                           - Check if CPU supports CET
KeIsKernelCetEnabled                     - Check if kernel CET is enabled
KeIsKernelCetAuditModeEnabled            - Check if kernel CET audit mode
KeIsUserCetAllowed                       - Check if user CET is allowed
```

### 3.2 Kernel Shadow Stack Management

```
MiUpdateKernelShadowStackOwnerData       - Update shadow stack owner data
MiValidateKernelShadowStackPage          - Validate shadow stack page
MiKernelShadowStackIdealForCaching       - Check if shadow stack should be cached
MiDeleteCachedKernelShadowStack          - Delete cached shadow stack
MiDeleteShadowStackPtes                  - Delete shadow stack PTEs
KeAllocateKernelHiberSwapShadowStacks    - Allocate hibernation swap shadow stacks
```

**CET Process Mitigation:**

```
PsBlockNonCetBinaries                    - Block binaries without CET support
EtwTimLogBlockNonCetBinaries             - ETW log for blocked non-CET binaries
MITIGATION_ENFORCE_BLOCK_NON_CET_BINARIES - Enforce CET binary blocking
MITIGATION_AUDIT_BLOCK_NON_CET_BINARIES  - Audit mode for CET binary blocking
```

**Performance Impact:**
- Shadow stacks add memory overhead (one shadow stack per kernel thread)
- Shadow stack validation adds overhead on function return
- `PsBlockNonCetBinaries` can prevent legacy applications from running
- Kernel CET adds overhead to every kernel function call/return

---

## 4. CFG — Control Flow Guard

### 4.1 Kernel CFG (Kscp)

ARM64 kernel CFG uses the Kscp (Kernel Shadow Call Stack Protocol) mechanism:

```
KscpCfgCheckUserCallTargetEs             - Check user call target (entry stub)
KscpCfgHandleInvalidCallTarget           - Handle invalid CFG call target
MiPatchCfgCallTargetsSort                - Sort CFG call targets for patching
```

**Performance Impact:**
- CFG adds an indirect call check on every indirect function call
- The check is a table lookup (bitmask) — typically L1 cache resident
- On ARM64, uses `BTI` (Branch Target Identification) instruction where supported
- Overhead: ~1-3% on indirect-call-heavy workloads

---

## 5. Retpoline (Return Trampoline)

Retpoline replaces indirect branches with return instructions to prevent speculative execution.

```
KeIsRetpolineEnabled                     - Check if retpoline is active
KeExitRetpoline                          - Exit retpoline context
MiIsRetpolineEnabled                     - Module-level retpoline check
MiCaptureRetpolineImportInfo             - Capture retpoline import data
MiUpdateRetpolineImportFixups            - Update retpoline import fixups
MiCreateRetpolineRelocationInformation   - Create retpine relocation data
MiFreeImageRetpolineContext              - Free retpoline image context
MiDoesPageRequireRetpolineFixups         - Check if page needs retpoline
MiDoesControlAreaRequireRetpolineFixups  - Check if control area needs retpoline
MiCaptureBootDriverRetpolineInfo         - Capture boot driver retpoline info
MiCaptureRetpolineRelocationTables       - Capture retpoline relocation tables
RtlSizeOfRetpolineRelocationEntry        - Size of retpine relocation entry
RtlpApplyGenericRetpolineFixup           - Apply generic retpine fixup
RtlCaptureRetpolineImportRvas            - Capture retpine import RVAs
RtlCreateRetpolineRelocationInformation  - Create retpine relocation info
MmGetImageRetpolineCodePage              - Get retpoline code page for image
```

**Performance Impact:**
- Retpoline replaces indirect calls with return-trampoline sequence
- Significant overhead on indirect-call-heavy code (virtual dispatch, COM interfaces)
- On ARM64, BTI (Branch Target Identification) instruction is preferred over retpoline
- Modern ARM64 CPUs should use firmware BTI rather than retpoline

---

## 6. HVCI — Hypervisor-Enforced Code Integrity

### 6.1 HVCI State

| Global | Value | Description |
|---|---|---|
| `HalpHvciEnabled` | 0 | HVCI is **not enabled** on this VM |
| `Feature_HvciScanHvptHandling__private_featureState` | 0x47 | HVCI scan handling feature flags |

**HVCI Functions:**

```
KeEnableCoreIsolationMitigationPolicyThread  - Enable core isolation (HVCI thread)
KiEvaluateCoreIsolationMitigationPolicyEnforcibility - Evaluate if HVCI enforceable
KeIsCoreIsolationMitigationPolicyEnforceable - Query if HVCI policy enforceable
KiAdjustCoreIsolationReasonThread        - Adjust core isolation reason per thread
PspApplyCoreIsolationPolicy              - Apply core isolation to process
```

**Core Isolation Global:**

| Global | Value | Description |
|---|---|---|
| `KiCoreIsolationEnforceable` | 1 | Core isolation is enforceable (but not enabled) |

**Performance Impact (when enabled):**
- HVCI uses the hypervisor to enforce code integrity — every executable page is verified
- Write to executable pages triggers hypercall (slow)
- Code page modifications require HVPT (Hypervisor Page Table) updates
- Can add 5-15% overhead on code-generation workloads (JIT compilers, emulators)
- **Not active on this VM** (`HalpHvciEnabled = 0`)

---

## 7. Process Mitigation Options

### 7.1 System-Wide Mitigation Configuration

| Global | Value | Description |
|---|---|---|
| `PspSystemMitigationOptions` | 0x00000000 (all zeros) | System mitigation options (all disabled/defaults) |
| `PspSystemMitigationAuditOptions` | (exists) | Audit options for system mitigations |
| `PspSystemMitigationOptionsLength` | (exists) | Length of mitigation options array |
| `PspHardenedMitigationOptionsMap` | (exists) | Hardened mitigation options mapping |

**Process Mitigation Functions:**

```
PspApplyMitigationOptions                - Apply mitigation options to new process
PspInheritMitigationOptions              - Inherit mitigations from parent process
PspInheritMitigationAuditOptions         - Inherit audit options
PspReadIFEOMitigationOptions             - Read IFEO mitigation options
PspReadIFEOMitigationAuditOptions        - Read IFEO audit options
PspValidateMitigationOptions             - Validate mitigation options
PspValidateMitigationAuditOptions        - Validate audit options
PspDecodeMitigationExecuteOptions        - Decode execute mitigation options
PspHardenMitigationOptions               - Harden mitigation options
PsQueryProcessSignatureMitigationPolicy  - Query process signature mitigation
```

**Available Process Mitigations:**

| Mitigation | Enforce Symbol | Audit Symbol |
|---|---|---|
| Prohibit Dynamic Code | `MITIGATION_ENFORCE_PROHIBIT_DYNAMIC_CODE` | `MITIGATION_AUDIT_PROHIBIT_DYNAMIC_CODE` |
| Prohibit Remote Image Map | `MITIGATION_ENFORCE_PROHIBIT_REMOTE_IMAGE_MAP` | `MITIGATION_AUDIT_PROHIBIT_REMOTE_IMAGE_MAP` |
| Prohibit Low-IL Image Map | `MITIGATION_ENFORCE_PROHIBIT_LOWIL_IMAGE_MAP` | `MITIGATION_AUDIT_PROHIBIT_LOWIL_IMAGE_MAP` |
| Prohibit Win32k System Calls | `MITIGATION_ENFORCE_PROHIBIT_WIN32K_SYSTEM_CALLS` | `MITIGATION_AUDIT_PROHIBIT_WIN32K_SYSTEM_CALLS` |
| Prohibit Fsctl System Calls | `MITIGATION_ENFORCE_PROHIBIT_FSCTL_SYSTEM_CALLS` | `MITIGATION_AUDIT_PROHIBIT_FSCTL_SYSTEM_CALLS` |
| Prohibit Non-Microsoft Binaries | `MITIGATION_ENFORCE_PROHIBIT_NON_MICROSOFT_BINARIES` | `MITIGATION_AUDIT_PROHIBIT_NON_MICROSOFT_BINARIES` |
| Prohibit Child Process Creation | `MITIGATION_ENFORCE_PROHIBIT_CHILD_PROCESS_CREATION` | `MITIGATION_AUDIT_PROHIBIT_CHILD_PROCESS_CREATION` |
| Block Non-CET Binaries | `MITIGATION_ENFORCE_BLOCK_NON_CET_BINARIES` | `MITIGATION_AUDIT_BLOCK_NON_CET_BINARIES` |
| Redirection Trust Policy | `MITIGATION_ENFORCE_REDIRECTION_TRUST_POLICY` | `MITIGATION_AUDIT_REDIRECTION_TRUST_POLICY` |

---

## 8. ASLR (Address Space Layout Randomization)

### 8.1 Kernel SharedUserData ASLR

| Global | Value | Description |
|---|---|---|
| `Feature_KernelSharedUserDataAslr__private_featureState` | 0x57 | SharedUserData ASLR feature active |

**ASLR Functions:**

```
MiInitializeProcessBottomUpEntropy       - Initialize bottom-up ASLR entropy for process
```

**Performance Impact:**
- ASLR randomization adds negligible runtime overhead
- Bottom-up entropy increases randomization quality at image load time (one-time cost)
- SharedUserData ASLR randomizes the KUSER_SHARED_DATA location

---

## 9. Secure Boot

```
SeSecureBootQueryInformation             - Query Secure Boot state
SeQuerySecureBootPolicyValue             - Query Secure Boot policy value
SeQuerySecureBootPlatformManifest       - Query platform manifest
SepSecureBootCheckForUpdates             - Check for Secure Boot policy updates
SepSecureBootBuildRules                  - Build Secure Boot policy rules
SepSecureBootCorrectBcd                  - Correct BCD for Secure Boot compliance
SepSecureBootValidateBcdDataAgainstBcdRule - Validate BCD against Secure Boot rules
SeSecureBootRegisterPolicy               - Register Secure Boot policy
```

---

## 10. Firmware Page Protection

| Global | Value | Description |
|---|---|---|
| `ExpFirmwarePageProtectionSupported` | 1 | Firmware page protection is supported |

```
ExpGetKernelDataProtection               - Get kernel data protection state
ExpSetKernelDataProtection               - Set kernel data protection state
```

---

## 11. DRIPS — Directed Runtime Idle Power State (Mitigation Integration)

DRIPS queries enabled security mitigations for power management decisions:

```
PopDirectedDripsQueryEnabledMitigations  - Query enabled mitigations for DRIPS
PopDirectedDripsQueryMitigationStatus    - Query mitigation status for DRIPS
```

---

## 12. ETW Tracing for Security Mitigations

```
EtwSecurityMitigationsRegHandle          - ETW registration handle
SecurityMitigationsProviderGuid          - ETW provider GUID
EtwpQueryProcessEnabledSecurityMitigations - Query process mitigations via ETW
EtwpTimLogMitigationForProcess           - Log mitigation event for process
EtwTraceLongDpcMitigationEvent           - Trace long DPC mitigation event
```

---

## 13. ARM64-Specific: SMMU (System MMU / IOMMU)

ARM64 uses SMMU (System Memory Management Unit) for device I/O isolation. SMMUv3 is the latest version.

```
SmmupRegisterSmmu                        - Register SMMU device
Smmupv3InitializeIommu                   - Initialize SMMUv3 IOMMU
SmmupV2InitializeIommu                   - Initialize SMMUv2 IOMMU
Smmupv3EnableIommu                       - Enable SMMUv3
SmmupV2EnableIommu                       - Enable SMMUv2
Smmupv3AttachDeviceDomain                - Attach device to SMMUv3 domain
SmmupV2AttachDeviceDomain                - Attach device to SMMUv2 domain
Smmupv3HandleGlobalAndSmmuFaults         - Handle SMMUv3 faults
SmmupV2HandleSmmuGlobalFault             - Handle SMMUv2 global fault
Smmupv3SubmitCommands                    - Submit commands to SMMUv3
SmmupProcessIortTable                    - Process IORT (IO Remapping Table)
```

**SMMU Globals:**

| Global | Description |
|---|---|
| `SmmupInitFlags` | SMMU initialization flags |
| `SmmupInitStatus` | SMMU initialization status |
| `SmmupInterfaceMode` | SMMU interface mode |
| `SmmupIsQcClientPlatform` | Qualcomm client platform flag |
| `SmmupPlatformErrata` | Platform SMMU errata flags |

**Performance Impact:**
- SMMU adds address translation overhead for DMA operations
- SMMUv3 command queue submission has latency
- Fault handling can stall device I/O
- On this VM (Parallels), SMMU may be passthrough (no isolation)

---

## 14. ARM64 Exception Vector Variants

The kernel selects exception vectors based on which mitigations are active. The vector table determines the cost of every exception entry/exit:

| Vector Set | Mitigations Active | Performance Cost |
|---|---|---|
| `KiArm64ExceptionKvaVectors` | KVA only | Baseline |
| `KiArm64ExceptionKvaBtiSmcVectors` | KVA + BTI via SMC | +DSB on entry/exit |
| `KiArm64ExceptionKvaBtiHvcVectors` | KVA + BTI via HVC | +DSB on entry/exit |
| `KiArm64ExceptionKvaBhbSbVectors` | KVA + BHB SB | +SB on entry |
| `KiArm64ExceptionKvaBhbClrVectors` | KVA + BHB CLRM | +CLRM on entry |
| `KiArm64ExceptionKvaBhbDsbIsbVectors` | KVA + BHB DSB+ISB | **Highest cost** — full barrier on every exception |

---

## 15. User-Mode / Registry APIs for Mitigation Control

### Windows Security Settings

| API / Setting | Purpose |
|---|---|
| `SystemPropertiesPerformance` > Data Execution Prevention | DEP configuration |
| `Windows Security` > Core Isolation > Memory Integrity | HVCI toggle |
| `Windows Security` > Core Isolation > Kernel DMA Protection | DMA protection |
| Registry: `HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity` | HVCI enable |
| Registry: `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\EnableCfg` | CFG enable |
| Registry: `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\MoveImages` | ASLR enable |
| `bcdedit /set nx OptIn / OptOut` | DEP policy |
| `bcdedit /set hypervisorlaunchtype Auto / Off` | VBS/HVCI control |
| Process Mitigation APIs: `SetProcessMitigationPolicy` | Per-process mitigation control |

### Spectre/Meltdown Mitigation Registry

| Key | Path | Description |
|---|---|---|
| `FeatureSettingsOverride` | `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management` | Override spectre mitigation |
| `FeatureSettingsOverrideMask` | `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management` | Override mask |

---

## 16. Key Findings for Optimizer

1. **KVA Shadow is NOT active on this ARM64 system** (`KiTbFlushKvaShadowAsid = 0`) — ARM64 CPUs are not vulnerable to Meltdown, so this costly mitigation is bypassed
2. **HVCI is NOT enabled** (`HalpHvciEnabled = 0`) — but core isolation is enforceable (`KiCoreIsolationEnforceable = 1`)
3. **CET shadow stacks are configured** (`KiCetCapable = 0x20000100`) — hardware supports shadow stacks and indirect branch tracking
4. **System-wide mitigation options are all zeros** (`PspSystemMitigationOptions = 0`) — no extra per-process mitigations enforced by default
5. **Firmware page protection is supported** (`ExpFirmwarePageProtectionSupported = 1`)
6. **Speculation control is active** (`SpcSpeculationPsciState = 0x3`) — some speculative execution mitigations are engaged via PSCI
7. **ARM64 uses alternative exception vectors for mitigations** — the selected vector set determines the per-syscall/interrupt overhead
8. **Retpoline is available** but ARM64 prefers BTI instruction — retpoline is less likely to be active on ARM64
9. **Kernel SharedUserData ASLR is active** (`featureState = 0x57`) — minimal performance impact
10. **CFG adds overhead on indirect calls** — `KscpCfgCheckUserCallTargetEs` is checked on every indirect user-mode call
11. **Branch predictor flush** (`SpcFlushBranchPredictorCache`) is the most expensive single mitigation operation
12. **DPC long-duration mitigation** is traced via ETW (`EtwTraceLongDpcMitigationEvent`) — helps identify DPC-related stalls
13. **SMMU (IOMMU) adds DMA translation overhead** — SMMUv3 command queue submission has measurable latency
14. **Process mitigation options can be customized per-process** via `SetProcessMitigationPolicy` — optimizer could relax mitigations for trusted game/trading processes
15. **DRIPS integration** — power management considers active mitigations when deciding idle state entry
16. **HVCI scan handling feature is partially active** (`Feature_HvciScanHvptHandling = 0x47`) — scan infrastructure is present even though HVCI is off
