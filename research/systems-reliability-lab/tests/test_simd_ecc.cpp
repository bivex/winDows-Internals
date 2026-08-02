#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include "../include/ecc_branchless.hpp"

int main() {
    std::cout << "========================================================\n";
    std::cout << "   Scalar vs SIMD NEON Vectorized ECC Benchmark         \n";
    std::cout << "========================================================\n\n";

    const size_t BUFFER_SIZE = 32 * 1024 * 1024; // 32 MB Buffer
    std::cout << "[+] Allocating & Initializing " << (BUFFER_SIZE / (1024 * 1024)) << " MB Test Buffer...\n";

    std::vector<uint8_t> inputBuffer(BUFFER_SIZE);
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        inputBuffer[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto encodedStream = ecc::FastHamming74::encodeBuffer(inputBuffer.data(), BUFFER_SIZE);
    std::cout << "[+] Encoded Stream Size: " << (encodedStream.size() / (1024 * 1024)) << " MB (" << encodedStream.size() << " codewords)\n\n";

    // 1. Scalar Mode Benchmark (decodeBuffer)
    int dummyCorr = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    auto scalarDecoded = ecc::FastHamming74::decodeBuffer(encodedStream, dummyCorr);
    auto t1 = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::milli> scalarMs = t1 - t0;
    double scalarMBs = (BUFFER_SIZE / (1024.0 * 1024.0)) / (scalarMs.count() / 1000.0);

    // 2. SIMD NEON Vectorized Mode Benchmark (decodeBufferSIMD)
    auto t2 = std::chrono::high_resolution_clock::now();
    auto simdDecoded = ecc::FastHamming74::decodeBufferSIMD(encodedStream);
    auto t3 = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::milli> simdMs = t3 - t2;
    double simdMBs = (BUFFER_SIZE / (1024.0 * 1024.0)) / (simdMs.count() / 1000.0);

    // Verification
    bool match = (scalarDecoded == simdDecoded);

    double speedup = scalarMs.count() / simdMs.count();

    std::cout << "=================== SIMD BENCHMARK RESULTS ===================\n";
    std::cout << " Scalar Engine Speed : " << std::fixed << std::setprecision(2) << scalarMBs << " MB/sec (" << scalarMs.count() << " ms)\n";
    std::cout << " SIMD Vector Speed   : " << std::fixed << std::setprecision(2) << simdMBs << " MB/sec (" << simdMs.count() << " ms)\n";
    std::cout << " SIMD Speedup Factor : " << std::fixed << std::setprecision(2) << speedup << "x FASTER!\n";
    std::cout << " Output Verification : " << (match ? "100% PERFECT MATCH!" : "FAILED") << "\n";
    std::cout << "==============================================================\n";

    return 0;
}
