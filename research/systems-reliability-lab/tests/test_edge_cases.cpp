#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <iomanip>
#include "../include/ecc_branchless.hpp"

// 1. Test All-Zeros and All-Ones Boundary Nibbles and Bytes
void testExtremeBoundaries() {
    std::cout << "[TEST] 1. Extreme Boundaries (0x00, 0xFF, 0x55, 0xAA)...\n";

    uint8_t boundaryBytes[] = { 0x00, 0xFF, 0x55, 0xAA, 0x0F, 0xF0, 0x01, 0x80 };

    for (uint8_t original : boundaryBytes) {
        auto pair = ecc::FastHamming74::encodeByte(original);

        // Test clean roundtrip
        int correctionsClean = 0;
        uint8_t decodedClean = ecc::FastHamming74::decodeByte(pair.first, pair.second, correctionsClean);
        assert(decodedClean == original);
        assert(correctionsClean == 0);

        // Test low-nibble bit flip
        uint8_t lowCorrupted = pair.first ^ (1 << 3);
        int corrections1 = 0;
        uint8_t decoded1 = ecc::FastHamming74::decodeByte(lowCorrupted, pair.second, corrections1);
        assert(decoded1 == original);
        assert(corrections1 == 1);

        // Test high-nibble bit flip
        uint8_t highCorrupted = pair.second ^ (1 << 6);
        int corrections2 = 0;
        uint8_t decoded2 = ecc::FastHamming74::decodeByte(pair.first, highCorrupted, corrections2);
        assert(decoded2 == original);
        assert(corrections2 == 1);
    }

    std::cout << " -> PASSED: Extreme boundary tests succeeded!\n\n";
}

// 2. Test Binary Data with Embedded Null Bytes (0x00) & Non-ASCII Payload
void testBinaryPayloads() {
    std::cout << "[TEST] 2. Binary Data Stream with Embedded Null Bytes...\n";

    uint8_t binaryData[] = { 0x7F, 'E', 'L', 'F', 0x00, 0x00, 0x01, 0x02, 0xFF, 0xFE, 0x00, 0xDE, 0xAD, 0xBE, 0xEF };
    size_t dataLen = sizeof(binaryData);

    auto encoded = ecc::FastHamming74::encodeBuffer(binaryData, dataLen);
    assert(encoded.size() == dataLen * 2);

    // Corrupt 1 bit in every codeword
    for (size_t i = 0; i < encoded.size(); ++i) {
        encoded[i] ^= (1 << (i % 7));
    }

    int corrections = 0;
    auto decodedVec = ecc::FastHamming74::decodeBuffer(encoded, corrections);

    assert(decodedVec.size() == dataLen);
    assert(std::memcmp(decodedVec.data(), binaryData, dataLen) == 0);
    assert(corrections == (int)encoded.size());

    std::cout << " -> PASSED: Binary stream with null bytes 100% restored!\n\n";
}

// 3. Test Empty Buffer and 0-Length Edge Cases
void testEmptyBuffers() {
    std::cout << "[TEST] 3. Empty & 0-Length Buffer Edge Cases...\n";

    std::vector<uint8_t> emptyVec;
    auto encodedEmpty = ecc::FastHamming74::encodeBuffer(emptyVec.data(), 0);
    assert(encodedEmpty.empty());

    int corrections = 0;
    auto decodedEmpty = ecc::FastHamming74::decodeBuffer(encodedEmpty, corrections);
    assert(decodedEmpty.empty());
    assert(corrections == 0);

    std::string emptyStr = "";
    auto encodedStr = ecc::FastHamming74::encodeString(emptyStr);
    assert(encodedStr.empty());
    std::string decodedStr = ecc::FastHamming74::decodeString(encodedStr, corrections);
    assert(decodedStr.empty());

    std::cout << " -> PASSED: Empty buffer edge cases handled safely!\n\n";
}

// 4. Test Large Stress Buffer with Microsecond Latency Benchmark
void testLargeStressBufferWithBenchmark() {
    std::cout << "[TEST] 4. Stress Buffer Benchmark (1,000,000 bytes / 2,000,000 codewords)...\n";

    const size_t STRESS_SIZE = 1000000; // 1 MB
    std::vector<uint8_t> stressBuffer(STRESS_SIZE);

    for (size_t i = 0; i < STRESS_SIZE; ++i) {
        stressBuffer[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Measure Encoding Speed
    auto t1 = std::chrono::high_resolution_clock::now();
    auto encodedStream = ecc::FastHamming74::encodeBuffer(stressBuffer.data(), STRESS_SIZE);
    auto t2 = std::chrono::high_resolution_clock::now();
    assert(encodedStream.size() == STRESS_SIZE * 2);

    // Corrupt 1 random bit in every single codeword
    for (size_t i = 0; i < encodedStream.size(); ++i) {
        int bitPos = std::rand() % 7;
        encodedStream[i] ^= (1 << bitPos);
    }

    // Measure Decoding & Auto-Correction Speed
    auto t3 = std::chrono::high_resolution_clock::now();
    int totalCorrections = 0;
    auto restoredStream = ecc::FastHamming74::decodeBuffer(encodedStream, totalCorrections);
    auto t4 = std::chrono::high_resolution_clock::now();

    assert(restoredStream.size() == STRESS_SIZE);
    assert(restoredStream == stressBuffer);
    assert(totalCorrections == (int)(STRESS_SIZE * 2));

    std::chrono::duration<double, std::milli> encodeTime = t2 - t1;
    std::chrono::duration<double, std::milli> decodeTime = t4 - t3;
    double nsPerByte = (decodeTime.count() * 1000000.0) / STRESS_SIZE;
    double mbPerSec = (STRESS_SIZE / (1024.0 * 1024.0)) / (decodeTime.count() / 1000.0);

    std::cout << " -> PASSED: 1,000,000 bytes (2,000,000 bit flips corrected) 100% verified!\n";
    std::cout << "    [BENCHMARK] Encode Time    : " << std::fixed << std::setprecision(2) << encodeTime.count() << " ms\n";
    std::cout << "    [BENCHMARK] Decode Time    : " << std::fixed << std::setprecision(2) << decodeTime.count() << " ms\n";
    std::cout << "    [BENCHMARK] Latency        : " << std::fixed << std::setprecision(2) << nsPerByte << " ns / byte\n";
    std::cout << "    [BENCHMARK] Throughput     : " << std::fixed << std::setprecision(2) << mbPerSec << " MB / sec\n\n";
}

int main() {
    std::srand(42); // Deterministic seed

    std::cout << "========================================================\n";
    std::cout << "     ECC Branchless Library Edge Cases & Benchmark     \n";
    std::cout << "========================================================\n\n";

    testExtremeBoundaries();
    testBinaryPayloads();
    testEmptyBuffers();
    testLargeStressBufferWithBenchmark();

    std::cout << ">>> ALL EDGE CASE TESTS & BENCHMARKS PASSED! <<<\n";
    return 0;
}
