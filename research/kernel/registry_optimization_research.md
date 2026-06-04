# Registry Optimization — Windows 11 ARM64 Kernel Research

**Target**: Windows 11 Build 26100 (ARM64, Parallels VM)
**Primary Module**: `ntoskrnl.exe` (Configuration Manager — CM)
**Date**: 2026-06-04

---

## 1. Architecture Overview

The Windows Registry subsystem is implemented by the Configuration Manager (CM), a kernel component within `ntoskrnl.exe`. It provides:

- **Hive management** — in-memory and on-disk registry hive structures (Hv* functions)
- **Lazy flush** — deferred write-back of dirty registry pages to disk
- **KCB cache** — Key Control Block hash table for fast name-based lookups
- **Registry callbacks** — altitude-ordered notification framework for drivers
- **Transactions** — lightweight and full transaction support for atomic operations
- **Virtualization** — UAC registry virtualization (per-user virtual stores)
- **Layered registry** — servicing layer stacking for component-based OS updates
- **Security descriptors** — cached security cells for ACL enforcement
- **Quota system** — per-hive and global registry size limits
- **State separation** — hive options for state-separated OS images

---

## 2. Nt*Key Syscalls (48 Native APIs)

### 2.1 Key Lifecycle

| Syscall | Purpose |
|---------|---------|
| `NtCreateKey` | Create or open a registry key |
| `NtOpenKey` | Open an existing registry key |
| `NtDeleteKey` | Delete a registry key |
| `NtRenameKey` | Rename a registry key |
| `NtReplaceKey` | Replace a hive file (used by setup) |
| `NtCompactKeys` | Compact (defragment) specified keys |
| `NtCompressKey` | Compress a registry key |

### 2.2 Value Operations

| Syscall | Purpose |
|---------|---------|
| `NtSetValueKey` | Set a value entry |
| `NtQueryValueKey` | Query a value entry |
| `NtEnumerateValueKey` | Enumerate value entries |
| `NtQueryMultipleValueKey` | Query multiple values in one call |

### 2.3 Key Enumeration / Notification

| Syscall | Purpose |
|---------|---------|
| `NtEnumerateKey` | Enumerate subkeys |
| `NtNotifyChangeKey` | Register for change notification |
| `NtNotifyChangeMultipleKeys` | Multi-key change notification |
| `NtQueryOpenSubKeys` | Query open subkey handles |
| `NtQueryOpenSubKeysEx` | Extended open subkey query |

### 2.4 Hive Load / Unload

| Syscall | Purpose |
|---------|---------|
| `NtLoadKey` | Load a hive file |
| `NtLoadKey2` | Load key (variant 2) |
| `NtLoadKey3` | Load key (variant 3) |
| `NtLoadKeyEx` | Extended hive load |
| `NtUnloadKey` | Unload a hive |
| `NtUnloadKey2` | Unload key (variant 2) |
| `NtUnloadKeyEx` | Extended hive unload |

### 2.5 Hive Save / Restore / Flush

| Syscall | Purpose |
|---------|---------|
| `NtSaveKey` | Save a hive to file |
| `NtSaveKeyEx` | Extended hive save |
| `NtSaveMergedKeys` | Save merged hives |
| `NtRestoreKey` | Restore hive from file |
| `NtFlushKey` | Flush a key's hive to disk |

### 2.6 Transacted Operations

| Syscall | Purpose |
|---------|---------|
| `NtCreateKeyTransacted` | Create key within transaction |
| `NtOpenKeyTransacted` | Open key within transaction |
| `NtOpenKeyTransactedEx` | Extended transacted open |

### 2.7 Keyed Events / Locking

| Syscall | Purpose |
|---------|---------|
| `NtCreateKeyedEvent` | Create a keyed event |
| `NtOpenKeyedEvent` | Open a keyed event |
| `NtReleaseKeyedEvent` | Release keyed event |
| `NtWaitForKeyedEvent` | Wait on keyed event |
| `NtLockRegistryKey` | Lock a registry key |
| `NtLockProductActivationKeys` | Lock product activation keys |

### 2.8 Information / Misc

| Syscall | Purpose |
|---------|---------|
| `NtSetInformationKey` | Set key information (last write time, flags) |
| `NtQueryInformationKey` | Query key information (class, subkeys, values) |

