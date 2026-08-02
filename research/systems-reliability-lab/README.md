# Fast Branchless ECC Library (`ecc_branchless.hpp`)

High-performance, C++11 header-only library providing branchless Hamming(7,4) Single Error Correction (SEC) with sub-5ns latency and architecture-aware ARM NEON VTBL acceleration.

## ✨ Features

- **Header-Only C++11 Library**: Zero external dependencies, single include.
- **Branchless Decoding Implementation**: Data-dependent branches eliminated from core error correction paths.
- **Architecture-Aware Acceleration**: ARM64 NEON 128-bit vectorization with register-loaded VTBL lookups and cache prefetching.
- **Zero Heap Allocations**: Stack-allocated scalar primitives (`encode4`, `decode4`, `encodeByte`, `decodeByte`).
- **Cross-Platform Compatibility**: MSVC, GCC, and Clang support across Windows, macOS, and Linux.
- **Comprehensive Test Suite**: Includes automated unit tests, edge-case checks, 0-leak CRT memory checks, and multi-pass benchmarks.

## 📁 Directory Structure

```text
fast-ecc-branchless/
├── include/
│   └── ecc_branchless.hpp         # Header-only library (Branchless + NEON VTBL + Prefetch)
├── examples/
│   ├── basic_ecc.cpp               # Basic single-nibble example
│   └── stream_transmission.cpp     # Full string/buffer stream recovery
├── tests/
│   ├── test_ecc.cpp                # Automated unit test suite (112 test cases)
│   ├── test_edge_cases.cpp         # Edge cases & 1 MB high-res benchmark
│   ├── test_memleak.cpp            # MSVC CRT & ASan 0-leak memory check
│   ├── test_max_throughput.cpp     # 64 MB Multi-Pass Peak Throughput benchmark
│   └── test_simd_ecc.cpp           # Scalar vs SIMD Architecture-Aware benchmark
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
| **Scalar Engine (1 ARM64 Core)** | **214.48 MB / sec** | **1.71 Gbps** | **4.66 ns / byte** | Measured (Cold/Warm) |
| **Architecture-Aware NEON (VTBL + Prefetch)** | **526.61 MB / sec** | **4.21 Gbps** | **1.89 ns / byte** | Measured (2.46x Speedup) |
| **Multi-Threaded Stream (8 CPU Cores)** | **~4,210.00 MB / sec** | **33.68 Gbps** | **< 0.3 ns / byte** | Projected (Linear Scale) |
| **Server 16-Core Peak (AVX-512 / SVE2)** | **~9,600.00 MB / sec** | **76.80 Gbps** | **Vector Parallel** | Theoretical Estimate |

### Technical Optimization Highlights
1. **Register-Loaded VTBL Vector Lookups (`vqtbl1q_u8`):** Replaces bitwise XOR matrix math with single-cycle NEON table lookup instructions for 16 nibbles loaded directly into registers outside the loop.
2. **Hardware Cache Prefetching (`FAST_ECC_PREFETCH` / `PRFM`):** Preloads memory blocks 128-256 bytes ahead into L1 cache to eliminate DRAM access stalls.
3. **Loop Unrolling x4:** Processes 64 bytes (4 x 128-bit vector registers) per loop iteration to saturate execution pipelines.
4. **Pointer Aliasing Hints (`FAST_ECC_RESTRICT` / `__restrict`):** Informs compiler that source and destination memory regions do not overlap for aggressive instruction reordering.

## ⚙️ Supported Environment

- **Architectures**: ARM64 (Native NEON acceleration), x86-64 / x86 (Scalar fallback), ARMv7/v8.
- **Compilers**: MSVC 2019/2022/2026, GCC 7.0+, Clang 8.0+.
- **Thread Safety**: Thread-safe (All core primitives are stateless and thread-reentrant).
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
