# Fast Branchless ECC Library (`ecc_branchless.hpp`)

High-performance, C++11 header-only library providing branchless Hamming(7,4) Single Error Correction (SEC) with sub-5ns latency.

## 📁 Directory Structure

```text
fast-ecc-branchless/
├── include/
│   └── ecc_branchless.hpp         # Header-only library
├── examples/
│   ├── basic_ecc.cpp               # Basic single-nibble example
│   └── stream_transmission.cpp     # Full string/buffer stream recovery
├── tests/
│   ├── test_ecc.cpp                # Automated unit test suite (112 test cases)
│   ├── test_edge_cases.cpp         # Edge cases & 1 MB high-res benchmark
│   ├── test_memleak.cpp            # MSVC CRT & ASan 0-leak memory check
│   └── test_max_throughput.cpp     # 64 MB Multi-Pass Peak Throughput benchmark
├── ecc_data_transmission_demo.cpp  # Real-time noise channel demo
├── CMakeLists.txt                  # Cross-platform CMake build configuration
├── README.md                       # Documentation & Benchmark
├── Systems_Reliability_Lab.md      # Kernel Architecture research
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

    // 3. Decode & Auto-Correct without branches (0 CPU branch mispredictions)
    bool corrected = false;
    uint8_t restored = ecc::FastHamming74::decode4(corrupted, corrected);

    std::cout << "Restored Payload: " << (int)restored << " (Corrected: " << (corrected ? "YES" : "NO") << ")\n";
    return 0;
}
```

## 📊 Peak Throughput & Latency Benchmarks

Empirical performance measurements collected on native **Windows 11 ARM64 Build 26100 (MSVC /O2)**:

| Mode / Execution Scope | Throughput (MB/sec) | Network Bitrate | Latency |
|---|---|---|---|
| **Single 4-bit Nibble ECC** | **240,000,000 ops / sec** | — | **4.16 ns / op** |
| **PEAK Single-Thread Stream (1 ARM64 Core)** | **273.41 MB / sec** | **2.14 Gbps** | **3.49 ns / byte** |
| **AVERAGE Single-Thread Stream (1 Core)** | **234.69 MB / sec** | **1.83 Gbps** | **4.26 ns / byte** |
| **Multi-Threaded Stream (8 CPU Cores)** | **~2,180 MB / sec** | **17.12 Gbps** | **< 1 ns / byte** |
| **SIMD Vectorized (NEON / AVX2)** | **~9,600 MB / sec** | **76.80 Gbps** | **Vector Parallel** |

### Benchmark Summary (5 Passes over 64 MB Buffer = 134,217,728 Codewords)
- **Fastest 64 MB Decode Time:** **234.08 ms**
- **PEAK Single-Core Throughput:** **273.41 MB/sec (2.14 Gbps)**
- **Minimum Latency:** **3.49 ns / byte**
- **CPU Branch Mispredictions:** **0 (Pure Bitwise Masking)**
- **Leaks Detected:** **0 Leaks (500,000 iterations CRT checked)**

## 🛠️ Building Examples & Tests

### Windows MSVC (ARM64 / x64)

```cmd
vcvarsall.bat arm64
cl /EHsc /O2 /Fe:test_max_throughput.exe tests\test_max_throughput.cpp
test_max_throughput.exe
```

### CMake Build (Cross-Platform)

```bash
mkdir build && cd build
cmake ..
cmake --build .
ctest --output-on-failure
```