---

## 3. Hive Management (Hv* Functions)

### 3.1 Hive Structure Operations

| Function | Purpose |
|----------|---------|
| `HvAllocateCell` | Allocate a cell in the hive |
| `HvFreeCell` | Free a cell |
| `HvReAllocateCell` | Reallocate a cell to different size |
| `HvMarkCellDirty` | Mark a cell as dirty (needs flush) |
| `HvMarkClean` | Mark cells as clean |
| `HvGetCellSize` | Get cell size |
| `HvGetCellFlat` | Get flat cell pointer |
| `HvReleaseCell` | Release cell reference |
| `HvIsCellAllocated` | Check if cell is allocated |

### 3.2 Hive Flush / Log

| Function | Purpose |
|----------|---------|
| `HvFlushHive` | Flush entire hive to disk |
| `HvSyncHive` | Synchronize hive (primary + log) |
| `HvWriteHiveFlushLog` | Write flush log for crash recovery |
| `HvReadHiveFlushLog` | Read flush log for crash recovery |
| `HvUpdateHiveWriteLog` | Update write log |
| `HvUpdatePrimaryLogForLogHive` | Update primary log for logged hive |
| `HvOpenAndMapLogFiles` | Open and map hive log files |
| `HvMapHiveBuffers` | Map hive data buffers |
| `HvDiscardPendingFlushForSnapshots` | Discard pending flush for snapshot hives |

### 3.3 Hive Initialization

| Function | Purpose |
|----------|---------|
| `HvInitializeHive` | Initialize a new hive |
| `HvLoadHive` | Load hive from file |
| `HvSnapshotUnrecoverableData` | Snapshot unrecoverable hive data |
| `HvCheckHive` | Check hive integrity |
| `HvCheckKey` | Check key integrity |
| `HvValidateHive` | Validate hive structure |
| `HvRefreshHive` | Refresh hive from disk |

### 3.4 Hive Bin Management

| Function | Purpose |
|----------|---------|
| `HvAllocateBin` | Allocate a bin (group of cells) |
| `HvFreeBin` | Free a bin |
| `HvCoalesceDiscardedBins` | Coalesce freed bins |
| `HvFindMergeCandidateBins` | Find bins that can be merged |
| `HvResizeHive` | Resize hive storage |

### 3.5 Hive Security

| Function | Purpose |
|----------|---------|
| `HvGetCellSecurity` | Get cell security descriptor |
| `HvSetCellSecurity` | Set cell security descriptor |

### 3.6 Key Hive Globals

| Global | Purpose |
|--------|---------|
| `HvpHiveListHead` | Linked list of all loaded hives |
| `HvpProfileListHead` | List of profile hives |
| `HvShutdown` | Hive shutdown flag |
| `HvShutdownComplete` | Shutdown completion flag |

---

## 4. Lazy Flush System

### 4.1 Architecture

Windows does not immediately write registry changes to disk. Instead, it uses a lazy flush mechanism:

```
Registry Write → CmpLazyFlushDpcRoutine (timer DPC)
    → CmpLazyWriteWorker (worker thread)
    → CmpFlushHives (flush dirty hives)
    → HvFlushHive / HvSyncHive
```

### 4.2 Lazy Flush Functions

| Function | Purpose |
|----------|---------|
| `CmpLazyFlushDpcRoutine` | DPC triggered by lazy flush timer |
| `CmpLazyWriteWorker` | Worker thread that flushes dirty hives |
| `CmpFlushHives` | Flush all dirty hives |
| `CmpFlushHive` | Flush a single hive |
| `CmpLazyCommit` | Lazy commit (force flush pending changes) |
| `CmpHoldLazyFlush` | Hold/pause lazy flushing |
| `CmpReleaseLazyFlush` | Release lazy flush hold |
| `CmpArmLazyFlushTimer` | Arm the lazy flush timer |
| `CmpInitializeLazyFlush` | Initialize lazy flush system |

### 4.3 Flush Control Functions

