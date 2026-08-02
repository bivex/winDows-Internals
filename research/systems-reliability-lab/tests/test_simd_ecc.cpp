#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include "../include/ecc_branchless.hpp"

int main() {
    std::cout << "========================================================\n";
    std::cout << "  Software Pipelined Unroll x8 ARM NEON Benchmark       \n";
    std::cout << "========================================================\n\n";

    std::cout << "[+] Detected Architecture: ";
    switch (ecc::FastHamming74::getDetectedArchitecture()) {
        case ecc::Architecture::ARM_NEON: std::cout << "ARM64 NEON (128-bit SIMD)\n"; break;
        case ecc::Architecture::x86_AVX2: std::cout << "x86 AVX2 (256-bit SIMD)\n"; break;
        case ecc::Architecture::x86_SSE41: std::cout << "x86 SSE4.1 (128-bit SIMD)\n"; break;
        default: std::cout << "Portable C++ Scalar\n"; break;
    }

    const size_t BUFFER_SIZE = 64 * 1024 * 1024; // 64 MB Buffer
    std::cout << "[+] Allocating & Initializing " << (BUFFER_SIZE / (1024 * 1024)) << " MB Test Buffer...\n";

    std::vector<uint8_t> inputBuffer(BUFFER_SIZE);
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        inputBuffer[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // 1. Measure Register-Loaded VTBL Encoding Speed
    auto t0 = std::chrono::high_resolution_clock::now();
    auto encodedStream = ecc::FastHamming74::encodeBuffer(inputBuffer.data(), BUFFER_SIZE);
    auto t1 = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::milli> encodeMs = t1 - t0;
    double encodeMBs = (BUFFER_SIZE / (1024.0 * 1024.0)) / (encodeMs.count() / 1000.0);

    // 2. Measure Scalar Decoding Speed
    int dummyCorr = 0;
    auto t2 = std::chrono::high_resolution_clock::now();
    auto scalarDecoded = ecc::FastHamming74::decodeBuffer(encodedStream, dummyCorr);
    auto t3 = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::milli> scalarMs = t3 - t2;
    double scalarMBs = (BUFFER_SIZE / (1024.0 * 1024.0)) / (scalarMs.count() / 1000.0);

    // 3. Measure Unroll x8 Software Pipelined SIMD Decoding Speed
    auto t4 = std::chrono::high_resolution_clock::now();
    auto simdDecoded = ecc::FastHamming74::decodeBufferPipelinedx8(encodedStream);
    auto t5 = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::milli> simdMs = t5 - t4;
    double simdMBs = (BUFFER_SIZE / (1024.0 * 1024.0)) / (simdMs.count() / 1000.0);

    // Verification
    bool match = (scalarDecoded == simdDecoded);
    double speedup = scalarMs.count() / simdMs.count();

    std::cout << "\n=================== PIPELINED BENCHMARK RESULTS ===================\n";
    std::cout << " Register-Loaded VTBL Encoding : " << std::fixed << std::setprecision(2) << encodeMBs << " MB/sec (" << encodeMs.count() << " ms)\n";
    std::cout << " Standard Scalar Decoding       : " << std::fixed << std::setprecision(2) << scalarMBs << " MB/sec (" << scalarMs.count() << " ms)\n";
    std::cout << " Pipelined Unroll x8 NEON Vector: " << std::fixed << std::setprecision(2) << simdMBs << " MB/sec (" << simdMs.count() << " ms)\n";
    std::cout << " NEON Vector Bitrate            : " << std::fixed << std::setprecision(2) << (simdMBs * 8.0 / 1024.0) << " Gbps\n";
    std::cout << " Speedup vs Scalar              : " << std::fixed << std::setprecision(2) << speedup << "x FASTER!\n";
    std::cout << " Data Integrity Verification    : " << (match ? "100% PERFECT MATCH!" : "FAILED") << "\n";
    std::cout << "===================================================================\n";

    return 0;
}
