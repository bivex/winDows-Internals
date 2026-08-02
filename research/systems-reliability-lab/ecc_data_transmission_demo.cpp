#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>

// Kernel-Optimized Fast Branchless Hamming(7,4) Transmission Engine
class FastECCTransmitter {
public:
    // Encode 4 bits -> 7 bits
    static inline uint8_t encode4b(uint8_t d) {
        uint8_t d0 = (d >> 0) & 1;
        uint8_t d1 = (d >> 1) & 1;
        uint8_t d2 = (d >> 2) & 1;
        uint8_t d3 = (d >> 3) & 1;

        uint8_t p1 = d0 ^ d1 ^ d3;
        uint8_t p2 = d0 ^ d2 ^ d3;
        uint8_t p4 = d1 ^ d2 ^ d3;

        return (p1) | (p2 << 1) | (d0 << 2) | (p4 << 3) | (d1 << 4) | (d2 << 5) | (d3 << 6);
    }

    // Branchless Decode & Correct 7 bits -> 4 bits
    static inline uint8_t decode4b(uint8_t codeword, bool& corrected) {
        uint32_t c = codeword;
        uint32_t b1 = (c >> 0) & 1;
        uint32_t b2 = (c >> 1) & 1;
        uint32_t b3 = (c >> 2) & 1;
        uint32_t b4 = (c >> 3) & 1;
        uint32_t b5 = (c >> 4) & 1;
        uint32_t b6 = (c >> 5) & 1;
        uint32_t b7 = (c >> 6) & 1;

        uint32_t s1 = b1 ^ b3 ^ b5 ^ b7;
        uint32_t s2 = b2 ^ b3 ^ b6 ^ b7;
        uint32_t s4 = b4 ^ b5 ^ b6 ^ b7;

        uint32_t syndrome = (s4 << 2) | (s2 << 1) | s1;

        // Branchless Mask Fix
        uint32_t mask = (syndrome != 0) ? (1U << (syndrome - 1)) : 0U;
        corrected = (syndrome != 0);
        c ^= mask;

        return ((c >> 6) & 1) << 3 |
               ((c >> 5) & 1) << 2 |
               ((c >> 4) & 1) << 1 |
               ((c >> 2) & 1);
    }

    // Encode 1 Byte (8 bits) into 2 ECC Codewords (14 bits total)
    static std::pair<uint8_t, uint8_t> encodeByte(uint8_t byte) {
        uint8_t lowNibble = byte & 0x0F;
        uint8_t highNibble = (byte >> 4) & 0x0F;
        return { encode4b(lowNibble), encode4b(highNibble) };
    }

    // Decode 2 ECC Codewords -> 1 Byte
    static uint8_t decodeByte(uint8_t lowCode, uint8_t highCode, int& totalCorrections) {
        bool corrLow = false, corrHigh = false;
        uint8_t lowNibble = decode4b(lowCode, corrLow);
        uint8_t highNibble = decode4b(highCode, corrHigh);

        if (corrLow) totalCorrections++;
        if (corrHigh) totalCorrections++;

        return (highNibble << 4) | lowNibble;
    }
};

int main() {
    std::srand((unsigned int)std::time(NULL));

    std::cout << "=====================================================================\n";
    std::cout << "     Real-Time Data Transmission & ECC Self-Healing Engine           \n";
    std::cout << "=====================================================================\n\n";

    std::string originalText = "Hello, Windows Kernel Systems Reliability Lab!";
    std::cout << "[+] Original Message to Transmit (" << originalText.size() << " bytes):\n";
    std::cout << "    \"" << originalText << "\"\n\n";

    // 1. Encoding Payload
    std::vector<std::pair<uint8_t, uint8_t>> eccPackets;
    for (char ch : originalText) {
        eccPackets.push_back(FastECCTransmitter::encodeByte((uint8_t)ch));
    }
    std::cout << "[1] Encoded into " << eccPackets.size() * 2 << " ECC Codewords (Hamming 7,4).\n\n";

    // 2. Simulating Noisy Channel (Injecting Bit Flips)
    std::cout << "[!] SIMULATING NOISY HARDWARE CHANNEL (Injecting Bit Flips)...\n";
    std::vector<std::pair<uint8_t, uint8_t>> corruptedPackets = eccPackets;
    std::string corruptedRawText = originalText;

    int injectedErrors = 0;
    for (size_t i = 0; i < corruptedPackets.size(); ++i) {
        // Inject bit flip in low nibble codeword
        int bitPosLow = std::rand() % 7;
        corruptedPackets[i].first ^= (1 << bitPosLow);
        injectedErrors++;

        // Inject bit flip in high nibble codeword
        int bitPosHigh = std::rand() % 7;
        corruptedPackets[i].second ^= (1 << bitPosHigh);
        injectedErrors++;

        // Simulating what un-protected raw text looks like with byte corruptions
        corruptedRawText[i] ^= (1 << (std::rand() % 8));
    }
    std::cout << "    Injected " << injectedErrors << " single-bit errors in transmission stream.\n\n";

    // 3. Reception WITHOUT ECC Protection
    std::cout << "[2] Received Text WITHOUT ECC Protection (Corrupted Stream):\n";
    std::cout << "    \"" << corruptedRawText << "\"\n\n";

    // 4. Reception WITH Fast Branchless ECC Engine
    auto startTime = std::chrono::high_resolution_clock::now();

    std::string recoveredText = "";
    int totalCorrections = 0;
    for (const auto& pkt : corruptedPackets) {
        char restoredChar = (char)FastECCTransmitter::decodeByte(pkt.first, pkt.second, totalCorrections);
        recoveredText += restoredChar;
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::micro> elapsed = endTime - startTime;

    std::cout << "[3] Received Text WITH Branchless Fast ECC Protection:\n";
    std::cout << "    \"" << recoveredText << "\"\n\n";

    std::cout << "======================== RECOVERY STATS ========================\n";
    std::cout << " Total Errors Injected   : " << injectedErrors << "\n";
    std::cout << " Total Errors Corrected  : " << totalCorrections << "\n";
    std::cout << " Processing Time         : " << std::fixed << std::setprecision(2) << elapsed.count() << " us\n";
    std::cout << " Data Integrity Match    : " << (originalText == recoveredText ? "100% PERFECT MATCH!" : "FAILED") << "\n";
    std::cout << "================================================================\n";

    return 0;
}
