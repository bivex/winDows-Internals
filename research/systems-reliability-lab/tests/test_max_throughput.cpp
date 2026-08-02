#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <cstdint>
#include "../include/ecc_branchless.hpp"

int main() {
    std::cout << "========================================================\n";
    std::cout << "      Fast Branchless ECC Peak Throughput Benchmark     \n";
    std::cout << "========================================================\n\n";

    const size_t BUFFER_SIZE = 64 * 1024 * 1024; // 64 MB Buffer
    std::cout << "[+] Allocating & Initializing " << (BUFFER_SIZE / (1024 * 1024)) << " MB Test Buffer...\n";

    std::vector<uint8_t> inputBuffer(BUFFER_SIZE);
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        inputBuffer[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // 1. Measure Encode Speed
    auto t0 = std::chrono::high_resolution_clock::now();
    auto encodedStream = ecc::FastHamming74::encodeBuffer(inputBuffer.data(), BUFFER_SIZE);
    auto t1 = std::chrono::high_resolution_clock::now();

    // 2. Measure Decode & Auto-Correction Speed over 64 MB
    int totalCorrections = 0;
    auto t2 = std::chrono::high_resolution_clock::now();
    auto decodedBuffer = ecc::FastHamming74::decodeBuffer(encodedStream, totalCorrections);
    auto t3 = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::milli> encodeMs = t1 - t0;
    std::chrono::duration<double, std::milli> decodeMs = t3 - t2;

    double encodeMBs = (BUFFER_SIZE / (1024.0 * 1024.0)) / (encodeMs.count() / 1000.0);
    double decodeMBs = (BUFFER_SIZE / (1024.0 * 1024.0)) / (decodeMs.count() / 1000.0);
    double decodeGbps = (decodeMBs * 8.0) / 1024.0; // Gigabits per second

    std::cout << "\n=================== PEAK THROUGHPUT RESULTS ===================\n";
    std::cout << " Buffer Size Tested     : " << (BUFFER_SIZE / (1024 * 1024)) << " MB (" << encodedStream.size() << " Codewords)\n";
    std::cout << " Encode Speed           : " << std::fixed << std::setprecision(2) << encodeMBs << " MB/sec (" << encodeMs.count() << " ms)\n";
    std::cout << " Decode & Correct Speed : " << std::fixed << std::setprecision(2) << decodeMBs << " MB/sec (" << decodeMs.count() << " ms)\n";
    std::cout << " Peak Network Throughput: " << std::fixed << std::setprecision(2) << decodeGbps << " Gbps (Gigabits/sec)\n";
    std::cout << "===============================================================\n";

    return 0;
}
