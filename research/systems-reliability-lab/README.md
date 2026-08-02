# Fast Branchless ECC & Kernel Lookaside Pool Library

High-performance, C++11 header-only libraries providing branchless Hamming(7,4) Single Error Correction (SEC) with software-pipelined ARM NEON acceleration and sub-5ns lock-free Kernel-Style Lookaside memory pools.

## 📁 Included Header-Only Libraries

1. **`include/ecc_branchless.hpp`**: Branchless Hamming(7,4) SEC encoder/decoder with ARM NEON 128-bit SIMD, software pipelining (Unroll x8), and register-loaded VTBL lookups.
2. **`include/kernel_lookaside.hpp`**: Lock-free S-List memory pool (`LockFreeLookasidePool`) and cache-line aligned ring buffer (`LockFreeRingBuffer`) inspired by Windows Kernel Lookaside Lists (`nt!ExAllocateFromNPagedLookasideList`).

## ✨ Features

- **Header-Only C++11 Libraries**: Zero external dependencies, single include.
- **Multi-Architecture Dispatch**: ARM64 NEON (128-bit SIMD), x86 AVX2 (256-bit SIMD), x86 SSE4.1, and Portable Scalar.
- **Software Pipelining & x8 Register Unrolling**: Process 128 codewords per loop iteration with interleaved instruction scheduling.
- **Sub-5ns Lock-Free Allocations**: `LockFreeLookasidePool` provides **4.10 ns/allocation** and **3.01 ns/deallocation** with 0 dynamic heap calls (`0 malloc/new`).
- **Cache-Line Alignment (`alignas(64)`)**: Prevents CPU False Sharing across multi-threaded operations.
- **Branchless Decoding Implementation**: Data-dependent branches eliminated from core error correction paths.
- **Register-Loaded VTBL Acceleration**: 1-cycle table lookups loaded into registers outside the loop (**463.79 MB/sec encoding**).
- **Comprehensive Test Suite**: Automated unit tests, edge-case checks, 0-leak MSVC CRT / ASan checks, and assembly dump inspection.

## 📁 Directory Structure

```text
fast-ecc-branchless/
├── include/
│   ├── ecc_branchless.hpp          # Header-only ECC library v2.2.0
│   └── kernel_lookaside.hpp        # Header-only Lock-Free Pool library v1.1.0
├── examples/
│   ├── basic_ecc.cpp               # Basic single-nibble example
│   ├── stream_transmission.cpp     # Full string/buffer stream recovery
│   ├── ecc_data_transmission_demo.cpp # Real-time noise channel demo
│   ├── fast_10mb_transmission_demo.cpp # High-speed 10 MB noise transmission
│   ├── lookaside_ecc_integration_demo.cpp # Lock-Free Pool + ECC Integration Demo
│   ├── ecc_hamming_poc.cpp         # Standard Hamming C++ PoC
│   └── ecc_hamming_fast_poc.cpp    # Branchless C++ PoC
├── tests/
│   ├── test_ecc.cpp                # Automated unit test suite (112 test cases)
│   ├── test_edge_cases.cpp         # Edge cases & 1 MB high-res benchmark
│   ├── test_memleak.cpp            # MSVC CRT & ASan 0-leak memory check
│   ├── test_max_throughput.cpp     # 64 MB Multi-Pass Peak Throughput benchmark
│   ├── test_simd_ecc.cpp           # Pipelined Unroll x8 NEON Benchmark
│   └── test_kernel_lookaside.cpp   # Lock-Free Lookaside Pool Test (8 threads)
├── docs/
│   ├── Systems_Reliability_Lab.md  # Systems Reliability Architecture research
│   └── Systems_Reliability_Lab.tex # Academic LaTeX paper (Ukrainian)
├── CMakeLists.txt                  # Cross-platform CMake build configuration
├── README.md                       # Documentation & Benchmark Breakdown
└── .gitignore                      # Git ignore rules
```

## 🚀 Quick Start (Kernel Lookaside Pool)

```cpp
#include "include/kernel_lookaside.hpp"
#include <iostream>

struct MyMessage {
    uint64_t id;
    char payload[64];
};

int main() {
    // Sub-5ns Lock-Free Lookaside Pool (65,536 slots)
    kernel::LockFreeLookasidePool<MyMessage, 65536> pool;

    // Allocate without heap malloc (4.10 ns latency)
    MyMessage* msg = pool.allocate();
    if (msg) {
        msg->id = 42;
        // ... process message ...
        pool.deallocate(msg); // 3.01 ns latency
    }
    return 0;
}
```

## 📊 Benchmark & Performance Qualification

> **Benchmarking Environment Disclaimer:**  
> Measured on **Windows 11 ARM64 (Build 26100, MSVC /O2)** running on Apple Silicon hypervisor. Benchmark timing results will vary depending on hardware architecture, compiler version, optimization flags, cache hierarchy, memory subsystem, and workload thermal throttling.

| Execution Mode / Scope | Throughput / Latency | Network Bitrate | Notes |
|---|---|---|---|
| **LockFreeLookasidePool Allocation** | **4.10 ns / alloc** | — | **0 Malloc / Zero Lock** |
| **LockFreeLookasidePool Deallocation** | **3.01 ns / free** | — | **0 Free / Atomic S-List** |
| **Register-Loaded VTBL Encoding** | **463.79 MB / sec** | **3.71 Gbps** | Peak VTBL NEON |
| **Pipelined Unroll x8 NEON Vector** | **325.94 MB / sec** | **2.55 Gbps** | **1.61x Speedup** |
| **Multi-Threaded Stream (8 CPU Cores)** | **~2,600.00 MB / sec** | **20.80 Gbps** | Projected (Linear Scale) |

## 🛠️ Building Examples & Tests

### Windows MSVC (ARM64 / x64)

```cmd
vcvarsall.bat arm64
cl /EHsc /O2 /Fe:test_kernel_lookaside.exe tests\test_kernel_lookaside.cpp
test_kernel_lookaside.exe
```

### CMake Build (Cross-Platform)

```bash
mkdir build && cd build
cmake ..
cmake --build .
ctest --output-on-failure
```
