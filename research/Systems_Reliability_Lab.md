# Systems Reliability Lab: Windows Kernel Hardware Error Architecture & Data Integrity

## 1. Master Systems Reliability Architecture

```
Physical World
(Thermal Noise, Cosmic Rays, Semiconductor Aging)
            │
            ▼
┌───────────────────────────────────────────────────────────┐
│ 1. ECC / Hardware Level                                   │
│    RAM (Hamming SECDED), CPU (MCA), Bus Controllers       │
└───────────────────────────────────────────────────────────┘
            │
            ▼
┌───────────────────────────────────────────────────────────┐
│ 2. WHEA (Windows Hardware Error Architecture)             │
│    Diagnostics & CPER Reports (CMC, MCE, PCIe AER)        │
└───────────────────────────────────────────────────────────┘
            │
            ▼
┌───────────────────────────────────────────────────────────┐
│ 3. Memory Manager (Page Offlining)                        │
│    PFN Masking, nt!MmMarkPhysicalMemoryAsBad, BCD          │
└───────────────────────────────────────────────────────────┘
            │
            ▼
┌───────────────────────────────────────────────────────────┐
│ 4. Storage & Integrity                                    │
│    ReFS (CRC32c / armv8_crc32_pmull_little), Parity       │
└───────────────────────────────────────────────────────────┘
            │
            ▼
       User & Application Space
```

---

## 2. Core Subsystem Responsibilities

| Subsystem | Core Question Answered | Mechanism & Kernel Symbol | Recovery Timeframe |
|---|---|---|---|
| **ECC RAM** | *"Can we fix a single flipped bit in RAM instantly?"* | Hamming SECDED (64b + 8b parity) | Hardware Nanoseconds |
| **WHEA** | *"What happened, where did it occur, and how critical is it?"* | `nt!_WHEA_ERROR_RECORD_HEADER`, CPER | Microseconds |
| **Memory Manager** | *"How to prevent this bad RAM frame from ever being allocated again?"* | `nt!WheapAttemptPhysicalPageOffline`, `nt!MmMarkPhysicalMemoryAsBad` | Immediate & BCD Persistent |
| **ReFS / Storage** | *"How to detect silent data corruption and recover the bad block?"* | `nt!RtlpCrc32c`, `nt!armv8_crc32_pmull_little`, Parity Streams | Milliseconds |

---

## 3. Disassembly & WinDbg Kernel Verification

### A. Memory Page Offlining (`nt!WheapAttemptPhysicalPageOffline`)

Captured from Windows 11 ARM64 Kernel (`ntoskrnl.exe`):

```assembly
; PFN to Physical Address translation
lsl  x0, x23, #0xC    ; PhysicalAddress = PFN << 12 (4 KB Page Align)
mov  x8, #0x1000      ; Page Size = 4096 bytes

; Mark Memory Frame as Bad in Memory Manager
bl   nt!MmMarkPhysicalMemoryAsBad

; Persist Bad Page across reboots
cmp  w8, #1
bne  WheapAttemptPhysicalPageOffline+0x158
bl   nt!WheaPersistBadPageToBcd         ; Store in BCD Bootloader Configuration
bl   nt!WheaPersistBadPageToRegistry    ; Store in System Registry
```

### B. WHEA Error Severity Enum (`nt!_WHEA_ERROR_SEVERITY`)

```text
0: kd> dt nt!_WHEA_ERROR_SEVERITY
   WheaErrSevRecoverable   = 0n0    ; Non-fatal, page offline possible
   WheaErrSevFatal         = 0n1    ; Unrecoverable -> BugCheck 0x124
   WheaErrSevCorrected     = 0n2    ; Corrected by Hardware (Single-Bit ECC)
   WheaErrSevInformational = 0n3    ; Audit logging
```

### C. Hardware-Accelerated Checksums (`nt!RtlpCrc32c`)

```text
0: kd> x nt!*Crc32*
fffff802`abd0aa10 nt!RtlpCrc32c
fffff802`abd1aac0 nt!armv8_crc32_pmull_little
```

---

## 4. Four Principles of Systems Self-Healing

1. **Detection:** Hardware or filesystem detects inconsistency via checksum or parity bit.
2. **Localization:** Precise isolation of PFN (Page Frame Number) or storage block LBA.
3. **Isolation:** Soft blacklisting (`BadPageList`) and persistent BCD blacklisting.
4. **Restoration:** Self-healing via Storage Spaces parity stream or backup fetch.
