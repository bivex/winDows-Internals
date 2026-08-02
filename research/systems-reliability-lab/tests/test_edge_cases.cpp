#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <ctime>
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

    // Binary payload with null bytes, executable headers (MZ, ELF), and random binary
    uint8_t binaryData[] = { 0x7F, 'E', 'L', 'F', 0x00, 0x00, 0x01, 0x02, 0xFF, 0xFE, 0x00, 0xDEAD, 0xBEEF };
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

// 4. Test Large Stress Buffer (100,000 Bytes = 200,000 Codewords)
void testLargeStressBuffer() {
    std::cout << "[TEST] 4. Large Stress Buffer (100,000 bytes / 200,000 codewords)...\n";

    const size_t STRESS_SIZE = 100000;
    std::vector<uint8_t> stressBuffer(STRESS_SIZE);

    for (size_t i = 0; i < STRESS_SIZE; ++i) {
        stressBuffer[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto encodedStream = ecc::FastHamming74::encodeBuffer(stressBuffer.data(), STRESS_SIZE);
    assert(encodedStream.size() == STRESS_SIZE * 2);

    // Corrupt 1 random bit in every single codeword
    for (size_t i = 0; i < encodedStream.size(); ++i) {
        int bitPos = std::rand() % 7;
        encodedStream[i] ^= (1 << bitPos);
    }

    int totalCorrections = 0;
    auto restoredStream = ecc::FastHamming74::decodeBuffer(encodedStream, totalCorrections);

    assert(restoredStream.size() == STRESS_SIZE);
    assert(restoredStream == stressBuffer);
    assert(totalCorrections == (int)(STRESS_SIZE * 2));

    std::cout << " -> PASSED: 100,000 bytes (200,000 errors corrected) 100% verified!\n\n";
}

int main() {
    std::srand(42); // Deterministic seed

    std::cout << "========================================================\n";
    std::cout << "     ECC Branchless Library Edge Cases Test Suite       \n";
    std::cout << "========================================================\n\n";

    testExtremeBoundaries();
    testBinaryPayloads();
    testEmptyBuffers();
    testLargeStressBuffer();

    std::cout << ">>> ALL EDGE CASE TESTS PASSED SUCCESSFULLY! <<<\n";
    return 0;
}
