#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include "../include/ecc_branchless.hpp"

void testAllSingleBitFlips() {
    std::cout << "[TEST 1] Testing all 16 nibbles with all 7 bit-flip positions...\n";
    int passCount = 0;

    for (uint8_t nibble = 0; nibble < 16; ++nibble) {
        uint8_t codeword = ecc::FastHamming74::encode4(nibble);

        // Test clean decode
        bool correctedClean = false;
        uint8_t cleanDecoded = ecc::FastHamming74::decode4(codeword, correctedClean);
        assert(cleanDecoded == nibble);
        assert(!correctedClean);

        // Test each of the 7 bit flip positions
        for (int bitPos = 0; bitPos < 7; ++bitPos) {
            uint8_t corrupted = codeword ^ (1 << bitPos);
            bool corrected = false;
            uint8_t restored = ecc::FastHamming74::decode4(corrupted, corrected);
            assert(restored == nibble);
            assert(corrected == true);
            passCount++;
        }
    }

    std::cout << " -> PASSED: " << passCount << " single-bit error recovery tests passed!\n\n";
}

void testBufferTransmission() {
    std::cout << "[TEST 2] Testing buffer roundtrip encoding/decoding...\n";
    std::string testStr = "The quick brown fox jumps over the lazy dog 0123456789!@#$%^&*()";

    auto encodedStream = ecc::FastHamming74::encodeString(testStr);

    // Corrupt 1 bit in every byte's codewords
    for (size_t i = 0; i < encodedStream.size(); ++i) {
        encodedStream[i] ^= (1 << (i % 7));
    }

    int corrections = 0;
    std::string decodedStr = ecc::FastHamming74::decodeString(encodedStream, corrections);

    assert(decodedStr == testStr);
    assert(corrections == (int)encodedStream.size());

    std::cout << " -> PASSED: Buffer roundtrip 100% verified with " << corrections << " corrections!\n\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "      Fast Branchless ECC Automated Test Suite          \n";
    std::cout << "========================================================\n\n";

    testAllSingleBitFlips();
    testBufferTransmission();

    std::cout << ">>> ALL UNIT TESTS PASSED SUCCESSFULLY! <<<\n";
    return 0;
}