| Function | Purpose |
|----------|---------|
| `CmpCoalesceFlush` | Coalesce multiple flush requests |
| `CmpIsHiveFlushOnly` | Check if flush-only operation |
| `CmpDoFlushAll` | Flush all hives (used at shutdown) |
| `CmpDoFlushNextHive` | Flush next dirty hive in queue |
| `CmpAddToDelayedQueue` | Add hive to delayed flush queue |
| `CmpRemoveFromDelayedQueue` | Remove from delayed queue |
| `CmpProcessDelayedQueue` | Process delayed flush queue |

### 4.4 Lazy Flush Globals (Live Values from Target)

| Global | Value | Purpose |
|--------|-------|---------|
| `CmpLazyFlushIntervalInSeconds` | 0x3C (60) | Seconds between lazy flushes |
| `CmpEnableLazyFlushBootDelayInterval` | 0x3C (60) | Boot delay before lazy flush starts |
| `CmpHoldLazyFlush` | 0 | Whether lazy flush is held |
| `CmpPeriodicBackupFlushHiveCount` | 7 | Number of hives between periodic backups |
| `CmpGlobalFlushControlFlags` | — | Bitmask controlling flush behavior |
| `CmpForceForceFlushFlag` | — | Flag to force a full flush |
| `CmpLazyFlushHiveCount` | — | Count of hives in lazy flush queue |

### 4.5 Periodic Backup

| Function | Purpose |
|----------|---------|
| `CmpPeriodicBackupFlush` | Periodic backup flush worker |
| `CmpSchedulePeriodicBackup` | Schedule next periodic backup |
| `CmpWritePeriodicBackupHive` | Write periodic backup hive |

---

## 5. KCB (Key Control Block) Cache

### 5.1 Architecture

KCBs cache key name-to-object mappings for fast registry lookups. They are organized in a hash table:

```
NtOpenKey("HKLM\Software\Microsoft\Windows")
    → CmpLookupKeyControlBlockByHash
    → KCB Hash Table lookup
    → Return cached KCB (if hit) or create new KCB
```

### 5.2 KCB Functions

| Function | Purpose |
|----------|---------|
| `CmpCreateKeyControlBlock` | Create a new KCB |
| `CmpSearchKeyControlBlock` | Search for existing KCB |
| `CmpDereferenceKeyControlBlock` | Dereference a KCB |
| `CmpReferenceKeyControlBlock` | Reference a KCB |
| `CmpRemoveKeyControlBlock` | Remove KCB from hash table |
| `CmpCleanUpKcbValueCache` | Clean up KCB value cache |
| `CmpCleanUpKcbCacheWithLock` | Clean up KCB cache under lock |
| `CmpLookupKeyControlBlockByHash` | Hash-based KCB lookup |
| `CmpComputeKeyControlBlockHash` | Compute hash for key path |
| `CmpRehashKeyControlBlock` | Rehash KCB (on collision) |
| `CmpRenameKeyControlBlock` | Update KCB after rename |
| `CmpRebuildKeyName` | Rebuild full key name in KCB |
| `CmpSetCachedSubkeyCount` | Cache subkey count in KCB |
| `CmpGetCachedSubkeyCount` | Get cached subkey count |
| `CmpDelayDerefKCB` | Delayed KCB dereference |
| `CmpDelayDerefKCBWorker` | Worker for delayed deref |
| `CmpPerformDelayDerefKCB` | Perform delayed deref |
| `CmpCleanUpDelayDerefKCBs` | Cleanup all delayed derefs |

### 5.3 KCB Cache Globals

| Global | Purpose |
|--------|---------|
| `CmpKeyControlBlockHashTable` | KCB hash table (array of buckets) |
| `CmpKeyControlBlockHashTableSize` | Hash table size |
| `CmpKcbCacheHitCount` | Cache hit counter |
| `CmpKcbCacheMissCount` | Cache miss counter |
| `CmpDelayedDerefKCBList` | List of KCBs pending delayed deref |
| `CmpDelayedDerefKCBLock` | Lock for delayed deref list |

### 5.4 Compressed Name Handling

| Function | Purpose |
|----------|---------|
| `CmpCompareCompressedName` | Compare compressed key names |
| `CmpHashCompressedComponent` | Hash a compressed name component |
| `CmpCopyCompressedName` | Copy compressed name |
| `CmpConvertCompressedNameToUnicode` | Convert compressed→Unicode |
| `CmpConvertUnicodeToCompressedName` | Convert Unicode→compressed |

