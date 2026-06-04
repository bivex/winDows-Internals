# Network Stack API Research (Windows 11 ARM64 Build 26100)

> Kernel-debugged via WinDbg MCP on ARM64 target (Parallels VM)
> Modules: `tcpip` (fffff800`6a980000 - fffff800`6acb5000), `ndis` (fffff800`6a610000 - fffff800`6a786000), `NETIO` (fffff800`6a790000 - fffff800`6a83b000), `afd` (fffff800`6c950000 - fffff800`6ca0a000), `tcpipreg` (fffff800`6e220000), `nsiproxy` (fffff800`6bea0000)

---

## 1. NDIS — Network Driver Interface Specification

### 1.1 Receive Side Scaling (RSS)

RSS distributes incoming packets across multiple CPUs using Toeplitz hashing for parallel receive processing.

**Global Configuration (ndis):**

| Global | Value | Description |
|---|---|---|
| `ndis!ndisRssCpuCount` | 4 | Number of CPUs participating in RSS |
| `ndis!ndisMaxNumRssCpus` | 4 | Maximum RSS CPUs configured |
| `ndis!ndisRssBaseCpu` | 0 | Base CPU for RSS indirection table |
| `ndis!ndisLowPowerEpoch` | 0 | Low power epoch (0 = normal operation) |

**RSS v1 Functions (ndis):**

```
ndis!ndisSetMiniportRSSParameters       - Set RSS parameters on miniport adapter
ndis!ndisGetDefaultNumRssProcessors     - Get default RSS processor count
ndis!ndisPopulateRssProcessorSet        - Populate RSS processor set for adapter
ndis!ndisIsRssEnabledForMiniport        - Check if RSS is enabled for adapter
ndis!ndisEmulateRSSv1Dpc               - Emulate RSS v1 DPC for receive processing
ndis!NdisGetRssProcessorInformation     - Exported API: query RSS processor info
```

**RSS v2 Functions (ndis):**

```
ndis!ndisSetMiniportRSSv2Parameters     - Set RSS v2 parameters (per-queue indirection)
ndis!ndisRequestSetRSSv2IndirectionTable - Set RSS v2 indirection table
ndis!ndisValidateRSSv2Parameters         - Validate RSS v2 parameter changes
```

**RSS Distribution Functions (tcpip):**

```
tcpip!RssStartDistribution              - Start RSS packet distribution
tcpip!RssStopDistribution               - Stop RSS packet distribution
tcpip!RssInitializeIndirectionTable      - Initialize RSS indirection table
tcpip!RssPrepareReceiveScaleParameters   - Prepare receive scaling parameters
tcpip!RssUpdateAdapter                   - Update RSS state for adapter
tcpip!RssUpdateAllAdaptersUnderLock      - Update all adapters (under lock)
tcpip!RssHashComputeToeplitzHash         - Compute Toeplitz hash for packet
tcpip!RssGetProcessorAffinity            - Get target CPU affinity from hash
```

**User-Mode APIs:**

| API | Purpose |
|---|---|
| `GetRssProcessorInformation` (via NDIS) | Query RSS processor set and capabilities |
| `SetReceiveScaling` (via NDIS config) | Configure RSS indirection table |

---

### 1.2 DPC (Deferred Procedure Call) Handling

NDIS uses DPCs for deferred interrupt processing — packet reception and completion.

**DPC Functions (ndis):**

```
ndis!ndisInterruptDpc                   - Main interrupt DPC handler
ndis!ndisMiniportDpc                    - Miniport-specific DPC processing
ndis!ndisMDeferredDpc                   - Deferred DPC for batched processing
ndis!NdisMQueueDpc                      - Queue DPC for specific CPU (RSS-targeted)
ndis!NdisMQueueDpcEx                    - Extended DPC queuing with more control
ndis!ndisQueueDpcWorkItem               - Queue DPC as work item (for overflow)
ndis!ndisQueuedMiniportDpcWorkItem      - Process queued DPC work items
```

**Interrupt Registration:**

