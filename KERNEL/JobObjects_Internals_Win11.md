# Job Objects & Silos Internals — Windows 11 Kernel Architecture

**Kernel Target:** Windows 11 ARM64 / x64 (Build 26100)  
**Verified via:** WinDbg Kernel Debugger (`ntkrnlmp.exe`)  
**Scope:** Resource Control, Process Limits, Hierarchies, and Container Silos (`_EJOB` / `_ESERVERSILO_GLOBALS`)

---

## 1. Overview & Architecture

**Job Objects** in the Windows Executive subsystem (`_EJOB`) are the native Windows kernel equivalent of Linux **Control Groups (cgroups v2)**. They allow one or more processes to be managed, accounted for, and restricted as a single logical unit.

```
┌─────────────────────────────────────────────────────────────────┐
│                        _EJOB (Root Job)                         │
│  - LimitFlags: ActiveProcessLimit, ProcessMemoryLimit, etc.     │
│  - CpuRateControl / IoRateControl / NetRateControl              │
└─────────────────────────────────────────────────────────────────┘
         │                                       │
         ▼                                       ▼
┌─────────────────┐                     ┌─────────────────┐
│   _EPROCESS     │                     │  _EJOB (Child)  │
│  Job -> _EJOB   │                     │  ParentJob ->   │
└─────────────────┘                     └─────────────────┘
                                                 │
                                                 ▼
                                        ┌─────────────────┐
                                        │   _EPROCESS     │
                                        │  Job -> _EJOB   │
                                        └─────────────────┘
```

When a process is assigned to a Job Object, the kernel sets the `Job` pointer inside its `_EPROCESS` structure to point to the `_EJOB` instance.

---

## 2. Core Kernel Data Structures (`_EJOB`)

Verified layout of `nt!_EJOB` on Windows 11 ARM64 (Build 26100), total size **`0x728` bytes**:

```text
struct nt!_EJOB (Size: 0x728 bytes)
   +0x000 Event            : _KEVENT
   +0x018 JobLinks         : _LIST_ENTRY
   +0x028 ProcessListHead  : _LIST_ENTRY
   +0x038 JobLock          : _ERESOURCE
   +0x0a0 TotalUserTime    : _LARGE_INTEGER
   +0x0a8 TotalKernelTime  : _LARGE_INTEGER
   +0x0b0 TotalCycleTime   : _LARGE_INTEGER
   +0x0d4 TotalProcesses   : Uint4B
   +0x0d8 ActiveProcesses  : Uint4B
   +0x100 LimitFlags       : Uint4B
   +0x104 ActiveProcessLimit : Uint4B
   +0x108 Affinity         : _KAFFINITY_EX
   +0x2b0 ProcessMemoryLimit : Uint8B
   +0x2b8 JobMemoryLimit   : Uint8B
   +0x2c0 JobTotalMemoryLimit : Uint8B
   +0x2d8 EffectiveAffinity : _KAFFINITY_EX
   +0x4d8 CpuRateControl   : Ptr64 _JOB_CPU_RATE_CONTROL
   +0x4f8 SiblingJobLinks  : _LIST_ENTRY
   +0x508 ChildJobListHead : _LIST_ENTRY
   +0x518 ParentJob        : Ptr64 _EJOB
   +0x520 RootJob          : Ptr64 _EJOB
   +0x5bc JobId            : Uint4B
   +0x5c0 ContainerId      : _GUID
   +0x5e0 ServerSiloGlobals : Ptr64 _ESERVERSILO_GLOBALS
   +0x608 NetRateControl   : Ptr64 _JOB_NET_RATE_CONTROL
   +0x610 JobFlags         : Uint4B (Bitfield)
   +0x610 CpuRateControlActive : Pos 5, 1 Bit
   +0x610 IoRateControlActive  : Pos 27, 1 Bit
   +0x610 Silo                 : Pos 30, 1 Bit
   +0x638 IoRateControlHeader : _JOB_RATE_CONTROL_HEADER
   +0x6a0 VolumeIoControlTree : _RTL_RB_TREE
```

### Key Field Breakdown

