# Fast Branchless ECC Library (`ecc_branchless.hpp`)

High-performance, C++11 header-only library providing branchless Hamming(7,4) Single Error Correction (SEC) with sub-5ns latency.

## 📁 Directory Structure

```text
systems-reliability-lab/
├── include/
│   └── ecc_branchless.hpp         # Header-only library
├── examples/
│   ├── basic_ecc.cpp               # Basic single-nibble example
│   ├── stream_transmission.cpp     # Full string/buffer stream recovery
│   └── ecc_data_transmission_demo.cpp # Real-time noise simulation demo
├── tests/
│   └── test_ecc.cpp                # Automated unit test suite (112 test cases)
├── Systems_Reliability_Lab.md      # Architecture research document
└── Systems_Reliability_Lab.tex     # Academic LaTeX paper (Ukrainian)
```

## 🚀 Quick Start

Include `ecc_branchless.hpp` directly into your C++ project:

```cpp
#include "include/ecc_branchless.hpp"
#include <iostream>

int main() {
    // 1. Encode 4-bit data
    uint8_t codeword = ecc::FastHamming74::encode4(0b1011);

    // 2. Simulate bit flip (Bit #3)
    uint8_t corrupted = codeword ^ (1 << 2);

    // 3. Decode & Auto-Correct without branches
    bool corrected = false;
    uint8_t restored = ecc::FastHamming74::decode4(corrupted, corrected);

    std::cout << "Restored Payload: " << (int)restored << " (Corrected: " << (corrected ? "YES" : "NO") << ")\n";
    return 0;
}
```

## 🛠️ Building Examples & Tests

### Windows MSVC (ARM64 / x64)

```cmd
vcvarsall.bat arm64
cl /EHsc /O2 /Fe:test_ecc.exe tests\test_ecc.cpp
cl /EHsc /O2 /Fe:basic_ecc.exe examples\basic_ecc.cpp
cl /EHsc /O2 /Fe:stream_transmission.exe examples\stream_transmission.cpp

test_ecc.exe
```

### GCC / Clang (macOS / Linux)

```bash
clang++ -std=c++11 -O2 -Iinclude -o test_ecc tests/test_ecc.cpp
./test_ecc
```

## 📊 Performance Benchmark

| Metric | Result |
|---|---|
| **Latency per ECC operation** | **4.16 ns** |
| **Throughput** | **240,124,072 ops / sec** |
| **CPU Branch Mispredictions** | **0 (Branchless Execution)** |
