#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include "../include/ecc_branchless.hpp"

int main() {
    std::cout << "========================================================\n";
    std::cout << "  Kernel-Max (VTBL + Prefetch + Unroll x4) Benchmark    \n";
    std::cout << "========================================================\n\n";

    const size_t BUFFER_SIZE = 64 * 1024 * 1024; // 64 MB Buffer
    std::cout << "[+] Allocating & Initializing " << (BUFFER_SIZE / (1024 * 1024)) << " MB Test Buffer...\n";

    std::vector<uint8_t> inputBuffer(BUFFER_SIZE);
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        inputBuffer[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // 1. Measure 1-Cycle VTBL NEON Vector Encoding Speed
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

    // 3. Measure Kernel-Max Unrolled SIMD Decoding Speed (decodeBufferKernelMax)
    auto t4 = std::chrono::high_resolution_clock::now();
    auto kernelMaxDecoded = ecc::FastHamming74::decodeBufferKernelMax(encodedStream);
    auto t5 = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::milli> kernelMaxMs = t5 - t4;
    double kernelMaxMBs = (BUFFER_SIZE / (1024.0 * 1024.0)) / (kernelMaxMs.count() / 1000.0);

    // Verification
    bool match = (scalarDecoded == kernelMaxDecoded);
    double speedup = scalarMs.count() / kernelMaxMs.count();

    std::cout << "\n=================== KERNEL-MAX BENCHMARK RESULTS ===================\n";
    std::cout << " 1-Cycle VTBL Encoding Speed : " << std::fixed << std::setprecision(2) << encodeMBs << " MB/sec (" << encodeMs.count() << " ms)\n";
    std::cout << " Standard Scalar Decoding    : " << std::fixed << std::setprecision(2) << scalarMBs << " MB/sec (" << scalarMs.count() << " ms)\n";
    std::cout << " Kernel-Max Vector Decoding  : " << std::fixed << std::setprecision(2) << kernelMaxMBs << " MB/sec (" << kernelMaxMs.count() << " ms)\n";
    std::cout << " Kernel Vector Bitrate       : " << std::fixed << std::setprecision(2) << (kernelMaxMBs * 8.0 / 1024.0) << " Gbps\n";
    std::cout << " Speedup vs Scalar           : " << std::fixed << std::setprecision(2) << speedup << "x FASTER!\n";
    std::cout << " Data Integrity Verification : " << (match ? "100% PERFECT MATCH!" : "FAILED") << "\n";
    std::cout << "===================================================================\n";

    return 0;
}