| Offset | Field Name | Type | Function |
|---|---|---|---|
| `+0x028` | `ProcessListHead` | `_LIST_ENTRY` | Doubly-linked list of member `_EPROCESS` structures |
| `+0x104` | `ActiveProcessLimit` | `Uint4B` | Max active processes allowed in the job (`pids.max`) |
| `+0x2b0` | `ProcessMemoryLimit` | `Uint8B` | Hard memory limit per process (`JOB_OBJECT_LIMIT_PROCESS_MEMORY`) |
| `+0x2b8` | `JobMemoryLimit` | `Uint8B` | Hard total memory limit for entire job (`memory.max`) |
| `+0x4d8` | `CpuRateControl` | `_JOB_CPU_RATE_CONTROL*` | CFS-like CPU rate limits (Hard cap / Soft cap / Weight) |
| `+0x508` | `ChildJobListHead` | `_LIST_ENTRY` | List of child job objects (Nested Jobs hierarchy) |
| `+0x518` | `ParentJob` | `_EJOB*` | Pointer to parent job in hierarchy |
| `+0x5e0` | `ServerSiloGlobals` | `_ESERVERSILO_GLOBALS*` | Silo context for Windows Containers |
| `+0x608` | `NetRateControl` | `_JOB_NET_RATE_CONTROL*` | Network bandwidth throttling |
| `+0x610:30` | `JobFlags.Silo` | `1 Bit` | Flag set to `1` if Job is an isolated Silo Container |
| `+0x6a0` | `VolumeIoControlTree` | `_RTL_RB_TREE` | Red-black tree for per-volume IOPS / MBps throttling |

---

## 3. Comparison: Windows `_EJOB` vs Linux `cgroups v2`

| Feature / Resource | Linux `cgroups v2` | Windows Kernel (`_EJOB`) |
|---|---|---|
| **Process Assignment** | `task_struct.cgroups` | `_EPROCESS.Job` (`+0x...`) |
| **Max Process Limit** | `pids.max` | `_EJOB.ActiveProcessLimit` |
| **Memory Throttling** | `memory.max`, `memory.high` | `ProcessMemoryLimit`, `JobMemoryLimit` |
| **CPU Rate Limits** | `cpu.max` (CFS quota/period) | `_JOB_CPU_RATE_CONTROL` (Rate / Weight) |
| **CPU Core Pinning** | `cpuset.cpus` | `_EJOB.Affinity` / `EffectiveAffinity` |
| **Disk I/O Limits** | `io.max` (BFQ / blk-iocost) | `VolumeIoControlTree` / `IoRateControlHeader` |
| **Network Throttling** | `net_cls`, `tc` | `NetRateControl` |
| **Tree Hierarchy** | Virtual directory tree | `ParentJob` $\rightarrow$ `ChildJobListHead` |
| **Container Isolation** | Namespaces (PID, Mount, Net) | **Silos** (`ServerSilo` / `AppSilo` via `_ESERVERSILO_GLOBALS`) |

---

## 4. Win32 & Native API Interface

### User-Mode Win32 API

```c
#include <windows.h>

// 1. Create Job Object
HANDLE hJob = CreateJobObjectW(NULL, L"MyCustomJob");

// 2. Configure Limits
JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = { 0 };
limits.BasicLimitInformation.LimitFlags = 
    JOB_OBJECT_LIMIT_ACTIVE_PROCESS | 
    JOB_OBJECT_LIMIT_JOB_MEMORY;

limits.BasicLimitInformation.ActiveProcessLimit = 10;     // pids.max = 10
limits.JobMemoryLimit = 512 * 1024 * 1024;                // memory.max = 512MB

SetInformationJobObject(
    hJob, 
    JobObjectExtendedLimitInformation, 
    &limits, 
    sizeof(limits)
);

// 3. Assign Target Process
AssignProcessToJobObject(hJob, hProcess);
```

### Native API (NTDLL)

In kernel / low-level Native API, Job Object management is handled via:
- `NtCreateJobObject(PHANDLE JobHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes)`
- `NtAssignProcessToJobObject(HANDLE JobHandle, HANDLE ProcessHandle)`
- `NtSetInformationJobObject(HANDLE JobHandle, JOBOBJECTINFOCLASS JobObjectInformationClass, PVOID JobObjectInformation, ULONG JobObjectInformationLength)`

---

## 5. Windows Containers & Silos (`ServerSilo` / `AppSilo`)

Windows Containers (Docker on Windows, Defender Application Guard, WSL2 Silos) extend `_EJOB` into **Silos**:

1. **ServerSilo:** Server-level isolation. Created when `JobFlags.Silo == 1`. Provides a completely separate Object Manager namespace (`\Silos\N\`), separate registry hive views, and dedicated Network Compartment IDs (`COMPARTMENT_ID`).
2. **AppSilo / AppContainer:** Lightweight isolation for UWP and sandboxed Win32 applications.

When a thread in a Silo calls system services, the kernel inspects `_KTHREAD.Silo` / `_EJOB.ServerSiloGlobals` to route object lookups into the Silo's isolated namespace instead of the root global namespace.
