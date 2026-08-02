# Fast Branchless ECC Library (`ecc_branchless.hpp`)

High-performance, C++11 header-only library providing branchless Hamming(7,4) Single Error Correction (SEC) with architecture-aware ARM NEON acceleration and software pipelining.

## ✨ Features

- **Header-Only C++11 Library**: Zero external dependencies, single include.
- **Multi-Architecture Dispatch**: ARM64 NEON (128-bit SIMD), x86 AVX2 (256-bit SIMD), x86 SSE4.1, and Portable Scalar.
- **Software Pipelining & x8 Register Unrolling**: Process 128 codewords per loop iteration with interleaved instruction scheduling.
- **Branchless Decoding Implementation**: Data-dependent branches eliminated from core error correction paths.
- **Register-Loaded VTBL Acceleration**: 1-cycle table lookups loaded into registers outside the loop.
- **Hardware Cache Prefetching**: Inline `PRFM PLDL1KEEP` cache line prefetching.
- **Zero Dynamic Allocations**: Stack-allocated scalar primitives (`encode4`, `decode4`, `encodeByte`, `decodeByte`).
- **Comprehensive Test Suite**: Automated unit tests, edge-case checks, 0-leak MSVC CRT / ASan checks, and assembly dump inspection.

## 📁 Directory Structure

```text
fast-ecc-branchless/
├── include/
│   └── ecc_branchless.hpp         # Header-only library v2.0.0 (SIMD Dispatch + Unroll x8)
├── examples/
│   ├── basic_ecc.cpp               # Basic single-nibble example
│   └── stream_transmission.cpp     # Full string/buffer stream recovery
├── tests/
│   ├── test_ecc.cpp                # Automated unit test suite (112 test cases)
│   ├── test_edge_cases.cpp         # Edge cases & 1 MB high-res benchmark
│   ├── test_memleak.cpp            # MSVC CRT & ASan 0-leak memory check
│   ├── test_max_throughput.cpp     # 64 MB Multi-Pass Peak Throughput benchmark
│   └── test_simd_ecc.cpp           # Pipelined Unroll x8 NEON Benchmark
├── ecc_data_transmission_demo.cpp  # Real-time noise channel demo
├── CMakeLists.txt                  # Cross-platform CMake build configuration
├── README.md                       # Documentation & Benchmark Breakdown
├── Systems_Reliability_Lab.md      # Architecture research document
└── Systems_Reliability_Lab.tex     # Academic LaTeX paper (Ukrainian)
```

## 🚀 Quick Start

Include `ecc_branchless.hpp` directly into your C++ project:

```cpp
#include "include/ecc_branchless.hpp"
#include <iostream>

int main() {
    // Query detected SIMD architecture
    std::cout << "Detected SIMD: " << (int)ecc::FastHamming74::getDetectedArchitecture() << "\n";

    // 1. Encode 4-bit data (0b1011 = 11)
    uint8_t codeword = ecc::FastHamming74::encode4(0b1011);

    // 2. Simulate hardware bit flip (Bit #3)
    uint8_t corrupted = codeword ^ (1 << 2);

    // 3. Decode & Auto-Correct without data-dependent branches
    bool corrected = false;
    uint8_t restored = ecc::FastHamming74::decode4(corrupted, corrected);

    std::cout << "Restored Payload: " << (int)restored << " (Corrected: " << (corrected ? "YES" : "NO") << ")\n";
    return 0;
}
```

## 📊 Benchmark & Performance Qualification

> **Benchmarking Environment Disclaimer:**  
> Measured on **Windows 11 ARM64 (Build 26100, MSVC /O2)** running on Apple Silicon hypervisor. Benchmark timing results will vary depending on hardware architecture, compiler version, optimization flags, cache hierarchy, memory subsystem, and workload thermal throttling.

| Execution Mode / Scope | Throughput (MB/sec) | Network Bitrate | Latency | Classification |
|---|---|---|---|---|
| **Single 4-bit Nibble ECC** | **240,000,000 ops / sec** | — | **4.16 ns / op** | Measured (MSVC /O2) |
| **Scalar Engine (1 ARM64 Core)** | **200.86 MB / sec** | **1.60 Gbps** | **4.97 ns / byte** | Measured (Cold/Warm) |
| **Pipelined Unroll x8 NEON** | **266.02 MB / sec** | **2.08 Gbps** | **3.75 ns / byte** | Measured (1.32x Speedup) |
| **Multi-Threaded Stream (8 CPU Cores)** | **~2,120.00 MB / sec** | **16.96 Gbps** | **< 0.5 ns / byte** | Projected (Linear Scale) |
| **Server 16-Core Peak (AVX-512 / SVE2)** | **~9,600.00 MB / sec** | **76.80 Gbps** | **Vector Parallel** | Theoretical Estimate |

### Technical Assembly Inspection (`cl /O2 /FAcs`)
- **Inline Assembly Verification:** MSVC ARM64 compiler produces 100% inlined `decodeSIMD16` vector routines with 0 stack spills.
- **Hardware Cache Prefetching:** Verified `prfm PLDL1KEEP, [x6, #0x100]` instructions inside the primary unrolled loop.
- **Register Allocation:** All 8 vector registers (`c0..c7`) are held entirely in hardware NEON registers without RAM memory roundtrips.

## ⚙️ Supported Environment

- **Architectures**: ARM64 (NEON acceleration), x86-64 / x86 (AVX2 / SSE4.1 / Scalar), ARMv7/v8.
- **Compilers**: MSVC 2019/2022/2026, GCC 7.0+, Clang 8.0+.
- **Thread Safety**: 100% Thread-safe (All core primitives are stateless and thread-reentrant).
- **Memory Allocation**: Zero dynamic allocations in `encode4`, `decode4`, `encodeByte`, `decodeByte`, `decodeSIMD16`.

## 🛠️ Building Examples & Tests

### Windows MSVC (ARM64 / x64)

```cmd
vcvarsall.bat arm64
cl /EHsc /O2 /Fe:test_simd_ecc.exe tests\test_simd_ecc.cpp
test_simd_ecc.exe
```

### CMake Build (Cross-Platform)

```bash
mkdir build && cd build
cmake ..
cmake --build .
ctest --output-on-failure
```