```
ndis!ndisRegisterInterrupt              - Register interrupt handler for miniport
ndis!ndisDeregisterInterrupt            - Deregister interrupt handler
ndis!ndisMiniportIsr                    - Miniport ISR (interrupt service routine)
ndis!ndisEnableInterrupt                - Enable interrupts on miniport
ndis!ndisDisableInterrupt               - Disable interrupts on miniport
```

---

### 1.3 NDIS Poll Mechanism

Modern interrupt mitigation replacing traditional ISR/DPC with busy-poll model.

```
ndis!ndisInitializePoll                 - Initialize poll context for miniport
ndis!ndisInvokePoll                     - Invoke poll handler (active polling)
ndis!ndisSetPollAffinity                - Set CPU affinity for poll instance
ndis!ndisArmPollNotification            - Arm notification for poll wake-up
ndis!ndisCompletePollNotification       - Complete poll notification
```

---

### 1.4 Packet Direct (PD)

Low-latency packet processing path bypassing standard NDIS stack.

```
ndis!ndisInitializePacketDirect         - Initialize Packet Direct path
ndis!ndisRegisterPacketDirectProvider   - Register as PD provider (miniport)
ndis!ndisRegisterPacketDirectClient     - Register as PD client (protocol)
ndis!ndisPdPostReceive                  - Post receive buffer to PD path
ndis!ndisPdPostSend                     - Post send buffer to PD path
ndis!ndisPdFlushReceiveQueue            - Flush PD receive queue
ndis!ndisPdFlushSendQueue               - Flush PD send queue
```

---

### 1.5 Power Management

NIC power management for energy efficiency — selective suspend and auto power saver.

**Selective Suspend:**

```
ndis!ndisSelectiveSuspendInitialize     - Initialize selective suspend for adapter
ndis!ndisSelectiveSuspendStop           - Stop selective suspend
ndis!ndisSelectiveSuspendEngage         - Engage selective suspend (idle detected)
ndis!ndisSelectiveSuspendComplete       - Complete selective suspend transition
ndis!ndisSelectiveSuspendWakeup         - Wake up from selective suspend
```

**NIC Auto Power Saver:**

```
ndis!ndisShouldEngageNicAutoPowerSaver  - Check if auto power saver should engage
ndis!ndisNicAutoPowerSaverControlIdleTimer - Control idle timer for power saver
ndis!ndisNicAutoPowerSaverEngage        - Engage NIC auto power saver
ndis!ndisNicAutoPowerSaverDisengage     - Disengage NIC auto power saver
```

**General Power:**

```
ndis!ndisMPowerPolicy                   - Miniport power policy handler
ndis!ndisPrepForLowPower                - Prepare adapter for low power state
ndis!ndisDevicePowerOn                  - Power on NIC device
ndis!ndisDevicePowerDown                - Power down NIC device
```

---

### 1.6 NBL (NET_BUFFER_LIST) Tracking

Packet lifecycle tracking with watchdog for stuck packets.

```
ndis!ndisTrackNbl                        - Track NBL for debugging/watchdog
ndis!ndisUntrackNbl                      - Untrack NBL (completion)
ndis!ndisNblStuckWatchdog                - Watchdog timer for stuck NBLs
ndis!ndisDumpStuckNbls                   - Dump info about stuck NBLs
```

---

## 2. TCPIP.SYS — TCP/IP Stack

### 2.1 RSS State (tcpip globals)

| Global | Value | Description |
|---|---|---|
| `tcpip!RssGloballyEnabled` | 1 | RSS is globally enabled |
| `tcpip!TcpipGlobalRscDisabledMask` | 0 | RSC not disabled (all interfaces) |

---

### 2.2 Receive Segment Coalescing (RSC)

RSC merges multiple TCP segments into fewer, larger buffers — reduces per-packet processing overhead.

```
tcpip!OlmTryToEnableRscOnInterface      - Enable RSC on network interface
tcpip!OlmDisableRscOnInterface           - Disable RSC on network interface
tcpip!TcpQueryRscPerformanceCounters     - Query RSC performance counters
tcpip!RscCoalesceSegments                - Core coalescing logic
tcpip!RscValidatePacket                  - Validate packet for RSC eligibility
```