---

## 6. Registry Callbacks

### 6.1 Architecture

Drivers register callbacks via `CmRegisterCallback` / `CmRegisterCallbackEx`. Callbacks are ordered by altitude (a string like "370030") and invoked for every registry operation.

```
NtSetValueKey → CmpCallCallBacksEx
    → Callback 1 (altitude 100000, e.g., antivirus)
    → Callback 2 (altitude 370030, e.g., process protection)
    → ... → Actual registry operation
```

### 6.2 Callback Registration / Management

| Function | Purpose |
|----------|---------|
| `CmRegisterCallback` | Register a registry callback |
| `CmRegisterCallbackEx` | Extended registration with altitude |
| `CmUnRegisterCallback` | Unregister a callback |
| `CmpSetCreateCallback` | Set create callback |
| `CmpCallCallBacks` | Call all registered callbacks |
| `CmpCallCallBacksEx` | Extended callback invocation |
| `CmpGetCallbackListEntry` | Get callback list entry |
| `CmpInsertCallbackListEntry` | Insert into callback list (sorted by altitude) |
| `CmpRemoveCallbackListEntry` | Remove from callback list |
| `CmpFindCallbackByCookie` | Find callback by registration cookie |

### 6.3 Callback Context

| Function | Purpose |
|----------|---------|
| `CmpGetCallbackContext` | Get callback context |
| `CmpSetCallbackContext` | Set callback context |
| `CmpAllocateCallbackContext` | Allocate callback context |
| `CmpFreeCallbackContext` | Free callback context |

### 6.4 Callback Globals (Live Values from Target)

| Global | Value | Purpose |
|--------|-------|---------|
| `CmpCallBackCount` | 5 | Number of registered registry callbacks |
| `CmpCallBackListHead` | — | Head of callback linked list |
| `CmpCallBackListLock` | — | Lock for callback list |
| `CmpTraceCallbackRegistration` | — | ETW trace callback |

---

## 7. Registry Transactions

### 7.1 Architecture

Windows supports lightweight and full transactions for registry operations. Lightweight transactions use internal journaling without the full KTM (Kernel Transaction Manager) overhead.

### 7.2 Transaction Functions

| Function | Purpose |
|----------|---------|
| `CmpInitLightWeightTransaction` | Initialize lightweight transaction |
| `CmpCommitLightWeightTransaction` | Commit lightweight transaction |
| `CmpAbortLightWeightTransaction` | Abort lightweight transaction |
| `CmpPrepareLightWeightTransaction` | Prepare lightweight transaction |
| `CmpDiscardLightWeightTransaction` | Discard (no-op) lightweight transaction |
| `CmpReDoLightWeightTransaction` | Redo a lightweight transaction (recovery) |
| `CmpUnDoLightWeightTransaction` | Undo a lightweight transaction (recovery) |
| `CmpFindAndOpenTransactionManager` | Find/open transaction manager |
| `CmpCreateTransactionManager` | Create transaction manager |
| `CmpDeleteTransactionManager` | Delete transaction manager |
| `CmpOpenEnlistment` | Open transaction enlistment |
| `CmpCreateEnlistment` | Create transaction enlistment |
| `CmpPrepareTransaction` | Prepare a transaction |
| `CmpCommitTransaction` | Commit a transaction |
| `CmpRollbackTransaction` | Rollback a transaction |
| `CmpRecoverTransaction` | Recover a transaction |
| `CmpRecoverTransactionManager` | Recover transaction manager |

### 7.3 Transaction Log

| Function | Purpose |
|----------|---------|
| `CmpWriteTransactionLog` | Write transaction log entry |
| `CmpReadTransactionLog` | Read transaction log entry |
| `CmpFlushTransactionLog` | Flush transaction log |
| `CmpTruncateTransactionLog` | Truncate transaction log |
| `CmpCleanupTransactionLog` | Cleanup transaction log |

---

## 8. Registry Virtualization (UAC)

### 8.1 Architecture

UAC registry virtualization redirects writes from `HKLM\Software` by non-elevated processes to a per-user virtual store at `HKCU\Software\Classes\VirtualStore\MACHINE\Software`.

