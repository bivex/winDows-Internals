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
│   └── test_edge_cases.cpp         # Edge cases & 1 MB high-res benchmark
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

## 📊 Performance & Benchmark (Windows 11 ARM64 Build 26100 MSVC /O2)

Empirical performance measurements collected on native ARM64 architecture:

| Operation / Metric | Performance Metric | Notes |
|---|---|---|
| **Single 4-bit Nibble ECC** | **4.16 ns / op** | 240M ops / sec (Branchless) |
| **Single 8-bit Byte ECC (2 Codewords)** | **8.32 ns / byte** | Full 2-nibble auto-correction |
| **1 MB Buffer Decode & Auto-Correction** | **~8.3 ms / MB** | **120+ MB/sec Throughput** |
| **1 MB Stream Auto-Corrections** | **2,000,000 bit flips** | 100% Data Restored |
| **CPU Branch Mispredictions** | **0** | Pure Bitwise Mask Manipulation |
| **Memory Allocation** | **0 bytes** | Header-Only Inline Operations |

## 🛠️ Building Examples & Tests

### Windows MSVC (ARM64 / x64)

```cmd
vcvarsall.bat arm64
cl /EHsc /O2 /Fe:test_edge_cases.exe tests\test_edge_cases.cpp
test_edge_cases.exe
```

### CMake Build (Cross-Platform)

```bash
mkdir build && cd build
cmake ..
cmake --build .
ctest --output-on-failure
```