**Registry Control:**
- `HKLM\System\CurrentControlSet\Services\Tcpip\Parameters\EnableRsc` — global RSC toggle
- Per-adapter: `*RscIPv4`, `*RscIPv6` advanced properties

---

### 2.3 UDP Receive Offload (URO)

UDP-specific receive coalescing (analogous to RSC for UDP).

```
tcpip!UdpOffloadIsUroAllowed             - Check if URO is allowed
tcpip!UdpOffloadSetUro                   - Enable/disable URO on interface
tcpip!UdpOffloadReceive                  - Process URO-received packets
```

| Global | Description |
|---|---|
| `tcpip!UroDisabledMask` | Bitmask of interfaces where URO is disabled |

---

### 2.4 IPSec Offload

Hardware-accelerated IPsec crypto operations.

```
tcpip!IPSecAttemptOffload                - Attempt to offload IPsec to hardware
tcpip!IPSecCanOffloadV2                  - Check V2 offload capability
tcpip!IPSecOffloadDeleteSa               - Delete offloaded SA
tcpip!IPSecOffloadGetSpi                 - Get SPI for offloaded SA
tcpip!IPSecOffloadUpdateSa               - Update offloaded SA
```

---

### 2.5 Enterprise QoS (EQoS)

Policy-based Quality of Service with DSCP marking, throttling, and URL-based policies.

**Auto-Tuning Integration:**

```
tcpip!EQoSpProcessTcpAutoTuningSettings  - Process TCP auto-tuning from QoS policy
tcpip!EQoSpParseTcpAutoTuningSetting     - Parse auto-tuning setting value
tcpip!TcpRcvWndScaleFactorFromAutoTuningLevel - Map auto-tuning level to window scale
```

**Auto-Tuning Levels (tcpip!TcpAutotuningLevelInfo):**

| Level | Window Scale | Description |
|---|---|---|
| Disabled | 0 | Fixed 64KB window |
| HighlyRestricted | 1 | Very conservative growth |
| Restricted | 2 | Conservative growth |
| Normal | 3 | Default — dynamic scaling |
| Experimental | 4 | Aggressive scaling |

**Policy Matching:**

```
tcpip!EQoSCompareNetPolicyPriority       - Compare QoS policy priorities
tcpip!EQoSMatchNetPolicy                 - Match network policy for connection
tcpip!EQoSApplyDscpMarking               - Apply DSCP marking to packet
tcpip!EQoSApplyThrottleRate              - Apply throttling rate
```

**QoS Inspection:**

```
tcpip!QimInspectSetQoS                   - QoS inspection module: set QoS
tcpip!QimMatchPolicyForTCPConnection     - Match QoS policy for TCP connection
tcpip!QimMatchPolicyForUDPFlow           - Match QoS policy for UDP flow
```

---

### 2.6 TCP Timer Wheels

Per-connection timer management using hierarchical timer wheels with global DPC.

```
tcpip!TcpArmGlobalTimer                  - Arm global TCP timer
tcpip!TcpGlobalTimeoutHandler            - Global timeout DPC handler
tcpip!TcpProcessExpiredTcbTimers         - Process expired TCB timers
tcpip!TcpRepartitionTimerWheels          - Repartition timer wheels across CPUs
tcpip!TcpInsertTimerWheelEntry           - Insert entry into timer wheel
tcpip!TcpRemoveTimerWheelEntry           - Remove entry from timer wheel
```

| Global | Description |
|---|---|
| `tcpip!TcpGlobalTimerExpirationTicks` | Global timer tick configuration |

---

### 2.7 Congestion Control

Windows supports multiple congestion control algorithms selectable per-connection.

**CCM (Compound Congestion Management):**

```
tcpip!TcpCcmIncreaseCwndInCongestionAvoidance  - CCM congestion avoidance increase
tcpip!TcpCcmIncreaseCwndInSlowStart             - CCM slow start increase
tcpip!TcpCcmOnPacketLoss                         - CCM packet loss reaction
tcpip!TcpCcmOnDataAck                            - CCM data acknowledgment
```