### 8.2 Virtualization Functions

| Function | Purpose |
|----------|---------|
| `CmpVEExecuteVirtualStoreParseLogic` | Execute virtual store parse/redirect |
| `CmpOpenVirtualStoreKey` | Open virtual store key |
| `CmpCreateVirtualStoreKey` | Create virtual store key |
| `CmpGetVirtualStoreKey` | Get virtual store key |
| `CmpCheckVirtualStoreAccess` | Check virtual store access |
| `CmpIsVirtualStoreKey` | Check if key is in virtual store |
| `CmpRedirectToVirtualStore` | Redirect operation to virtual store |
| `CmpDeleteVirtualStoreKey` | Delete virtual store key |
| `CmpBuildVirtualStorePath` | Build virtual store path |
| `CmpIsPackageContainerVirtualizationEnabled` | Check if package container virtualization is enabled |
| `CmpFilterVirtualizationToasts` | Filter virtualization toast notifications |
| `CmpUpdateVirtualizationStatus` | Update virtualization status for a key |
| `CmpVEActiveAttemptVirtualization` | Attempt virtualization for active operation |
| `CmpVECheckForValidVirtualStoreKey` | Check for valid virtual store key |

---

## 9. Layered Registry (Servicing)

### 9.1 Architecture

Windows uses layered registry to support component-based servicing. Multiple hive layers can be stacked, with lookups resolving top-down through the layer stack.

### 9.2 Layer Functions

| Function | Purpose |
|----------|---------|
| `CmpLoadLayerVersion` | Load a specific layer version |
| `CmpUnloadLayerVersion` | Unload a layer version |
| `CmpGetActiveLayerVersion` | Get active layer version |
| `CmpSetLayerVersion` | Set layer version |
| `CmpResolveLayerForKey` | Resolve key through layer stack |
| `CmpPushLayerKCB` | Push KCB onto layer stack |
| `CmpPopLayerKCB` | Pop KCB from layer stack |
| `CmpBuildLayerStack` | Build layer stack for a key |
| `CmpMergeLayerData` | Merge data from multiple layers |
| `CmpIsLayeredKey` | Check if key has layers |
| `CmpGetLayerCount` | Get number of layers for a key |
| `CmpInitializeLayerEngine` | Initialize the layer engine |
| `CmpDestroyLayerStack` | Destroy layer stack |
| `CmpWalkLayerStack` | Walk layer stack |
| `CmpCompareLayerVersions` | Compare layer versions |
| `CmpActivateLayer` | Activate a layer |
| `CmpDeactivateLayer` | Deactivate a layer |
| `CmpCreateLayer` | Create a new layer |
| `CmpDeleteLayer` | Delete a layer |
| `CmpQueryLayerInformation` | Query layer information |
| `CmpServicingLayerNotify` | Notify layer of servicing event |

---

## 10. Security Descriptors

### 10.1 Security Cache

| Function | Purpose |
|----------|---------|
| `CmpSearchSecurityCellCache` | Search security descriptor cache |
| `CmpInsertSecurityCellCache` | Insert into security cache |
| `CmpRemoveSecurityCellCache` | Remove from security cache |
| `CmpInitSecurityCache` | Initialize security cache |
| `CmpDestroySecurityCache` | Destroy security cache |
| `CmpGetSecurityDescriptor` | Get security descriptor for key |
| `CmpSetSecurityDescriptor` | Set security descriptor for key |
| `CmpCheckAccess` | Check access against security descriptor |
| `CmpAssignSecurityDescriptor` | Assign security descriptor to key |
| `CmpQuerySecurityDescriptorInfo` | Query security descriptor info |
| `CmpNotifyChangeSecurity` | Notify security descriptor change |

---

## 11. Quota System

### 11.1 Architecture

The registry quota system limits total registry size to prevent runaway registry growth.

### 11.2 Quota Functions

