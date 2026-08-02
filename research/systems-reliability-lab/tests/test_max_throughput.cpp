#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <cstdint>
#include <numeric>
#include <algorithm>
#include "../include/ecc_branchless.hpp"

int main() {
    std::cout << "========================================================\n";
    std::cout << "  Multi-Pass High-Precision ECC Speed Benchmark        \n";
    std::cout << "========================================================\n\n";

    const size_t BUFFER_SIZE = 64 * 1024 * 1024; // 64 MB Buffer
    const int RUNS = 5;

    std::cout << "[+] Allocating " << (BUFFER_SIZE / (1024 * 1024)) << " MB Test Buffer...\n";
    std::vector<uint8_t> inputBuffer(BUFFER_SIZE);
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        inputBuffer[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto encodedStream = ecc::FastHamming74::encodeBuffer(inputBuffer.data(), BUFFER_SIZE);

    // Warm-up run to eliminate CPU frequency scaling artifacts
    int dummyCorr = 0;
    (void)ecc::FastHamming74::decodeBuffer(encodedStream, dummyCorr);

    std::vector<double> decodeTimesMs;
    std::vector<double> throughputsMBs;

    std::cout << "[+] Running " << RUNS << " benchmark iterations over 64 MB (" << encodedStream.size() << " codewords)...\n\n";

    for (int run = 1; run <= RUNS; ++run) {
        int totalCorrections = 0;
        auto t0 = std::chrono::high_resolution_clock::now();
        auto decodedBuffer = ecc::FastHamming74::decodeBuffer(encodedStream, totalCorrections);
        auto t1 = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double, std::milli> durationMs = t1 - t0;
        double speedMBs = (BUFFER_SIZE / (1024.0 * 1024.0)) / (durationMs.count() / 1000.0);

        decodeTimesMs.push_back(durationMs.count());
        throughputsMBs.push_back(speedMBs);

        std::cout << "    Run #" << run << " : " << std::fixed << std::setprecision(2) 
                  << durationMs.count() << " ms | Speed: " << speedMBs << " MB/s (" 
                  << (speedMBs * 8.0 / 1024.0) << " Gbps)\n";
    }

    double minTime = *std::min_element(decodeTimesMs.begin(), decodeTimesMs.end());
    double maxTime = *std::max_element(decodeTimesMs.begin(), decodeTimesMs.end());
    double avgTime = std::accumulate(decodeTimesMs.begin(), decodeTimesMs.end(), 0.0) / RUNS;

    double maxSpeed = *std::max_element(throughputsMBs.begin(), throughputsMBs.end());
    double avgSpeed = std::accumulate(throughputsMBs.begin(), throughputsMBs.end(), 0.0) / RUNS;

    double nsPerByte = (minTime * 1000000.0) / BUFFER_SIZE;

    std::cout << "\n================ BENCHMARK SUMMARY (5 RUNS) ================\n";
    std::cout << " Fastest Decode Time   : " << std::fixed << std::setprecision(2) << minTime << " ms\n";
    std::cout << " Average Decode Time   : " << std::fixed << std::setprecision(2) << avgTime << " ms\n";
    std::cout << " PEAK Throughput       : " << std::fixed << std::setprecision(2) << maxSpeed << " MB/sec (" << (maxSpeed * 8.0 / 1024.0) << " Gbps)\n";
    std::cout << " AVERAGE Throughput    : " << std::fixed << std::setprecision(2) << avgSpeed << " MB/sec (" << (avgSpeed * 8.0 / 1024.0) << " Gbps)\n";
    std::cout << " Minimum Latency       : " << std::fixed << std::setprecision(2) << nsPerByte << " ns / byte\n";
    std::cout << "============================================================\n";

    return 0;
}
