# NativeAOT Security Surface Analysis PoC

Self-contained security auditor compiled as a NativeAOT executable. Runs **inside the analyzed process** and verifies research claims from:
📄 **[`KERNEL/RoP_NativeAOT_Research.md`](../../KERNEL/RoP_NativeAOT_Research.md)**

---

## What It Verifies

| Check | Research Claim | Expected Result |
|---|---|---|
| **1. PE Security Flags** | AOTSEC-01: No CFG | `DllCharacteristics: 0x8160` (bit `0x4000` absent) |
| **2. Memory Regions** | Module data sections writable | `PAGE_READWRITE` regions present |
| **3. Gadget Scan** | AOTSEC-03: Abundant ret gadgets | ~400+ in 64KB sample |
| **4. RWX Scan** | AOTSEC-03: Zero RWX regions | `Total RWX regions: 0` |

---

## Build & Run

```bash
dotnet publish -r win-arm64 -c Release
# Output: ./bin/Release/net10.0/win-arm64/publish/TestAotSec.exe
```

Or from source:
```bash
dotnet run
```

---

## Expected Output

```
=== NativeAOT Security Surface Analysis PoC ===

Module Base: 0x7FF6D60A0000

--- CHECK 1: PE Security Flags ---
  DllCharacteristics: 0x8160
  ASLR (0x0040):          ENABLED
  DEP (0x0100):           ENABLED
  HighEntropyVA (0x0020): ENABLED
  CFG (0x4000):           !!! DISABLED !!!
  [FINDING] No CFG = unvalidated indirect branches (AOTSEC-01)

--- CHECK 2: Memory Region Protections ---
  Region 0: 0x7FF6D60A0000 size=0x1000  prot=PAGE_READONLY
  Region 1: 0x7FF6D60A1000 size=0xB4000 prot=PAGE_EXECUTE_READ   ← .text (739KB!)
  Region 3: 0x7FF6D619D000 size=0x1000  prot=PAGE_READWRITE      ← .data (writable)
  Region 5: 0x7FF6D61AC000 size=0x3000  prot=PAGE_READWRITE      ← more writable data
  ...

--- CHECK 3: ARM64 ret Gadget Scan ---
  Found: 436 ret instructions in 64KB
  Density: 6.81 ret/KB
  Estimated total for 553KB .text: ~3767 gadgets

--- CHECK 4: Process-Wide RWX Regions ---
  Total RWX regions: 0
  [SECURE] Zero RWX = all code AOT compiled (confirms AOTSEC-03)

=== Analysis Complete ===
```

---

## Key Findings Demonstrated

1. **CFG disabled** — the PE header confirms bit `0x4000` is absent, meaning all indirect branches execute without kernel validation.

2. **Massive gadget surface** — 436 `ret` instructions in a 64KB sample (6.81/KB density). The full `.text` section contains an estimated **3,767 usable gadget entry points**.

3. **Writable data sections** — `PAGE_READWRITE` regions at known offsets contain function pointers (interface dispatch cells, GC globals) that can be overwritten.

4. **Zero RWX** — no memory is simultaneously writable and executable, confirming NativeAOT's advantage over C++/CLI (which has a 12KB RWX thunk heap).

---

## Files

| File | Description |
|---|---|
| `Program.cs` | Main security auditor — reads own PE header, scans memory, counts gadgets |
| `TestAotSec.csproj` | Project file with `PublishAot=true` for NativeAOT compilation |