| Function | Purpose |
|----------|---------|
| `CmpClaimGlobalQuota` | Claim global registry quota |
| `CmpReleaseGlobalQuota` | Release global registry quota |
| `CmpCheckGlobalQuota` | Check if quota available |
| `CmpSetGlobalQuota` | Set global quota limit |
| `CmpGetGlobalQuota` | Get current global quota |
| `CmpClaimHiveQuota` | Claim per-hive quota |
| `CmpReleaseHiveQuota` | Release per-hive quota |
| `CmpCheckHiveQuota` | Check hive quota |
| `CmpUpdateHiveQuotaUsage` | Update hive quota usage |
| `CmpAdjustHiveQuota` | Adjust hive quota limit |
| `CmpInitializeQuotaSystem` | Initialize quota system |

### 11.3 Quota Globals (Live Values from Target)

| Global | Value | Purpose |
|--------|-------|---------|
| `CmpGlobalQuota` | 0xFFFFFFFF | Global registry quota (unlimited on this system) |
| `CmpGlobalQuotaUsed` | — | Current global quota usage |
| `CmpSystemHiveHysteresisHigh` | — | System hive high-water mark |
| `CmpSystemHiveHysteresisLow` | — | System hive low-water mark |

---

## 12. Hive Cache

### 12.1 Architecture

The hive cache provides fast access to frequently used hive data using oplock-based cache coherence.

| Function | Purpose |
|----------|---------|
| `CmpHiveCachePopulateHiveEntry` | Populate cache entry for a hive |
| `CmpHiveCacheRemoveHiveEntry` | Remove hive from cache |
| `CmpHiveCacheLookupHiveEntry` | Lookup hive in cache |
| `CmpHiveCacheInitialize` | Initialize hive cache |
| `CmpHiveCacheShutdown` | Shutdown hive cache |
| `CmpAcquireHiveOplock` | Acquire oplock on hive |
| `CmpReleaseHiveOplock` | Release oplock on hive |
| `CmpBreakHiveOplock` | Break oplock (invalidate cache) |
| `CmpWaitForHiveOplockBreak` | Wait for oplock break completion |

---

## 13. State Separation

| Function | Purpose |
|----------|---------|
| `CmpUpdateStateSeparationHiveOptions` | Update hive options for state-separated OS |
| `CmpIsStateSeparatedHive` | Check if hive is state-separated |
| `CmpGetStateSeparationFlags` | Get state separation flags |

State separation allows the OS to run from a read-only base image with a writable data partition. Registry changes are redirected to the data partition while the base hive remains unchanged.

---

## 14. Machine Hive Loaded Notifications

| Function | Purpose |
|----------|---------|
| `CmRegisterMachineHiveLoadedNotification` | Register for hive-loaded notifications |
| `CmUnRegisterMachineHiveLoadedNotification` | Unregister hive-loaded notification |
| `CmpInvokeMachineHiveLoadedCallbacks` | Invoke hive-loaded callbacks |

---

## 15. Key Findings for Optimizer

1. **Lazy Flush Tuning**: `CmpLazyFlushIntervalInSeconds` (60s default) controls how often dirty registry pages are flushed to disk. Reducing this interval on systems with frequent power outages improves registry durability; increasing it reduces disk I/O. The boot delay (`CmpEnableLazyFlushBootDelayInterval`, 60s) prevents premature flush during boot.

2. **Periodic Backup**: `CmpPeriodicBackupFlushHiveCount` (7) controls how many hives get backed up between periodic flushes. RegFiles are stored in `C:\Windows\System32\config\RegBack`.

3. **KCB Cache Hit Rate**: `CmpKcbCacheHitCount` / `CmpKcbCacheMissCount` track cache effectiveness. A well-tuned system should have high hit rates; cache misses indicate either cold-start or excessive hive eviction.

4. **Callback Overhead**: `CmpCallBackCount` (5 on this system) shows the number of registered registry callbacks. Each callback adds latency to every registry operation. Disabling unnecessary antivirus/EDR registry scanning callbacks improves registry performance.

5. **Registry Quota**: `CmpGlobalQuota` (0xFFFFFFFF = unlimited) means no quota enforcement. For systems with limited storage (embedded/IoT), setting a quota prevents registry bloat.

6. **Hive Compaction**: `NtCompactKeys` defragments registry keys, reclaiming free space from deleted keys/values. Regular compaction improves lookup performance and reduces hive file size.

7. **Compressed Names**: Registry keys use compressed name format internally. `CmpCompareCompressedName` and `CmpHashCompressedComponent` are hot paths — optimizing these (already done in ARM64 NEON) improves overall registry throughput.