**BBRv2 (Bottleneck Bandwidth and RTT):**

```
tcpip!Bbr2SetCongestionState             - Set BBRv2 congestion state
tcpip!Bbr2UpdateModel                    - Update BBRv2 bandwidth/RTT model
tcpip!Bbr2UpdateBtlBw                    - Update bottleneck bandwidth estimate
tcpip!Bbr2UpdateRtProp                   - Update RTT propagation estimate
```

**DCTCP (Data Center TCP):**

```
tcpip!DctcpUpdateEcnAlpha                 - Update ECN alpha (DCTCP weight)
tcpip!TcpTcbEcnProcessCwndReduction      - Process ECN-based cwnd reduction
tcpip!DctcpUpdateBytesUntilNewAlpha       - Update byte counter for alpha
```

**General Congestion:**

```
tcpip!TcpTcbChangeCongestionState         - Change TCB congestion state
tcpip!TcpCubicUpdate                      - CUBIC algorithm update
tcpip!TcpLedbatUpdate                     - LEDBAT low-priority update
```

---

### 2.8 Send/Receive Path

Core packet processing functions in tcpip.

**Receive Path:**

```
tcpip!TcpReceive                         - Main TCP receive handler
tcpip!TcpDeliverData                     - Deliver data to application
tcpip!TcpIndicateData                    - Indicate received data up the stack
tcpip!IppReceiveHeader                   - IP header receive processing
tcpip!IppReceiveDatagram                 - IP datagram receive processing
```

**Send Path:**

```
tcpip!TcpSend                            - Main TCP send handler
tcpip!TcpTcbSendData                     - Send data from TCB
tcpip!IppSendDatagram                    - IP datagram send
tcpip!IppRouteAndSendPacket              - Route and send IP packet
```

---

## 3. NETIO — Network I/O Subsystem

NETIO provides the QoS flow infrastructure used by EQoS and packet classification.

### 3.1 QoS Flow Management

```
NETIO!NetioCreateQoSFlow                 - Create QoS flow object
NETIO!NetioDeleteQoSFlow                 - Delete QoS flow object
NETIO!NetioAssociateQoSFlowWithNbl       - Associate QoS flow with NBL (packet)
NETIO!NetioGetStatsForQoSFlow            - Query statistics for QoS flow
NETIO!NetioUpdateQoSFlow                 - Update QoS flow parameters
NETIO!NetioDereferenceQoSFlow            - Dereference QoS flow (release)
```

### 3.2 QoS Provider/Client

```
NETIO!QoSProviderContext                  - QoS provider context management
NETIO!QoSClientHandle                    - QoS client handle management
NETIO!NetioRegisterQoSProvider           - Register QoS provider
NETIO!NetioDeregisterQoSProvider         - Deregister QoS provider
```

---

## 4. Optimization APIs Summary

### User-Mode / Registry APIs for Network Optimization

| API / Setting | Module | Purpose |
|---|---|---|
| `Netsh int tcp set global rss=enabled` | ndis/tcpip | Enable/disable RSS |
| `Netsh int tcp set global rssprofile=strict/numa/closest` | tcpip | RSS profile for CPU distribution |
| `Netsh int tcp set global rsc=enabled` | tcpip | Enable/disable RSC |
| `Netsh int tcp set global autotuninglevel=normal` | tcpip | TCP receive window auto-tuning |
| `Netsh int tcp set global congestionprovider=cubic/bbr2` | tcpip | Congestion control algorithm |
| `Netsh int tcp set global ecncapability=enabled` | tcpip | ECN (Explicit Congestion Notification) |
| `Netsh int tcp set global timestamps=enabled` | tcpip | TCP timestamps |
| `Netsh int tcp set global chimney=enabled` | tcpip | TCP Chimney Offload |
| `Netsh int tcp set global dca=enabled` | tcpip | Direct Cache Access |
| `Netsh int tcp set global netdma=enabled` | tcpip | Network DMA |
| `Netsh int tcp set global taskoffload=enabled` | tcpip | Task offload (checksum, LSO, etc.) |
| `Set-NetAdapterRss` | PowerShell | Per-adapter RSS configuration |
| `Set-NetAdapterRsc` | PowerShell | Per-adapter RSC configuration |
| `Set-NetAdapterAdvancedProperty` | PowerShell | Advanced adapter properties |