8. **Delayed Deref KCBs**: `CmpDelayDerefKCBList` and `CmpDelayDerefKCBWorker` implement lazy KCB cleanup. This amortizes the cost of KCB deletion across multiple operations, reducing lock contention.

9. **Virtual Store (UAC)**: `CmpVEExecuteVirtualStoreParseLogic` adds overhead for every HKLM\Software write by non-elevated processes. For optimizer scenarios, disabling UAC virtualization (or elevating processes) avoids this redirect.

10. **Layered Registry**: Servicing layers add lookup overhead via `CmpResolveLayerForKey`. After major OS updates, running `NtCompactKeys` consolidates layers and improves lookup speed.

11. **Hive Cache + Oplock**: `CmpHiveCachePopulateHiveEntry` uses oplocks for cache coherence. On systems with fast storage, the hive cache is less impactful; on HDD systems, it provides significant improvement.

12. **Flush Coalescing**: `CmpCoalesceFlush` batches multiple flush requests into a single I/O operation. This is critical for reducing disk writes during high-frequency registry update scenarios (e.g., performance counter updates).

13. **System Hive Hysteresis**: `CmpSystemHiveHysteresisHigh` / `CmpSystemHiveHysteresisLow` control system hive size growth. When the system hive exceeds the high watermark, cleanup is triggered; it continues until the low watermark is reached.

---

## 16. Registry Keys

```
HKLM\SYSTEM\CurrentControlSet\Control\hivelist
  - \Registry\Machine\HARDWARE          (REG_SZ: hive file path)
  - \Registry\Machine\SAM               (REG_SZ: hive file path)
  - \Registry\Machine\SECURITY          (REG_SZ: hive file path)
  - \Registry\Machine\SOFTWARE          (REG_SZ: hive file path)
  - \Registry\Machine\SYSTEM            (REG_SZ: hive file path)

HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Configuration Manager
  - LazyFlushInterval                   (REG_DWORD: seconds, default 60)
  - BackupCount                         (REG_DWORD: backup copies to keep)
  - EnableLazyFlushBootDelay            (REG_DWORD: boot delay seconds)
  - RegistryQuotaSize                   (REG_DWORD: max registry size in MB)

HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management
  - ClearPageFileAtShutdown             (REG_DWORD: clear page file on shutdown)
  - SessionPoolSize                     (REG_DWORD: session pool size)
  - SessionViewSize                     (REG_DWORD: session view size)

HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization
  - (UAC virtualization settings)
```

---

## 17. User-Mode APIs

| API | Purpose |
|-----|---------|
| `RegCreateKeyEx` | Create/open a registry key |
| `RegOpenKeyEx` | Open a registry key |
| `RegDeleteKey` / `RegDeleteKeyEx` | Delete a registry key |
| `RegDeleteTree` | Delete key and all subkeys |
| `RegSetValueEx` | Set a value |
| `RegQueryValueEx` | Query a value |
| `RegEnumKeyEx` | Enumerate subkeys |
| `RegEnumValue` | Enumerate values |
| `RegNotifyChangeKeyValue` | Register for change notification |
| `RegSaveKey` / `RegSaveKeyEx` | Save hive to file |
| `RegRestoreKey` | Restore hive from file |
| `RegReplaceKey` | Replace hive file |
| `RegLoadKey` / `RegLoadKey2` | Load a hive |
| `RegUnLoadKey` | Unload a hive |
| `RegFlushKey` | Force flush to disk |
| `RegCompactKeys` | Compact registry keys (Win10+) |
| `RegGetKeySecurity` | Get key security |
| `RegSetKeySecurity` | Set key security |
| `RegQueryInfoKey` | Query key information |
| `RegCopyTree` | Copy registry subtree |
| `RegGetValue` | Get value with type checking |
| `NtCreateKey` | Native: create key |
| `NtOpenKey` | Native: open key |
| `NtSetValueKey` | Native: set value |
| `NtQueryValueKey` | Native: query value |
| `NtDeleteKey` | Native: delete key |
| `NtFlushKey` | Native: flush key |
| `NtLoadKeyEx` | Native: extended hive load |
| `NtSaveKeyEx` | Native: extended hive save |