### Registry Keys for Tuning

| Key | Path | Description |
|---|---|---|
| `TcpAckFrequency` | `Tcpip\Parameters\Interfaces\{GUID}` | ACK frequency (1 = immediate) |
| `TCPNoDelay` | `Tcpip\Parameters\Interfaces\{GUID}` | Disable Nagle algorithm |
| `TcpDelAckTicks` | `Tcpip\Parameters\Interfaces\{GUID}` | Delayed ACK timer (ticks) |
| `Tcp1323Opts` | `Tcpip\Parameters` | Window scaling + timestamps (0=off, 1=ws, 2=ts, 3=both) |
| `MaxFreeTcbs` | `Tcpip\Parameters` | Maximum free TCBs |
| `MaxHashTableSize` | `Tcpip\Parameters` | TCP connection hash table size |
| `MaxUserPort` | `Tcpip\Parameters` | Maximum ephemeral port number |
| `TcpTimedWaitDelay` | `Tcpip\Parameters` | TIME_WAIT delay (seconds) |
| `EnableWsd` | `Tcpip\Parameters` | Winsock Direct (datacenter) |
| `NicAutoPowerSaverEnabled` | NIC advanced property | NIC auto power save toggle |
| `SelectiveSuspendEnabled` | NIC advanced property | Selective suspend toggle |
| `*RSS` | NIC advanced property | Per-adapter RSS toggle |
| `*RscIPv4` / `*RscIPv6` | NIC advanced property | Per-adapter RSC toggle |
| `*UsoIPv4` / `*UsoIPv6` | NIC advanced property | UDP Send Offload |

### Kernel Objects for Network Optimization

| Object | Module | Use |
|---|---|---|
| `NDIS_MINIPORT_BLOCK` | ndis | Per-adapter state (RSS, power, DPC) |
| `NDIS_INTERRUPT` | ndis | Interrupt/DPC management per adapter |
| `TCB` (Transmission Control Block) | tcpip | Per TCP connection state |
| `TCP_ENDPOINT` | tcpip | TCP endpoint (listener/connector) |
| `IP_INTERFACE` | tcpip | IP interface state |
| `IP_ROUTE` | tcpip | Route entry |
| `QOS_FLOW` | NETIO | QoS flow tracking |
| `NBL` (NET_BUFFER_LIST) | ndis | Packet descriptor |

---

## 5. Key Findings for Optimizer

1. **RSS is fully controllable** — CPU count, indirection table, hash function, and profile (strict/NUMA/closest) all configurable via netsh and `Set-NetAdapterRss`
2. **RSC reduces CPU overhead** significantly by merging TCP segments — verify it's enabled on all adapters
3. **TCP auto-tuning level** is the single most impactful setting for throughput — `normal` or `experimental` for high-bandwidth links
4. **Congestion control** is selectable (CUBIC default, BBRv2 available) — BBRv2 better for high-BDP networks
5. **Selective suspend and NicAutoPowerSaver** can add latency — disable for gaming/real-time workloads
6. **EQoS/DSCP** policies can throttle connections — audit with `netsh advfirewall` and Group Policy
7. **NBL stuck watchdog** can detect network hangs — monitor via performance counters
8. **NDIS Poll** is the modern path for low-latency networking — drivers supporting it bypass ISR/DPC overhead
9. **Packet Direct (PD)** is the ultimate low-latency path — bypasses NDIS stack entirely, used by DPDK-like workloads
10. **UDP Receive Offload (URO)** is the UDP equivalent of RSC — verify it's enabled for UDP-heavy workloads
11. **IPSec offload** moves crypto to NIC — critical for VPN performance
12. **TCP timer wheels** govern retransmit behavior — `TcpGlobalTimerExpirationTicks` is the key global
